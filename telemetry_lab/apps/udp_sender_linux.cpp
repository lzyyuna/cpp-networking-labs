
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char* argv[])
{
    // 1. Validate command-line arguments
    if (argc != 3)
    {
        std::cerr
            << "Usage: telemetry_udp_sender "
            << "<packets_per_second> <duration_ms>\n";
        return 1;
    }

    int rate = 0;
    int duration_ms = 0;

    try
    {
        std::size_t pos = 0;

        rate = std::stoi(argv[1], &pos);

        if (pos != std::string(argv[1]).size() ||
            rate < 1 || rate > 10000)
        {
            throw std::invalid_argument("invalid rate");
        }

        duration_ms = std::stoi(argv[2], &pos);

        if (pos != std::string(argv[2]).size() ||
            duration_ms < 1 || duration_ms > 600000)
        {
            throw std::invalid_argument("invalid duration");
        }
    }
    catch (const std::exception&)
    {
        std::cerr
            << "Invalid arguments.\n"
            << "Rate: 1-10000 packets/s\n"
            << "Duration: 1-600000 ms\n";
        return 1;
    }

    // 2. Create a UDP socket
    int fd = socket(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP
    );

    if (fd == -1)
    {
        const int error = errno;

        std::cerr << "socket failed: "
                  << std::strerror(error) << '\n';
        return 1;
    }

    // 3. Configure destination address
    sockaddr_in destination{};

    destination.sin_family = AF_INET;
    destination.sin_port = htons(9000);
    if (inet_pton(
        AF_INET,
        "10.29.213.237",
        &destination.sin_addr) != 1)
{
    std::cerr << "Invalid destination IP\n";
    return 1;
}
    // 4. Configure sending interval
    using Clock = std::chrono::steady_clock;

    const auto interval =
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / rate)
        );

    const auto start = Clock::now();

    const auto deadline =
        start + std::chrono::milliseconds(duration_ms);

    auto next_send = start;

    long long seq = 1;
    long long sent_count = 0;
    bool success = true;

    std::cout << "Linux UDP sender started\n";
    std::cout << "Target rate: "
              << rate << " packets/s\n";
    std::cout << "Duration: "
              << duration_ms << " ms\n\n";

    // 5. Send UDP datagrams until the deadline
    while (Clock::now() < deadline)
    {
        std::this_thread::sleep_until(next_send);

        if (Clock::now() >= deadline)
        {
            break;
        }

        // Generate a sample telemetry value
        int value = static_cast<int>(
            (seq % 1000) * 10
        );

        // Message format: seq,value
        std::string message =
            std::to_string(seq) + "," +
            std::to_string(value);

        // Send one UDP datagram
        ssize_t result = sendto(
            fd,
            message.data(),
            message.size(),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination)
        );

        if (result == -1)
        {
            const int error = errno;

            std::cerr << "sendto failed: "
                      << std::strerror(error) << '\n';

            success = false;
            break;
        }

        ++sent_count;

        // Print only selected packets
        if (sent_count <= 5 || sent_count % 1000 == 0)
        {
            std::cout << "Sent: seq=" << seq
                      << ", value=" << value << '\n';
        }

        ++seq;

        // Schedule the next packet
        next_send += interval;
    }

    // 6. Calculate sending statistics
    const auto end = Clock::now();

    const double elapsed_seconds =
        std::chrono::duration<double>(
            end - start
        ).count();

    std::cout << "\n--- Sender Summary ---\n";

    std::cout << "Sent packets: "
              << sent_count << '\n';

    std::cout << "Elapsed time: "
              << elapsed_seconds << " s\n";

    std::cout << "Actual send rate: "
              << sent_count / elapsed_seconds
              << " packets/s\n";

    // 7. Release the socket resource
    close(fd);

    return success ? 0 : 1;
}
