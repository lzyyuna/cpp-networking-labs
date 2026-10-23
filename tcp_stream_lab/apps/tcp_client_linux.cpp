#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kDefaultServerIp = "10.29.213.237";
constexpr unsigned short kDefaultServerPort = 9001;
constexpr const char* kRobotId = "robot01";
constexpr std::size_t kMaxMessageBytes = 4096;
constexpr auto kReconnectDelay = std::chrono::seconds(2);

class Socket {
public:
    explicit Socket(int descriptor) noexcept : descriptor_(descriptor) {}

    ~Socket() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

[[noreturn]] void throw_system_error(const std::string& operation) {
    const int error = errno;
    throw std::runtime_error(operation + " failed: " + std::strerror(error));
}

void send_all(int socket, const char* data, std::size_t size) {
    std::size_t total_sent = 0;
    while (total_sent < size) {
        const ssize_t sent = send(socket,
                                  data + total_sent,
                                  size - total_sent,
                                  MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error("send");
        }
        if (sent == 0) {
            throw std::runtime_error("send returned 0 before the frame was complete");
        }
        total_sent += static_cast<std::size_t>(sent);
    }
}

bool receive_exact(int socket, char* data, std::size_t size) {
    std::size_t total_received = 0;
    while (total_received < size) {
        const ssize_t received = recv(socket,
                                      data + total_received,
                                      size - total_received,
                                      0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error("recv");
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

void send_frame(int socket, const std::string& message) {
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

bool receive_frame(int socket, std::string& message) {
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

std::vector<std::string> split_fields(const std::string& message) {
    std::vector<std::string> fields;
    std::size_t field_begin = 0;

    while (true) {
        const std::size_t separator = message.find('|', field_begin);
        if (separator == std::string::npos) {
            fields.push_back(message.substr(field_begin));
            return fields;
        }
        fields.push_back(message.substr(field_begin, separator - field_begin));
        field_begin = separator + 1;
    }
}

int parse_positive_integer(const std::string& text, const std::string& field_name) {
    std::size_t parsed_characters = 0;
    int value = 0;
    try {
        value = std::stoi(text, &parsed_characters);
    } catch (const std::exception&) {
        throw std::runtime_error(field_name + " must be an integer");
    }

    if (parsed_characters != text.size() || value <= 0) {
        throw std::runtime_error(field_name + " must be a positive integer");
    }
    return value;
}

enum class RobotState {
    Idle,
    Running,
    Stopped,
    Completed,
};

const char* state_name(RobotState state) {
    switch (state) {
    case RobotState::Idle:
        return "IDLE";
    case RobotState::Running:
        return "RUNNING";
    case RobotState::Stopped:
        return "STOPPED";
    case RobotState::Completed:
        return "COMPLETED";
    }
    return "UNKNOWN";
}

struct CommandResult {
    std::string response;
    bool close_after_send = false;
};

class RobotSimulator {
public:
    CommandResult handle(const std::string& command) {
        const std::vector<std::string> fields = split_fields(command);
        if (fields.empty() || fields.front().empty()) {
            return {"ERROR|COMMAND|EMPTY", false};
        }

        if (fields[0] == "START") {
            return handle_start(fields);
        }
        if (fields[0] == "STATUS") {
            return handle_status(fields);
        }
        if (fields[0] == "STOP") {
            return handle_stop(fields);
        }
        if (fields[0] == "QUIT" && fields.size() == 1) {
            return {"BYE|" + std::string(kRobotId), true};
        }
        return {"ERROR|COMMAND|UNKNOWN|" + fields[0], false};
    }

private:
    CommandResult handle_start(const std::vector<std::string>& fields) {
        if (fields.size() != 3 || fields[1].empty()) {
            return {"ERROR|START|USAGE_START_TASKID_DURATION", false};
        }

        refresh_state();
        if (state_ == RobotState::Running) {
            return {"ERROR|" + fields[1] + "|BUSY_WITH|" + task_id_, false};
        }

        int duration_seconds = 0;
        try {
            duration_seconds = parse_positive_integer(fields[2], "duration");
        } catch (const std::exception& error) {
            return {"ERROR|" + fields[1] + "|" + error.what(), false};
        }
        if (duration_seconds > 3600) {
            return {"ERROR|" + fields[1] + "|DURATION_TOO_LARGE", false};
        }

        task_id_ = fields[1];
        duration_seconds_ = duration_seconds;
        started_at_ = std::chrono::steady_clock::now();
        state_ = RobotState::Running;
        return {"ACK|" + task_id_ + "|STARTED", false};
    }

    CommandResult handle_status(const std::vector<std::string>& fields) {
        if (fields.size() > 2) {
            return {"ERROR|STATUS|USAGE_STATUS_OPTIONAL_TASKID", false};
        }

        refresh_state();
        if (state_ == RobotState::Idle) {
            return {"STATUS|NONE|IDLE|0", false};
        }
        if (fields.size() == 2 && fields[1] != task_id_) {
            return {"ERROR|" + fields[1] + "|TASK_NOT_FOUND", false};
        }

        return {"STATUS|" + task_id_ + '|' + state_name(state_) + '|' +
                    std::to_string(progress_percent()),
                false};
    }

    CommandResult handle_stop(const std::vector<std::string>& fields) {
        if (fields.size() != 2 || fields[1].empty()) {
            return {"ERROR|STOP|USAGE_STOP_TASKID", false};
        }

        refresh_state();
        if (state_ == RobotState::Idle || fields[1] != task_id_) {
            return {"ERROR|" + fields[1] + "|TASK_NOT_FOUND", false};
        }
        if (state_ == RobotState::Running) {
            state_ = RobotState::Stopped;
        }
        return {"RESULT|" + task_id_ + '|' + state_name(state_), false};
    }

    void refresh_state() {
        if (state_ != RobotState::Running) {
            return;
        }

        const auto elapsed = std::chrono::steady_clock::now() - started_at_;
        if (elapsed >= std::chrono::seconds(duration_seconds_)) {
            state_ = RobotState::Completed;
        }
    }

    int progress_percent() const {
        if (state_ == RobotState::Idle) {
            return 0;
        }
        if (state_ == RobotState::Completed) {
            return 100;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started_at_)
                                 .count();
        const long long duration_ms = static_cast<long long>(duration_seconds_) * 1000;
        const long long calculated = duration_ms > 0 ? elapsed * 100 / duration_ms : 0;
        if (calculated < 0) {
            return 0;
        }
        if (calculated > 99) {
            return 99;
        }
        return static_cast<int>(calculated);
    }

    RobotState state_ = RobotState::Idle;
    std::string task_id_;
    int duration_seconds_ = 0;
    std::chrono::steady_clock::time_point started_at_{};
};

unsigned short parse_port(const std::string& text) {
    const int port = parse_positive_integer(text, "port");
    if (port > 65535) {
        throw std::runtime_error("port must be in the range 1..65535");
    }
    return static_cast<unsigned short>(port);
}

enum class SessionOutcome {
    Reconnect,
    Quit,
};

SessionOutcome run_connected_session(int socket, RobotSimulator& robot) {
    send_frame(socket, "HELLO|" + std::string(kRobotId));

    std::string welcome;
    if (!receive_frame(socket, welcome)) {
        throw std::runtime_error("Windows server closed during the handshake");
    }
    if (welcome != "WELCOME|" + std::string(kRobotId)) {
        throw std::runtime_error("unexpected handshake response: " + welcome);
    }
    std::cout << "Handshake complete: " << welcome << '\n';

    while (true) {
        std::string command;
        if (!receive_frame(socket, command)) {
            std::cout << "Windows server closed the connection.\n";
            return SessionOutcome::Reconnect;
        }

        if (command.rfind("PING|", 0) == 0) {
            send_frame(socket, "PONG|" + command.substr(5));
            continue;
        }

        std::cout << "command> " << command << '\n';
        const CommandResult result = robot.handle(command);
        send_frame(socket, result.response);
        std::cout << "response> " << result.response << '\n';

        if (result.close_after_send) {
            return SessionOutcome::Quit;
        }
    }
}

SessionOutcome connect_and_run(const char* server_ip,
                               unsigned short server_port,
                               RobotSimulator& robot) {
    const Socket socket_handle(socket(AF_INET, SOCK_STREAM, 0));
    if (socket_handle.get() < 0) {
        throw_system_error("socket");
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);

    const int parse_result = inet_pton(AF_INET, server_ip, &server.sin_addr);
    if (parse_result == 0) {
        throw std::runtime_error(std::string("invalid IPv4 address: ") + server_ip);
    }
    if (parse_result < 0) {
        throw_system_error("inet_pton");
    }

    std::cout << "Connecting to " << server_ip << ':' << server_port << "...\n";
    if (connect(socket_handle.get(),
                reinterpret_cast<const sockaddr*>(&server),
                sizeof(server)) < 0) {
        throw_system_error("connect");
    }

    std::cout << "Connected. Sending robot handshake.\n";
    return run_connected_session(socket_handle.get(), robot);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "Usage: " << argv[0] << " [windows-ipv4-address] [port]\n";
        return 2;
    }

    const char* server_ip = argc >= 2 ? argv[1] : kDefaultServerIp;

    try {
        const unsigned short server_port =
            argc == 3 ? parse_port(argv[2]) : kDefaultServerPort;
        RobotSimulator robot;

        while (true) {
            try {
                const SessionOutcome outcome =
                    connect_and_run(server_ip, server_port, robot);
                if (outcome == SessionOutcome::Quit) {
                    break;
                }
            } catch (const std::exception& error) {
                std::cerr << "Connection error: " << error.what() << '\n';
            }

            std::cout << "Reconnecting in " << kReconnectDelay.count()
                      << " seconds. Press Ctrl+C to stop.\n";
            std::this_thread::sleep_for(kReconnectDelay);
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
