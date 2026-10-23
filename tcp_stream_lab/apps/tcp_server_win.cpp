#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr unsigned short kDefaultPort = 9001;
constexpr std::size_t kMaxMessageBytes = 4096;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
constexpr auto kHeartbeatTimeout = std::chrono::seconds(4);

std::mutex output_mutex;

void print_line(const std::string& text) {
    const std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << text << '\n';
}

void print_error(const std::string& text) {
    const std::lock_guard<std::mutex> lock(output_mutex);
    std::cerr << text << '\n';
}

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed with error " + std::to_string(result));
        }
    }

    ~WinsockRuntime() { WSACleanup(); }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class Socket {
public:
    explicit Socket(SOCKET socket) noexcept : socket_(socket) {}

    ~Socket() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }

private:
    SOCKET socket_;
};

[[noreturn]] void throw_socket_error(const std::string& operation) {
    const int error = WSAGetLastError();
    throw std::runtime_error(operation + " failed with WSA error " + std::to_string(error));
}

void send_all(SOCKET socket, const char* data, std::size_t size) {
    std::size_t total_sent = 0;
    while (total_sent < size) {
        const std::size_t remaining = size - total_sent;
        const int chunk_size = static_cast<int>(std::min(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int sent = send(socket, data + total_sent, chunk_size, 0);

        if (sent == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
            throw_socket_error("send");
        }
        if (sent == 0) {
            throw std::runtime_error("send returned 0 before the frame was complete");
        }
        total_sent += static_cast<std::size_t>(sent);
    }
}

bool receive_exact(SOCKET socket, char* data, std::size_t size) {
    std::size_t total_received = 0;
    while (total_received < size) {
        const std::size_t remaining = size - total_received;
        const int chunk_size = static_cast<int>(std::min(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int received = recv(socket, data + total_received, chunk_size, 0);

        if (received == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
            throw_socket_error("recv");
        }
        if (received == 0) {
            if (total_received == 0) {
                return false;
            }
            throw std::runtime_error("connection closed in the middle of a frame");
        }
        total_received += static_cast<std::size_t>(received);
    }
    return true;
}

void send_frame(SOCKET socket, const std::string& message) {
    if (message.size() > kMaxMessageBytes) {
        throw std::runtime_error("outgoing message exceeds the 4096-byte limit");
    }

    const auto message_size = static_cast<std::uint32_t>(message.size());
    const std::uint32_t network_size = htonl(message_size);
    send_all(socket,
             reinterpret_cast<const char*>(&network_size),
             sizeof(network_size));
    send_all(socket, message.data(), message.size());
}

bool receive_frame(SOCKET socket, std::string& message) {
    std::uint32_t network_size = 0;
    if (!receive_exact(socket,
                       reinterpret_cast<char*>(&network_size),
                       sizeof(network_size))) {
        return false;
    }

    const std::uint32_t message_size = ntohl(network_size);
    if (message_size > kMaxMessageBytes) {
        throw std::runtime_error("incoming message exceeds the 4096-byte limit");
    }

    message.resize(message_size);
    if (message_size > 0 &&
        !receive_exact(socket, message.data(), static_cast<std::size_t>(message_size))) {
        throw std::runtime_error("connection closed before the frame body arrived");
    }
    return true;
}

class CommandQueue {
public:
    void push(std::string command) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            commands_.push_back(std::move(command));
        }
        condition_.notify_one();
    }

    bool wait_pop(std::string& command, const std::atomic<bool>& session_stopped) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] {
            return session_stopped.load() || !commands_.empty() || closed_;
        });

        if (session_stopped.load()) {
            return false;
        }
        if (commands_.empty()) {
            return false;
        }

        command = std::move(commands_.front());
        commands_.pop_front();
        return true;
    }

    void close() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

    void notify_all() { condition_.notify_all(); }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::string> commands_;
    bool closed_ = false;
};

long long steady_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool is_valid_hello(const std::string& message, std::string& robot_id) {
    constexpr char prefix[] = "HELLO|";
    if (message.rfind(prefix, 0) != 0) {
        return false;
    }

    robot_id = message.substr(sizeof(prefix) - 1);
    return !robot_id.empty() && robot_id.find('|') == std::string::npos;
}

unsigned short parse_port(const std::string& text) {
    std::size_t parsed_characters = 0;
    int value = 0;
    try {
        value = std::stoi(text, &parsed_characters);
    } catch (const std::exception&) {
        throw std::runtime_error("port must be an integer");
    }

    if (parsed_characters != text.size() || value <= 0 || value > 65535) {
        throw std::runtime_error("port must be in the range 1..65535");
    }
    return static_cast<unsigned short>(value);
}

void read_console(CommandQueue& commands, std::atomic<bool>& shutdown_requested) {
    print_line("Commands:");
    print_line("  START|<task-id>|<duration-seconds>  (tcp_stream_lab client)");
    print_line("  START|<task-id>|<x>|<y>|<tolerance>|<timeout-sec>  (LinkGuard bridge)");
    print_line("  STATUS or STATUS|<task-id>");
    print_line("  STOP|<task-id>");
    print_line("  PROFILE|<good|congested|disconnected>  (LinkGuard bridge)");
    print_line("  QUIT");

    bool quit_received = false;
    std::string command;
    while (true) {
        {
            const std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << "command> " << std::flush;
        }

        if (!std::getline(std::cin, command)) {
            break;
        }
        if (!command.empty() && command.back() == '\r') {
            command.pop_back();
        }
        if (command.empty()) {
            continue;
        }

        commands.push(command);
        if (command == "QUIT") {
            quit_received = true;
            break;
        }
    }

    if (!quit_received) {
        commands.push("QUIT");
    }
    shutdown_requested.store(true);
    commands.close();
}

void run_session(SOCKET client, CommandQueue& commands) {
    std::string hello;
    if (!receive_frame(client, hello)) {
        print_line("Client disconnected before the handshake.");
        return;
    }

    std::string robot_id;
    if (!is_valid_hello(hello, robot_id)) {
        send_frame(client, "ERROR|HANDSHAKE|EXPECTED_HELLO");
        throw std::runtime_error("invalid handshake: " + hello);
    }

    send_frame(client, "WELCOME|" + robot_id);
    print_line("Robot registered: " + robot_id);

    std::atomic<bool> session_stopped{false};
    std::atomic<long long> last_received_ms{steady_milliseconds()};
    std::mutex send_mutex;
    std::mutex heartbeat_mutex;
    std::condition_variable heartbeat_condition;

    const auto stop_session = [&] {
        if (!session_stopped.exchange(true)) {
            shutdown(client, SD_BOTH);
        }
        commands.notify_all();
        heartbeat_condition.notify_all();
    };

    std::thread receiver([&] {
        try {
            while (!session_stopped.load()) {
                std::string message;
                if (!receive_frame(client, message)) {
                    print_line("Linux client disconnected.");
                    break;
                }

                last_received_ms.store(steady_milliseconds());
                if (message.rfind("PONG|", 0) == 0) {
                    continue;
                }

                print_line("robot> " + message);
                if (message.rfind("BYE|", 0) == 0) {
                    break;
                }
            }
        } catch (const std::exception& error) {
            if (!session_stopped.load()) {
                print_error(std::string("Receive error: ") + error.what());
            }
        }
        stop_session();
    });

    std::thread sender([&] {
        std::string command;
        while (commands.wait_pop(command, session_stopped)) {
            try {
                const std::lock_guard<std::mutex> lock(send_mutex);
                send_frame(client, command);
            } catch (const std::exception& error) {
                print_error(std::string("Command send error: ") + error.what());
                stop_session();
                break;
            }

            if (command == "QUIT") {
                break;
            }
        }
    });

    std::thread heartbeat([&] {
        std::uint64_t sequence = 1;
        std::unique_lock<std::mutex> lock(heartbeat_mutex);

        while (!session_stopped.load()) {
            if (heartbeat_condition.wait_for(lock,
                                             kHeartbeatInterval,
                                             [&] { return session_stopped.load(); })) {
                break;
            }

            const long long silent_ms = steady_milliseconds() - last_received_ms.load();
            if (silent_ms >
                std::chrono::duration_cast<std::chrono::milliseconds>(kHeartbeatTimeout)
                    .count()) {
                print_error("Heartbeat timeout: robot is offline.");
                stop_session();
                break;
            }

            try {
                const std::lock_guard<std::mutex> send_lock(send_mutex);
                send_frame(client, "PING|" + std::to_string(sequence++));
            } catch (const std::exception& error) {
                print_error(std::string("Heartbeat send error: ") + error.what());
                stop_session();
                break;
            }
        }
    });

    receiver.join();
    stop_session();
    sender.join();
    heartbeat.join();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [listen-port]\n";
        return 2;
    }

    try {
        const unsigned short listen_port =
            argc == 2 ? parse_port(argv[1]) : kDefaultPort;
        const WinsockRuntime winsock;

        const Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (listener.get() == INVALID_SOCKET) {
            throw_socket_error("socket");
        }

        const BOOL reuse_address = TRUE;
        if (setsockopt(listener.get(),
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse_address),
                       sizeof(reuse_address)) == SOCKET_ERROR) {
            throw_socket_error("setsockopt(SO_REUSEADDR)");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(listen_port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR) {
            throw_socket_error("bind");
        }
        if (listen(listener.get(), SOMAXCONN) == SOCKET_ERROR) {
            throw_socket_error("listen");
        }

        print_line("Listening on 0.0.0.0:" + std::to_string(listen_port));

        CommandQueue commands;
        std::atomic<bool> shutdown_requested{false};
        std::thread console_thread;
        bool console_started = false;

        while (!shutdown_requested.load()) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listener.get(), &read_set);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200'000;

            const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) {
                print_error("select failed with WSA error " +
                            std::to_string(WSAGetLastError()));
                continue;
            }
            if (ready == 0) {
                continue;
            }

            sockaddr_in peer{};
            int peer_size = sizeof(peer);
            const Socket client(accept(listener.get(),
                                       reinterpret_cast<sockaddr*>(&peer),
                                       &peer_size));
            if (client.get() == INVALID_SOCKET) {
                print_error("accept failed with WSA error " +
                            std::to_string(WSAGetLastError()));
                continue;
            }

            char peer_ip[INET_ADDRSTRLEN]{};
            const char* printed_ip = inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
            print_line("Linux client connected from " +
                       std::string(printed_ip != nullptr ? printed_ip : "unknown") + ':' +
                       std::to_string(ntohs(peer.sin_port)));

            if (!console_started) {
                console_thread =
                    std::thread(read_console, std::ref(commands), std::ref(shutdown_requested));
                console_started = true;
            }

            try {
                run_session(client.get(), commands);
            } catch (const std::exception& error) {
                print_error(std::string("Session ended: ") + error.what());
            }

            if (!shutdown_requested.load()) {
                print_line("Waiting for the Linux robot to reconnect...");
            }
        }

        if (console_thread.joinable()) {
            console_thread.join();
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
