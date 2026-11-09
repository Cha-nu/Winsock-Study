#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <charconv>

#pragma comment(lib, "Ws2_32.lib")

namespace chat {
constexpr unsigned short Port = 27017;
constexpr size_t MaxMessage = 1024; // UTF-8 바이트 수 기준

// 실험은 최대 5초로 제한한다. 잘못된 입력은 필터를 만들기 전에 거절한다.
inline bool ParseLossSeconds(const std::string& value, unsigned& seconds)
{
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
        && seconds >= 1 && seconds <= 5;
}

// 함수의 모든 return 경로에서 WSACleanup이 호출되도록 수명을 묶는다.
struct Network {
    bool started = false;
    bool Start() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        started = error == 0;
        if (!started) std::fprintf(stderr, "WSAStartup failed: %d\n", error);
        return started;
    }
    ~Network() { if (started) WSACleanup(); }
};

// send는 일부 바이트만 전송할 수도 있다. 남은 부분을 끝까지 보낸다.
inline bool SendAll(SOCKET socket, const std::string& message)
{
    size_t offset = 0;
    while (offset < message.size()) {
        const int sent = send(socket, message.data() + offset,
            static_cast<int>(message.size() - offset), 0);
        if (sent <= 0) return false;
        offset += sent;
    }
    return true;
}

// recv 한 번 != 메시지 한 개. 쪼개진 데이터는 pending에 보관하고,
// 여러 줄이 한 번에 오면 모두 추출한다. 개행 전까지는 처리하지 않는다.
inline bool ExtractLines(std::string& pending, const char* data, int count,
    std::vector<std::string>& lines, size_t maxLength = MaxMessage)
{
    pending.append(data, count);
    size_t newline = 0;
    while ((newline = pending.find('\n')) != std::string::npos) {
        if (newline > maxLength) return false;
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return pending.size() <= maxLength;
}
} // namespace chat

int RunChatServer();
int RunChatClient(const char* host);
