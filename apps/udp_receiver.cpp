#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "telemetry/bounded_queue.hpp"
#include "telemetry/statistics.hpp"

namespace {

constexpr int kPort = 9000;
constexpr std::size_t kQueueCapacity = 64;
constexpr DWORD kReceiveTimeoutMs = 200;

std::atomic<std::atomic<bool>*> gConsoleStopRequested{nullptr};

BOOL WINAPI handle_console_event(DWORD event)
{
    std::atomic<bool>* const stop_requested =
        gConsoleStopRequested.load();

    if ((event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
        && stop_requested != nullptr)
    {
        stop_requested->store(true);
        return TRUE;
    }
    return FALSE;
}

class WinsockRuntime
{
public:
    WinsockRuntime()
    {
        WSADATA wsa{};
        const int result = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (result != 0)
        {
            throw std::runtime_error(
                "WSAStartup failed: " + std::to_string(result));
        }
    }

    ~WinsockRuntime() { WSACleanup(); }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class Socket
{
public:
    explicit Socket(SOCKET value) : value_(value)
    {
        if (value_ == INVALID_SOCKET)
        {
            throw std::runtime_error(
                "socket failed: " + std::to_string(WSAGetLastError()));
        }
    }

    ~Socket() { closesocket(value_); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const { return value_; }

private:
    SOCKET value_;
};

class ConsoleStopHandler
{
public:
    explicit ConsoleStopHandler(std::atomic<bool>& stop_requested)
    {
        gConsoleStopRequested.store(&stop_requested);
        if (!SetConsoleCtrlHandler(handle_console_event, TRUE))
        {
            gConsoleStopRequested.store(nullptr);
            throw std::runtime_error("cannot install Ctrl+C handler");
        }
    }

    ~ConsoleStopHandler()
    {
        SetConsoleCtrlHandler(handle_console_event, FALSE);
        gConsoleStopRequested.store(nullptr);
    }

    ConsoleStopHandler(const ConsoleStopHandler&) = delete;
    ConsoleStopHandler& operator=(const ConsoleStopHandler&) = delete;
};

std::chrono::milliseconds parse_duration(const std::string& text)
{
    if (text.empty() ||
        text.find_first_not_of("0123456789") != std::string::npos)
    {
        throw std::invalid_argument(
            "duration_ms must be a positive integer");
    }

    const unsigned long long value = std::stoull(text);
    const auto maximum = static_cast<unsigned long long>(
        std::numeric_limits<std::chrono::milliseconds::rep>::max());

    if (value == 0 || value > maximum)
    {
        throw std::invalid_argument(
            "duration_ms must be a positive integer");
    }
    return std::chrono::milliseconds(value);
}

bool parse_message(
    const char* data, int length, long long& sequence, double& value)
{
    const std::string message(data, static_cast<std::size_t>(length));
    std::istringstream input(message);
    char separator = '\0';

    if (!(input >> sequence >> separator >> value)
        || separator != ','
        || sequence <= 0
        || !std::isfinite(value)
        || value < 0.0)
    {
        return false;
    }

    input >> std::ws;
    return input.eof();
}

void print_statistics(const std::vector<double>& values)
{
    if (values.empty())
    {
        std::cout << "No valid data.\n";
        return;
    }

    const Statistics stats = calculate_statistics(values);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n--- Statistics ---\n";
    std::cout << "count: " << stats.count << '\n';
    std::cout << "min:   " << stats.min << '\n';
    std::cout << "max:   " << stats.max << '\n';
    std::cout << "mean:  " << stats.mean << '\n';
    std::cout << "p50:   " << stats.p50 << '\n';
    std::cout << "p95:   " << stats.p95 << '\n';
    std::cout << "p99:   " << stats.p99 << '\n';
}

// Analyze one sender run whose sequence starts from 1.
// Missing count only covers 1..largest sequence observed.
void print_sequence_statistics(const std::vector<long long>& arrivals)
{
    std::cout << "\n--- UDP Sequence Statistics ---\n";

    if (arrivals.empty())
    {
        std::cout << "No valid sequences received.\n";
        return;
    }

    std::unordered_set<long long> seen;
    seen.reserve(arrivals.size());

    std::size_t duplicates = 0;
    std::size_t out_of_order = 0;
    long long highest_seen = 0;

    for (long long sequence : arrivals)
    {
        if (!seen.insert(sequence).second)
        {
            ++duplicates;
            continue;
        }

        // Count late-arriving unique packets, not duplicates.
        if (sequence < highest_seen)
        {
            ++out_of_order;
        }

        highest_seen = std::max(highest_seen, sequence);
    }

    std::vector<long long> sorted(seen.begin(), seen.end());
    std::sort(sorted.begin(), sorted.end());

    std::uint64_t missing = 0;
    long long previous = 0;
    std::vector<std::pair<long long, long long>> missing_ranges;

    for (long long sequence : sorted)
    {
        if (sequence > previous + 1)
        {
            missing += static_cast<std::uint64_t>(
                sequence - previous - 1);

            // Avoid flooding the console with large loss reports.
            if (missing_ranges.size() < 10)
            {
                missing_ranges.emplace_back(
                    previous + 1, sequence - 1);
            }
        }
        previous = sequence;
    }

    std::cout << "Valid packets:       " << arrivals.size() << '\n';
    std::cout << "Unique sequences:    " << seen.size() << '\n';
    std::cout << "Duplicate packets:   " << duplicates << '\n';
    std::cout << "Out-of-order packets:" << ' ' << out_of_order << '\n';
    std::cout << "Highest sequence:    " << highest_seen << '\n';
    std::cout << "Missing (1..highest):" << ' ' << missing << '\n';

    for (const auto& range : missing_ranges)
    {
        std::cout << "Missing range: " << range.first;
        if (range.first != range.second)
        {
            std::cout << "-" << range.second;
        }
        std::cout << '\n';
    }

    if (missing_ranges.size() == 10)
    {
        std::cout << "(Missing range display is limited to 10 ranges.)\n";
    }
    std::cout << "Note: Missing packets after the highest received "
                 "sequence cannot be detected here.\n";
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 2)
    {
        std::cerr << "Usage: telemetry_udp_receiver [duration_ms]\n";
        return 1;
    }

    try
    {
        const std::optional<std::chrono::milliseconds> duration =
            argc == 2
                ? std::optional<std::chrono::milliseconds>(
                      parse_duration(argv[1]))
                : std::nullopt;

        WinsockRuntime winsock;
        Socket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));

        const DWORD timeout = kReceiveTimeoutMs;
        if (setsockopt(
                socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout),
                sizeof(timeout)) == SOCKET_ERROR)
        {
            throw std::runtime_error(
                "setsockopt failed: " + std::to_string(WSAGetLastError()));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(
                socket.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR)
        {
            throw std::runtime_error(
                "bind failed: " + std::to_string(WSAGetLastError()));
        }

        BoundedQueue queue(kQueueCapacity);
        std::vector<double> values;
        std::vector<long long> received_sequences;
        received_sequences.reserve(60000);

        std::size_t valid_packets = 0;
        std::size_t invalid_packets = 0;
        std::atomic<bool> stop_requested{false};
        ConsoleStopHandler console_handler(stop_requested);

        std::exception_ptr producer_error;
        std::exception_ptr consumer_error;

        std::thread producer([&]
        {
            try
            {
                while (!stop_requested.load())
                {
                    char buffer[1024];

                    const int bytes = recvfrom(
                        socket.get(), buffer,
                        static_cast<int>(sizeof(buffer)),
                        0, nullptr, nullptr);

                    if (bytes == SOCKET_ERROR)
                    {
                        const int error = WSAGetLastError();

                        if (error == WSAETIMEDOUT)
                        {
                            continue;
                        }

                        if (error == WSAEMSGSIZE)
                        {
                            ++invalid_packets;
                            continue;
                        }

                        if (stop_requested.load() && error == WSAEINTR)
                        {
                            break;
                        }

                        throw std::runtime_error(
                            "recvfrom failed: " + std::to_string(error));
                    }

                    long long sequence = 0;
                    double value = 0.0;

                    if (!parse_message(
                            buffer, bytes, sequence, value))
                    {
                        ++invalid_packets;
                        continue;
                    }

                    queue.push(value);
                    received_sequences.push_back(sequence);
                    ++valid_packets;

                    if (valid_packets <= 5 || valid_packets % 1000 == 0)
                    {
                        std::cout << "Received: seq=" << sequence
                                  << ", value=" << value << '\n';
                    }
                }
            }
            catch (...)
            {
                producer_error = std::current_exception();
                stop_requested.store(true);
            }

            // pop() still drains already queued values after close().
            queue.close();
        });

        std::thread consumer;

        try
        {
            consumer = std::thread([&]
            {
                try
                {
                    while (const auto value = queue.pop())
                    {
                        values.push_back(*value);
                    }
                }
                catch (...)
                {
                    consumer_error = std::current_exception();
                    stop_requested.store(true);
                    queue.close();
                }
            });
        }
        catch (...)
        {
            stop_requested.store(true);
            queue.close();
            producer.join();
            throw;
        }

        std::cout << "UDP receiver started on 0.0.0.0:"
                  << kPort << ". Press Ctrl+C to stop.\n";

        const auto start = std::chrono::steady_clock::now();

        while (!stop_requested.load())
        {
            if (duration.has_value()
                && std::chrono::steady_clock::now() - start >= *duration)
            {
                stop_requested.store(true);
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(25));
        }

        producer.join();
        consumer.join();

        if (consumer_error)
        {
            std::rethrow_exception(consumer_error);
        }

        if (producer_error)
        {
            std::rethrow_exception(producer_error);
        }

        std::cout << "\nReceiver stopped.\n";
        std::cout << "valid: " << valid_packets << '\n';
        std::cout << "invalid: " << invalid_packets << '\n';
        print_statistics(values);
        print_sequence_statistics(received_sequences);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Pipeline error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
