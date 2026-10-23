
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "telemetry/bounded_queue.hpp"
#include "telemetry/statistics.hpp"

// ==================================================
// 1. SIGINT handling
// ==================================================

// Only the main thread reads this flag.
// The signal handler sets it when Ctrl+C is pressed.
volatile std::sig_atomic_t g_sigint_requested = 0;

extern "C" void handle_sigint(int)
{
    // Keep signal handlers minimal.
    g_sigint_requested = 1;
}

void install_sigint_handler()
{
    struct sigaction action{};

    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (::sigaction(SIGINT, &action, nullptr) == -1)
    {
        throw std::runtime_error(
            "sigaction failed: " +
            std::string(std::strerror(errno))
        );
    }
}

// ==================================================
// 2. RAII socket wrapper
// ==================================================

class SocketFd
{
public:
    explicit SocketFd(int fd) noexcept
        : fd_(fd)
    {
    }

    ~SocketFd()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(const SocketFd&) = delete;

    int get() const noexcept
    {
        return fd_;
    }

private:
    int fd_;
};

// ==================================================
// 3. Utility functions
// ==================================================

int parse_duration(const std::string& text)
{
    if (text.empty() ||
        text.find_first_not_of("0123456789")
            != std::string::npos)
    {
        throw std::invalid_argument("invalid duration");
    }

    int value = std::stoi(text);

    if (value < 1 || value > 600000)
    {
        throw std::out_of_range("duration out of range");
    }

    return value;
}

// Expected format: seq,value
bool parse_message(
    const char* data,
    ssize_t length,
    long long& sequence,
    double& value
)
{
    std::string message(
        data,
        static_cast<std::size_t>(length)
    );

    std::istringstream input(message);

    char separator = '\0';

    if (!(input >> sequence >> separator >> value) ||
        separator != ',' ||
        sequence <= 0 ||
        !std::isfinite(value) ||
        value < 0.0)
    {
        return false;
    }

    input >> std::ws;

    return input.eof();
}

// ==================================================
// 4. Main
// ==================================================

int main(int argc, char* argv[])
{
    // No argument: run until Ctrl+C.
    // With argument: stop after duration_ms.
    int duration_ms = 0;

    if (argc > 2)
    {
        std::cerr
            << "Usage: telemetry_udp_receiver "
            << "[duration_ms]\n";
        return 1;
    }

    try
    {
        if (argc == 2)
        {
            duration_ms = parse_duration(argv[1]);
        }

        // Register Ctrl+C handler
        install_sigint_handler();

        // ------------------------------------------
        // Create UDP socket
        // ------------------------------------------

        SocketFd sock(
            ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        );

        if (sock.get() == -1)
        {
            throw std::runtime_error(
                "socket failed: " +
                std::string(std::strerror(errno))
            );
        }

        // Listen on 127.0.0.1:9000
        sockaddr_in address{};

        address.sin_family = AF_INET;
        address.sin_port = htons(9000);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(
                sock.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == -1)
        {
            throw std::runtime_error(
                "bind failed: " +
                std::string(std::strerror(errno))
            );
        }

        // Receive timeout: 200 ms
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        if (::setsockopt(
                sock.get(),
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)) == -1)
        {
            throw std::runtime_error(
                "setsockopt failed: " +
                std::string(std::strerror(errno))
            );
        }

        // ------------------------------------------
        // Shared resources
        // ------------------------------------------

        BoundedQueue queue(64);

        std::vector<double> values;

        std::atomic<bool> stop_requested{false};

        std::exception_ptr producer_error;
        std::exception_ptr consumer_error;

        std::size_t received_packets = 0;
        std::size_t valid_packets = 0;
        std::size_t invalid_packets = 0;

        std::cout << "Linux UDP receiver started\n";
        std::cout << "Listening on 127.0.0.1:9000\n";
        std::cout << "Socket FD: " << sock.get() << '\n';

        if (duration_ms == 0)
        {
            std::cout << "Press Ctrl+C to stop.\n\n";
        }
        else
        {
            std::cout << "Duration: "
                      << duration_ms << " ms\n";
            std::cout << "Ctrl+C can stop early.\n\n";
        }

        // ------------------------------------------
        // Block SIGINT before creating worker threads.
        // Workers inherit the blocked signal mask.
        // Main will unblock SIGINT after creation.
        // ------------------------------------------

        sigset_t sigint_set{};
        sigemptyset(&sigint_set);
        sigaddset(&sigint_set, SIGINT);

        int mask_error = ::pthread_sigmask(
            SIG_BLOCK, &sigint_set, nullptr
        );

        if (mask_error != 0)
        {
            throw std::runtime_error(
                "pthread_sigmask failed: " +
                std::string(std::strerror(mask_error))
            );
        }

        // ------------------------------------------
        // Consumer thread
        // ------------------------------------------

        std::thread consumer([&]()
        {
            try
            {
                while (auto value = queue.pop())
                {
                    values.push_back(*value);
                }
            }
            catch (...)
            {
                consumer_error = std::current_exception();
                queue.close();
            }
        });

        // ------------------------------------------
        // Producer thread
        // ------------------------------------------

        std::thread producer;

        try
        {
            producer = std::thread([&]()
            {
                try
                {
                    while (!stop_requested.load())
                    {
                        char buffer[1024];

                        ssize_t bytes = ::recvfrom(
                            sock.get(),
                            buffer,
                            sizeof(buffer),
                            0,
                            nullptr,
                            nullptr
                        );

                        if (bytes == -1)
                        {
                            const int error = errno;

                            // Timeout: retry and check stop flag.
                            if (error == EAGAIN ||
                                error == EWOULDBLOCK)
                            {
                                continue;
                            }

                            // Interrupted system call.
                            if (error == EINTR)
                            {
                                continue;
                            }

                            throw std::runtime_error(
                                "recvfrom failed: " +
                                std::string(std::strerror(error))
                            );
                        }

                        ++received_packets;

                        long long sequence = 0;
                        double value = 0.0;

                        if (!parse_message(
                                buffer,
                                bytes,
                                sequence,
                                value))
                        {
                            ++invalid_packets;
                            continue;
                        }

                        queue.push(value);
                        ++valid_packets;

                        if (valid_packets <= 5 ||
                            valid_packets % 1000 == 0)
                        {
                            std::cout
                                << "Received: seq="
                                << sequence
                                << ", value="
                                << value << '\n';
                        }
                    }
                }
                catch (...)
                {
                    producer_error = std::current_exception();
                }

                // Producer has stopped.
                // Consumer can drain remaining values.
                queue.close();
            });
        }
        catch (...)
        {
            stop_requested.store(true);
            queue.close();
            consumer.join();
            throw;
        }

        // Only the main thread receives SIGINT.
        mask_error = ::pthread_sigmask(
            SIG_UNBLOCK, &sigint_set, nullptr
        );

        if (mask_error != 0)
        {
            stop_requested.store(true);
            producer.join();
            consumer.join();

            throw std::runtime_error(
                "pthread_sigmask failed: " +
                std::string(std::strerror(mask_error))
            );
        }

        // ------------------------------------------
        // Main thread: wait for Ctrl+C or deadline
        // ------------------------------------------

        using Clock = std::chrono::steady_clock;

        const auto start = Clock::now();

        while (!g_sigint_requested)
        {
            if (duration_ms > 0 &&
                Clock::now() - start >=
                    std::chrono::milliseconds(duration_ms))
            {
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(50)
            );
        }

        // ------------------------------------------
        // Graceful shutdown
        // ------------------------------------------

        if (g_sigint_requested)
        {
            std::cout << "\nSIGINT received. "
                      << "Stopping gracefully...\n";
        }
        else
        {
            std::cout << "\nDuration reached. "
                      << "Stopping gracefully...\n";
        }

        // Tell producer to exit its receive loop.
        stop_requested.store(true);

        // Producer closes queue after stopping.
        producer.join();

        // Consumer drains the remaining queue.
        consumer.join();

        // ------------------------------------------
        // Check worker errors
        // ------------------------------------------

        if (producer_error)
        {
            std::rethrow_exception(producer_error);
        }

        if (consumer_error)
        {
            std::rethrow_exception(consumer_error);
        }

        // ------------------------------------------
        // Final statistics
        // ------------------------------------------

        std::cout << "\n--- Receiver Summary ---\n";

        std::cout << "UDP received: "
                  << received_packets << '\n';

        std::cout << "valid: "
                  << valid_packets << '\n';

        std::cout << "invalid: "
                  << invalid_packets << '\n';

        std::cout << "processed: "
                  << values.size() << '\n';

        if (!values.empty())
        {
            const Statistics stats =
                calculate_statistics(values);

            std::cout << "\n--- Statistics ---\n";

            std::cout << "count: "
                      << stats.count << '\n';

            std::cout << "min:   "
                      << stats.min << '\n';

            std::cout << "max:   "
                      << stats.max << '\n';

            std::cout << "mean:  "
                      << stats.mean << '\n';

            std::cout << "p50:   "
                      << stats.p50 << '\n';

            std::cout << "p95:   "
                      << stats.p95 << '\n';

            std::cout << "p99:   "
                      << stats.p99 << '\n';
        }
        else
        {
            std::cout << "No valid data.\n";
        }

        std::cout << "\nReceiver exited normally.\n";

        // SocketFd destructor closes the socket.
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Receiver error: "
                  << e.what() << '\n';

        return 1;
    }
}
