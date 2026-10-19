
#include <winsock2.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main()
{
    // 1. Initialize Winsock
    WSADATA wsa{};

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // 2. Create a UDP socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == INVALID_SOCKET)
    {
        std::cerr << "socket failed\n";
        WSACleanup();
        return 1;
    }

    // 3. Configure the destination address
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(9000);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    constexpr int kTotalPackets = 5;

    // Set true to skip sequence number 3
    constexpr bool kSkipSeq3 = false;

    bool success = true;

    // 4. Send UDP datagrams
    for (int seq = 1; seq <= kTotalPackets; ++seq)
    {
        if (kSkipSeq3 && seq == 3)
        {
            std::cout << "Skipped seq=3\n";
            continue;
        }

        int value = seq * 10;

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

        std::cout << "Sent: seq=" << seq
                  << ", value=" << value << '\n';

        std::this_thread::sleep_for(
            std::chrono::milliseconds(300)
        );
    }

    // 5. Release network resources
    closesocket(sock);
    WSACleanup();

    return success ? 0 : 1;
}
