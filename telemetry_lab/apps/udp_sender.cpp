
#include <winsock2.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <stdexcept>

int main(int argc, char* argv[])
{
    // 1. Parse command-line arguments
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

    // 2. Initialize Winsock
    WSADATA wsa{};

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // 3. Create a UDP socket
    SOCKET sock = socket(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP
    );

    if (sock == INVALID_SOCKET)
    {
        std::cerr << "socket failed\n";
        WSACleanup();
        return 1;
    }

    // 4. Configure destination address
    sockaddr_in destination{};

    destination.sin_family = AF_INET;
    destination.sin_port = htons(9000);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // 5. Configure sending schedule
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

    std::cout << "UDP sender started\n";
    std::cout << "Target rate: "
              << rate << " packets/s\n";
    std::cout << "Duration: "
              << duration_ms << " ms\n\n";

    // 6. Send packets until the deadline
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

        int result = sendto(
            sock,
            message.data(),
            static_cast<int>(message.size()),
            0,
            reinterpret_cast<sockaddr*>(&destination),
            sizeof(destination)
        );

        if (result == SOCKET_ERROR)
        {
            std::cerr << "sendto failed: "
                      << WSAGetLastError() << '\n';

            success = false;
            break;
        }

        ++sent_count;

        // Avoid printing every packet
        if (sent_count <= 5 || sent_count % 1000 == 0)
        {
            std::cout << "Sent: seq=" << seq
                      << ", value=" << value << '\n';
        }

        ++seq;
        next_send += interval;
    }

    // 7. Calculate sending statistics
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

    // 8. Release network resources
    closesocket(sock);
    WSACleanup();

    return success ? 0 : 1;
}
