#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr unsigned short kListenPort = 19001;
constexpr std::size_t kMaxMessageBytes = 4096;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
constexpr auto kHeartbeatTimeout = std::chrono::seconds(4);
constexpr UINT kNetworkEvent = WM_APP + 1;

constexpr int kTaskEdit = 101;
constexpr int kXEdit = 102;
constexpr int kYEdit = 103;
constexpr int kToleranceEdit = 104;
constexpr int kTimeoutEdit = 105;
constexpr int kStartButton = 201;
constexpr int kStopButton = 202;
constexpr int kStatusButton = 203;
constexpr int kClearButton = 204;
constexpr int kProfileGoodButton = 205;
constexpr int kProfileCongestedButton = 206;
constexpr int kProfileDisconnectedButton = 207;
constexpr int kConnectionLabel = 301;
constexpr int kMissionLabel = 302;
constexpr int kLogEdit = 303;
constexpr int kNetworkProfileLabel = 304;

enum class UiEventType {
    Listening,
    Connected,
    Disconnected,
    Message,
    Error,
};

struct UiEvent {
    UiEventType type;
    std::string text;
};

std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        required);
    return result;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8,
                                              0,
                                              text.data(),
                                              static_cast<int>(text.size()),
                                              nullptr,
                                              0,
                                              nullptr,
                                              nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        required,
                        nullptr,
                        nullptr);
    return result;
}

[[noreturn]] void throw_socket_error(const std::string& operation) {
    throw std::runtime_error(
        operation + " failed with WSA error " + std::to_string(WSAGetLastError()));
}

void send_all(SOCKET socket, const char* data, std::size_t size) {
    std::size_t sent_total = 0;
    while (sent_total < size) {
        const auto remaining = size - sent_total;
        const int chunk = static_cast<int>(std::min(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int sent = send(socket, data + sent_total, chunk, 0);
        if (sent == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
            throw_socket_error("send");
        }
        if (sent == 0) {
            throw std::runtime_error("send returned zero before the frame was complete");
        }
        sent_total += static_cast<std::size_t>(sent);
    }
}

bool receive_exact(SOCKET socket, char* data, std::size_t size) {
    std::size_t received_total = 0;
    while (received_total < size) {
        const auto remaining = size - received_total;
        const int chunk = static_cast<int>(std::min(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int received = recv(socket, data + received_total, chunk, 0);
        if (received == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
            throw_socket_error("recv");
        }
        if (received == 0) {
            return false;
        }
        received_total += static_cast<std::size_t>(received);
    }
    return true;
}

void send_frame(SOCKET socket, const std::string& message) {
    if (message.size() > kMaxMessageBytes) {
        throw std::runtime_error("outgoing message exceeds 4096 bytes");
    }
    const auto length = static_cast<std::uint32_t>(message.size());
    const std::uint32_t network_length = htonl(length);
    send_all(socket,
             reinterpret_cast<const char*>(&network_length),
             sizeof(network_length));
    send_all(socket, message.data(), message.size());
}

bool receive_frame(SOCKET socket, std::string& message) {
    std::uint32_t network_length = 0;
    if (!receive_exact(socket,
                       reinterpret_cast<char*>(&network_length),
                       sizeof(network_length))) {
        return false;
    }
    const std::uint32_t length = ntohl(network_length);
    if (length > kMaxMessageBytes) {
        throw std::runtime_error("incoming message exceeds 4096 bytes");
    }
    message.resize(length);
    return length == 0 || receive_exact(socket, message.data(), length);
}

void close_socket(std::atomic<SOCKET>& socket_holder) {
    const SOCKET socket = socket_holder.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

class NetworkServer {
public:
    explicit NetworkServer(HWND window) : window_(window) {}

    ~NetworkServer() { stop(); }

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    void start() {
        worker_ = std::thread(&NetworkServer::run, this);
    }

    void stop() {
        if (stopping_.exchange(true)) {
            return;
        }
        close_socket(client_);
        close_socket(listener_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

    bool send_command(std::string command) {
        if (!connected()) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        outgoing_.push_back(std::move(command));
        return true;
    }

private:
    void post(UiEventType type, std::string text) const {
        auto* event = new UiEvent{type, std::move(text)};
        if (!PostMessageW(window_, kNetworkEvent, 0, reinterpret_cast<LPARAM>(event))) {
            delete event;
        }
    }

    void clear_commands() {
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        outgoing_.clear();
    }

    bool pop_command(std::string& command) {
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        if (outgoing_.empty()) {
            return false;
        }
        command = std::move(outgoing_.front());
        outgoing_.pop_front();
        return true;
    }

    void run_session(SOCKET socket) {
        fd_set handshake_set;
        FD_ZERO(&handshake_set);
        FD_SET(socket, &handshake_set);
        timeval handshake_timeout{};
        handshake_timeout.tv_sec = 5;
        const int handshake_ready =
            select(0, &handshake_set, nullptr, nullptr, &handshake_timeout);
        if (handshake_ready <= 0) {
            throw std::runtime_error("robot handshake timed out");
        }

        std::string hello;
        if (!receive_frame(socket, hello) || hello.rfind("HELLO|", 0) != 0 ||
            hello.size() <= 6) {
            throw std::runtime_error("expected HELLO|<robot-id>");
        }
        const std::string robot_id = hello.substr(6);
        send_frame(socket, "WELCOME|" + robot_id);
        connected_.store(true);
        post(UiEventType::Connected, robot_id);

        auto last_received = std::chrono::steady_clock::now();
        auto last_ping = last_received;
        std::uint64_t ping_sequence = 1;

        while (!stopping_.load()) {
            std::string command;
            while (pop_command(command)) {
                send_frame(socket, command);
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_ping >= kHeartbeatInterval) {
                send_frame(socket, "PING|" + std::to_string(ping_sequence++));
                last_ping = now;
            }
            if (now - last_received > kHeartbeatTimeout) {
                throw std::runtime_error("heartbeat timeout");
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket, &read_set);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100'000;
            const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) {
                throw_socket_error("select(client)");
            }
            if (ready == 0) {
                continue;
            }

            std::string message;
            if (!receive_frame(socket, message)) {
                break;
            }
            last_received = std::chrono::steady_clock::now();
            if (message.rfind("PONG|", 0) == 0) {
                continue;
            }
            post(UiEventType::Message, message);
            if (message.rfind("BYE|", 0) == 0) {
                break;
            }
        }
    }

    void run() {
        WSADATA winsock_data{};
        const int startup_result = WSAStartup(MAKEWORD(2, 2), &winsock_data);
        if (startup_result != 0) {
            post(UiEventType::Error,
                 "WSAStartup failed with error " + std::to_string(startup_result));
            return;
        }

        try {
            const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET) {
                throw_socket_error("socket");
            }
            listener_.store(listener);

            const BOOL reuse_address = TRUE;
            if (setsockopt(listener,
                           SOL_SOCKET,
                           SO_REUSEADDR,
                           reinterpret_cast<const char*>(&reuse_address),
                           sizeof(reuse_address)) == SOCKET_ERROR) {
                throw_socket_error("setsockopt(SO_REUSEADDR)");
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(kListenPort);
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(listener,
                     reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == SOCKET_ERROR) {
                throw_socket_error("bind");
            }
            if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
                throw_socket_error("listen");
            }
            post(UiEventType::Listening,
                 "Listening on 0.0.0.0:" + std::to_string(kListenPort));

            while (!stopping_.load()) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(listener, &read_set);
                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 200'000;
                const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
                if (ready == SOCKET_ERROR) {
                    if (stopping_.load()) {
                        break;
                    }
                    throw_socket_error("select(listener)");
                }
                if (ready == 0) {
                    continue;
                }

                sockaddr_in peer{};
                int peer_size = sizeof(peer);
                const SOCKET accepted = accept(
                    listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
                if (accepted == INVALID_SOCKET) {
                    if (stopping_.load()) {
                        break;
                    }
                    throw_socket_error("accept");
                }
                client_.store(accepted);

                try {
                    run_session(accepted);
                } catch (const std::exception& error) {
                    if (!stopping_.load()) {
                        post(UiEventType::Error, error.what());
                    }
                }

                connected_.store(false);
                clear_commands();
                close_socket(client_);
                if (!stopping_.load()) {
                    post(UiEventType::Disconnected,
                         "Robot disconnected; waiting for reconnection");
                }
            }
        } catch (const std::exception& error) {
            if (!stopping_.load()) {
                post(UiEventType::Error, error.what());
            }
        }

        connected_.store(false);
        close_socket(client_);
        close_socket(listener_);
        WSACleanup();
    }

    HWND window_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<SOCKET> listener_{INVALID_SOCKET};
    std::atomic<SOCKET> client_{INVALID_SOCKET};
    std::thread worker_;
    std::mutex queue_mutex_;
    std::deque<std::string> outgoing_;
};

struct Pose {
    double x = 0.0;
    double y = 0.0;
};

struct AppState {
    HWND window = nullptr;
    HWND connection_label = nullptr;
    HWND mission_label = nullptr;
    HWND task_edit = nullptr;
    HWND x_edit = nullptr;
    HWND y_edit = nullptr;
    HWND tolerance_edit = nullptr;
    HWND timeout_edit = nullptr;
    HWND start_button = nullptr;
    HWND stop_button = nullptr;
    HWND status_button = nullptr;
    HWND profile_good_button = nullptr;
    HWND profile_congested_button = nullptr;
    HWND profile_disconnected_button = nullptr;
    HWND network_profile_label = nullptr;
    HWND log_edit = nullptr;
    std::unique_ptr<NetworkServer> server;
    bool connected = false;
    bool have_pose = false;
    bool have_target = false;
    Pose pose;
    Pose target;
    std::vector<Pose> path;
    std::wstring robot_id = L"-";
    std::wstring mission_state = L"IDLE";
    std::wstring communication_state = L"UNKNOWN";
    int progress = 0;
    double distance = 0.0;
    double network_quality = 1.0;
    double speed_scale = 1.0;
};

std::vector<std::string> split_fields(const std::string& message) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t separator = message.find('|', begin);
        if (separator == std::string::npos) {
            fields.push_back(message.substr(begin));
            break;
        }
        fields.push_back(message.substr(begin, separator - begin));
        begin = separator + 1;
    }
    return fields;
}

bool parse_double(const std::string& text, double& value) {
    try {
        std::size_t used = 0;
        value = std::stod(text, &used);
        return used == text.size() && std::isfinite(value);
    } catch (...) {
        return false;
    }
}

bool parse_int(const std::string& text, int& value) {
    try {
        std::size_t used = 0;
        value = std::stoi(text, &used);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

std::wstring read_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void append_log(AppState& state, const std::wstring& text) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[32]{};
    swprintf_s(timestamp,
               L"[%02u:%02u:%02u] ",
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond));
    const std::wstring line = std::wstring(timestamp) + text + L"\r\n";
    SendMessageW(state.log_edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(state.log_edit,
                 EM_REPLACESEL,
                 FALSE,
                 reinterpret_cast<LPARAM>(line.c_str()));
}

void set_controls_enabled(AppState& state, bool enabled) {
    EnableWindow(state.start_button, enabled);
    EnableWindow(state.stop_button, enabled);
    EnableWindow(state.status_button, enabled);
    EnableWindow(state.profile_good_button, enabled);
    EnableWindow(state.profile_congested_button, enabled);
    EnableWindow(state.profile_disconnected_button, enabled);
}

void update_mission_label(AppState& state) {
    wchar_t text[512]{};
    swprintf_s(text,
               L"任务状态：%s    进度：%d%%    剩余距离：%.3f m    "
               L"通信：%s    网络质量：%.2f    速度比例：%.0f%%",
               state.mission_state.c_str(),
               state.progress,
               state.distance,
               state.communication_state.c_str(),
               state.network_quality,
               state.speed_scale * 100.0);
    SetWindowTextW(state.mission_label, text);
}

void add_pose(AppState& state, double x, double y) {
    state.pose = Pose{x, y};
    state.have_pose = true;
    if (state.path.empty()) {
        state.path.push_back(state.pose);
    } else {
        const double dx = x - state.path.back().x;
        const double dy = y - state.path.back().y;
        if (dx * dx + dy * dy > 0.0001) {
            state.path.push_back(state.pose);
        }
    }
    InvalidateRect(state.window, nullptr, FALSE);
}

void process_protocol_message(AppState& state, const std::string& message) {
    const auto fields = split_fields(message);
    if (fields.empty()) {
        return;
    }

    if (fields[0] == "ACK" && fields.size() >= 3 && fields[1] == "PROFILE") {
        const std::wstring profile = to_wide(fields[2]);
        SetWindowTextW(state.network_profile_label,
                       (L"当前网络档位：" + profile + L"（设置成功）").c_str());
        append_log(state, L"Network profile changed to " + profile);
        return;
    }

    if (fields[0] == "ERROR" && fields.size() >= 3 && fields[1] == "PROFILE") {
        const std::wstring profile = to_wide(fields[2]);
        SetWindowTextW(state.network_profile_label,
                       (L"网络档位设置失败：" + profile).c_str());
        append_log(state, L"robot> " + to_wide(message));
        return;
    }

    if (fields[0] == "FEEDBACK" && fields.size() >= 9) {
        int progress = state.progress;
        double distance = state.distance;
        double quality = state.network_quality;
        double speed = state.speed_scale;
        double x = state.pose.x;
        double y = state.pose.y;
        parse_int(fields[2], progress);
        parse_double(fields[3], distance);
        parse_double(fields[4], quality);
        parse_double(fields[6], speed);
        if (parse_double(fields[7], x) && parse_double(fields[8], y)) {
            add_pose(state, x, y);
        }
        state.mission_state = L"RUNNING";
        state.progress = progress;
        state.distance = distance;
        state.network_quality = quality;
        state.communication_state = to_wide(fields[5]);
        state.speed_scale = speed;
        update_mission_label(state);
        return;
    }

    if (fields[0] == "STATUS" && fields.size() >= 4) {
        state.mission_state = to_wide(fields[2]);
        parse_int(fields[3], state.progress);
        if (fields.size() >= 10) {
            parse_double(fields[4], state.distance);
            parse_double(fields[5], state.network_quality);
            state.communication_state = to_wide(fields[6]);
            parse_double(fields[7], state.speed_scale);
            double x = state.pose.x;
            double y = state.pose.y;
            if (parse_double(fields[8], x) && parse_double(fields[9], y)) {
                add_pose(state, x, y);
            }
        }
        update_mission_label(state);
    } else if (fields[0] == "RESULT" && fields.size() >= 3) {
        state.mission_state = to_wide(fields[2]);
        if (state.mission_state == L"SUCCEEDED") {
            state.progress = 100;
        }
        update_mission_label(state);
    } else if (fields[0] == "ACK" && fields.size() >= 3) {
        state.mission_state = to_wide(fields[2]);
        update_mission_label(state);
    }

    append_log(state, L"robot> " + to_wide(message));
}

HWND create_control(HWND parent,
                    const wchar_t* class_name,
                    const wchar_t* text,
                    DWORD style,
                    int x,
                    int y,
                    int width,
                    int height,
                    int identifier = 0,
                    DWORD extended_style = 0) {
    HWND control = CreateWindowExW(
        extended_style,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (control != nullptr) {
        SendMessageW(control,
                     WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     TRUE);
    }
    return control;
}

void create_controls(AppState& state) {
    state.connection_label = create_control(
        state.window, L"STATIC", L"正在启动 TCP 服务……", SS_LEFT, 20, 15, 980, 24,
        kConnectionLabel);

    create_control(state.window, L"BUTTON", L"任务控制", BS_GROUPBOX, 20, 45, 980, 100);
    create_control(state.window, L"STATIC", L"任务号", SS_LEFT, 40, 76, 45, 22);
    state.task_edit = create_control(
        state.window, L"EDIT", L"1001", ES_AUTOHSCROLL | WS_BORDER, 88, 73, 75, 25,
        kTaskEdit, WS_EX_CLIENTEDGE);
    create_control(state.window, L"STATIC", L"目标 X", SS_LEFT, 180, 76, 45, 22);
    state.x_edit = create_control(
        state.window, L"EDIT", L"5.0", ES_AUTOHSCROLL | WS_BORDER, 228, 73, 65, 25,
        kXEdit, WS_EX_CLIENTEDGE);
    create_control(state.window, L"STATIC", L"目标 Y", SS_LEFT, 310, 76, 45, 22);
    state.y_edit = create_control(
        state.window, L"EDIT", L"2.0", ES_AUTOHSCROLL | WS_BORDER, 358, 73, 65, 25,
        kYEdit, WS_EX_CLIENTEDGE);
    create_control(state.window, L"STATIC", L"容差", SS_LEFT, 440, 76, 35, 22);
    state.tolerance_edit = create_control(
        state.window, L"EDIT", L"0.10", ES_AUTOHSCROLL | WS_BORDER, 478, 73, 65, 25,
        kToleranceEdit, WS_EX_CLIENTEDGE);
    create_control(state.window, L"STATIC", L"超时", SS_LEFT, 560, 76, 35, 22);
    state.timeout_edit = create_control(
        state.window, L"EDIT", L"30.0", ES_AUTOHSCROLL | WS_BORDER, 598, 73, 65, 25,
        kTimeoutEdit, WS_EX_CLIENTEDGE);

    state.start_button = create_control(
        state.window, L"BUTTON", L"启动任务", BS_PUSHBUTTON, 685, 70, 90, 31,
        kStartButton);
    state.stop_button = create_control(
        state.window, L"BUTTON", L"停止任务", BS_PUSHBUTTON, 785, 70, 90, 31,
        kStopButton);
    state.status_button = create_control(
        state.window, L"BUTTON", L"查询状态", BS_PUSHBUTTON, 885, 70, 90, 31,
        kStatusButton);
    create_control(state.window, L"STATIC", L"网络仿真", SS_LEFT, 40, 112, 55, 22);
    state.profile_good_button = create_control(
        state.window, L"BUTTON", L"正常", BS_PUSHBUTTON, 100, 106, 78, 28,
        kProfileGoodButton);
    state.profile_congested_button = create_control(
        state.window, L"BUTTON", L"拥塞", BS_PUSHBUTTON, 186, 106, 78, 28,
        kProfileCongestedButton);
    state.profile_disconnected_button = create_control(
        state.window, L"BUTTON", L"中断", BS_PUSHBUTTON, 272, 106, 78, 28,
        kProfileDisconnectedButton);
    state.network_profile_label = create_control(
        state.window,
        L"STATIC",
        L"当前网络档位：未知",
        SS_LEFT,
        370,
        111,
        350,
        22,
        kNetworkProfileLabel);
    create_control(state.window,
                   L"BUTTON",
                   L"清除轨迹",
                   BS_PUSHBUTTON,
                   885,
                   106,
                   90,
                   28,
                   kClearButton);

    state.mission_label = create_control(
        state.window,
        L"STATIC",
        L"任务状态：IDLE    进度：0%    通信：UNKNOWN",
        SS_LEFT,
        20,
        158,
        980,
        25,
        kMissionLabel);

    create_control(state.window, L"STATIC", L"二维运行轨迹", SS_LEFT, 20, 190, 150, 22);
    create_control(state.window, L"STATIC", L"关键事件", SS_LEFT, 680, 190, 150, 22);
    state.log_edit = create_control(
        state.window,
        L"EDIT",
        L"",
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER,
        680,
        215,
        320,
        425,
        kLogEdit,
        WS_EX_CLIENTEDGE);
    set_controls_enabled(state, false);
}

POINT world_to_screen(const RECT& area,
                      double x,
                      double y,
                      double min_x,
                      double min_y,
                      double scale) {
    constexpr int padding = 28;
    POINT point{};
    point.x = area.left + padding + static_cast<LONG>(std::lround((x - min_x) * scale));
    point.y = area.bottom - padding - static_cast<LONG>(std::lround((y - min_y) * scale));
    return point;
}

void draw_map(HDC device, const RECT& area, const AppState& state) {
    const HBRUSH background = CreateSolidBrush(RGB(250, 252, 255));
    FillRect(device, &area, background);
    DeleteObject(background);
    FrameRect(device, &area, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    double min_x = -1.0;
    double max_x = 9.0;
    double min_y = -1.0;
    double max_y = 5.0;
    const auto include = [&](const Pose& pose) {
        min_x = std::min(min_x, pose.x - 1.0);
        max_x = std::max(max_x, pose.x + 1.0);
        min_y = std::min(min_y, pose.y - 1.0);
        max_y = std::max(max_y, pose.y + 1.0);
    };
    if (state.have_pose) {
        include(state.pose);
    }
    if (state.have_target) {
        include(state.target);
    }
    for (const Pose& pose : state.path) {
        include(pose);
    }

    constexpr int padding = 28;
    const double width = static_cast<double>(area.right - area.left - 2 * padding);
    const double height = static_cast<double>(area.bottom - area.top - 2 * padding);
    const double scale = std::min(width / (max_x - min_x), height / (max_y - min_y));

    const HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(225, 230, 238));
    const HGDIOBJ old_pen = SelectObject(device, grid_pen);
    for (int index = 0; index <= 10; ++index) {
        const int x = area.left + padding + static_cast<int>(width * index / 10.0);
        MoveToEx(device, x, area.top + padding, nullptr);
        LineTo(device, x, area.bottom - padding);
    }
    for (int index = 0; index <= 8; ++index) {
        const int y = area.top + padding + static_cast<int>(height * index / 8.0);
        MoveToEx(device, area.left + padding, y, nullptr);
        LineTo(device, area.right - padding, y);
    }
    SelectObject(device, old_pen);
    DeleteObject(grid_pen);

    if (state.path.size() >= 2) {
        const HPEN path_pen = CreatePen(PS_SOLID, 2, RGB(47, 111, 237));
        const HGDIOBJ previous_pen = SelectObject(device, path_pen);
        POINT point = world_to_screen(
            area, state.path.front().x, state.path.front().y, min_x, min_y, scale);
        MoveToEx(device, point.x, point.y, nullptr);
        for (std::size_t index = 1; index < state.path.size(); ++index) {
            point = world_to_screen(
                area, state.path[index].x, state.path[index].y, min_x, min_y, scale);
            LineTo(device, point.x, point.y);
        }
        SelectObject(device, previous_pen);
        DeleteObject(path_pen);
    }

    if (state.have_target) {
        const POINT target = world_to_screen(
            area, state.target.x, state.target.y, min_x, min_y, scale);
        const HPEN target_pen = CreatePen(PS_SOLID, 3, RGB(220, 55, 55));
        const HGDIOBJ previous_pen = SelectObject(device, target_pen);
        MoveToEx(device, target.x - 8, target.y, nullptr);
        LineTo(device, target.x + 8, target.y);
        MoveToEx(device, target.x, target.y - 8, nullptr);
        LineTo(device, target.x, target.y + 8);
        SelectObject(device, previous_pen);
        DeleteObject(target_pen);
    }

    if (state.have_pose) {
        const POINT robot = world_to_screen(
            area, state.pose.x, state.pose.y, min_x, min_y, scale);
        const HBRUSH robot_brush = CreateSolidBrush(RGB(31, 117, 229));
        const HGDIOBJ previous_brush = SelectObject(device, robot_brush);
        Ellipse(device, robot.x - 9, robot.y - 9, robot.x + 9, robot.y + 9);
        SelectObject(device, previous_brush);
        DeleteObject(robot_brush);

        if (state.path.size() >= 2) {
            const Pose& previous = state.path[state.path.size() - 2];
            const double dx = state.pose.x - previous.x;
            const double dy = state.pose.y - previous.y;
            const double length = std::hypot(dx, dy);
            if (length > 0.0001) {
                const HPEN heading_pen = CreatePen(PS_SOLID, 3, RGB(15, 54, 120));
                const HGDIOBJ previous_pen = SelectObject(device, heading_pen);
                MoveToEx(device, robot.x, robot.y, nullptr);
                LineTo(device,
                       robot.x + static_cast<int>(15.0 * dx / length),
                       robot.y - static_cast<int>(15.0 * dy / length));
                SelectObject(device, previous_pen);
                DeleteObject(heading_pen);
            }
        }
    }

    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, RGB(80, 86, 96));
    const std::wstring range = L"X: " + std::to_wstring(min_x).substr(0, 5) + L" .. " +
                               std::to_wstring(max_x).substr(0, 5) + L"    Y: " +
                               std::to_wstring(min_y).substr(0, 5) + L" .. " +
                               std::to_wstring(max_y).substr(0, 5);
    TextOutW(device,
             area.left + 8,
             area.bottom - 21,
             range.c_str(),
             static_cast<int>(range.size()));
}

bool send_current_task_command(AppState& state, const std::string& command_name) {
    if (!state.server || !state.server->connected()) {
        MessageBoxW(state.window, L"Linux 机器人尚未连接。", L"无法发送", MB_ICONWARNING);
        return false;
    }
    const std::string task = to_utf8(read_text(state.task_edit));
    if (task.empty() || task.find('|') != std::string::npos) {
        MessageBoxW(state.window, L"请输入有效任务号。", L"输入错误", MB_ICONWARNING);
        return false;
    }
    return state.server->send_command(command_name + '|' + task);
}

void set_network_profile(AppState& state,
                         const std::string& profile,
                         const wchar_t* display_name) {
    if (!state.server || !state.server->connected()) {
        MessageBoxW(state.window, L"Linux 机器人尚未连接。", L"无法设置", MB_ICONWARNING);
        return;
    }
    const std::string command = "PROFILE|" + profile;
    if (state.server->send_command(command)) {
        SetWindowTextW(state.network_profile_label,
                       (std::wstring(L"正在设置网络档位：") + display_name + L"……").c_str());
        append_log(state, L"upper> " + to_wide(command));
    }
}

void start_mission(AppState& state) {
    if (!state.server || !state.server->connected()) {
        MessageBoxW(state.window, L"Linux 机器人尚未连接。", L"无法启动", MB_ICONWARNING);
        return;
    }

    const std::string task = to_utf8(read_text(state.task_edit));
    const std::string x_text = to_utf8(read_text(state.x_edit));
    const std::string y_text = to_utf8(read_text(state.y_edit));
    const std::string tolerance_text = to_utf8(read_text(state.tolerance_edit));
    const std::string timeout_text = to_utf8(read_text(state.timeout_edit));
    double x = 0.0;
    double y = 0.0;
    double tolerance = 0.0;
    double timeout = 0.0;
    if (task.empty() || task.find('|') != std::string::npos ||
        !parse_double(x_text, x) || !parse_double(y_text, y) ||
        !parse_double(tolerance_text, tolerance) || tolerance <= 0.0 ||
        !parse_double(timeout_text, timeout) || timeout <= 0.0) {
        MessageBoxW(state.window,
                    L"请检查任务号、目标坐标、容差和超时时间。",
                    L"输入错误",
                    MB_ICONWARNING);
        return;
    }

    const std::string command = "START|" + task + '|' + x_text + '|' + y_text + '|' +
                                tolerance_text + '|' + timeout_text;
    if (state.server->send_command(command)) {
        state.target = Pose{x, y};
        state.have_target = true;
        state.path.clear();
        if (state.have_pose) {
            state.path.push_back(state.pose);
        }
        state.progress = 0;
        state.mission_state = L"STARTING";
        update_mission_label(state);
        append_log(state, L"upper> " + to_wide(command));
        InvalidateRect(state.window, nullptr, FALSE);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        state = static_cast<AppState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE:
            create_controls(*state);
            return 0;

        case WM_COMMAND:
            if (state == nullptr) {
                return 0;
            }
            switch (LOWORD(w_param)) {
                case kStartButton:
                    start_mission(*state);
                    return 0;
                case kStopButton:
                    if (send_current_task_command(*state, "STOP")) {
                        append_log(*state,
                                   L"upper> STOP|" + read_text(state->task_edit));
                    }
                    return 0;
                case kStatusButton:
                    if (send_current_task_command(*state, "STATUS")) {
                        append_log(*state,
                                   L"upper> STATUS|" + read_text(state->task_edit));
                    }
                    return 0;
                case kProfileGoodButton:
                    set_network_profile(*state, "good", L"正常");
                    return 0;
                case kProfileCongestedButton:
                    set_network_profile(*state, "congested", L"拥塞");
                    return 0;
                case kProfileDisconnectedButton:
                    set_network_profile(*state, "disconnected", L"中断");
                    return 0;
                case kClearButton:
                    state->path.clear();
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                default:
                    break;
            }
            break;

        case kNetworkEvent: {
            std::unique_ptr<UiEvent> event(reinterpret_cast<UiEvent*>(l_param));
            if (state == nullptr || !event) {
                return 0;
            }
            const std::wstring text = to_wide(event->text);
            switch (event->type) {
                case UiEventType::Listening:
                    SetWindowTextW(state->connection_label, text.c_str());
                    append_log(*state, text);
                    break;
                case UiEventType::Connected:
                    state->connected = true;
                    state->robot_id = text;
                    SetWindowTextW(state->connection_label,
                                   (L"已连接机器人：" + text + L"    端口：19001").c_str());
                    set_controls_enabled(*state, true);
                    append_log(*state, L"Robot connected: " + text);
                    break;
                case UiEventType::Disconnected:
                    state->connected = false;
                    SetWindowTextW(state->connection_label,
                                   L"机器人已断开，正在等待自动重连……");
                    set_controls_enabled(*state, false);
                    SetWindowTextW(state->network_profile_label, L"当前网络档位：未知");
                    append_log(*state, text);
                    break;
                case UiEventType::Message:
                    process_protocol_message(*state, event->text);
                    break;
                case UiEventType::Error:
                    append_log(*state, L"ERROR: " + text);
                    break;
            }
            return 0;
        }

        case WM_PAINT:
            if (state != nullptr) {
                PAINTSTRUCT paint{};
                HDC device = BeginPaint(window, &paint);
                const RECT map_area{20, 215, 650, 640};
                draw_map(device, map_area, *state);
                EndPaint(window, &paint);
                return 0;
            }
            break;

        case WM_DESTROY:
            if (state != nullptr && state->server) {
                state->server->stop();
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    constexpr wchar_t kWindowClass[] = L"LinkGuardBasicUpperComputer";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    AppState state;
    HWND window = CreateWindowExW(0,
                                  kWindowClass,
                                  L"LinkGuard 基础机器人上位机",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  1040,
                                  700,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  &state);
    if (window == nullptr) {
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    state.server = std::make_unique<NetworkServer>(window);
    state.server->start();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
