#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr unsigned short Port = 27016;
constexpr float Speed = 5.0f; // World units per second, decided ONLY by the server.
constexpr auto Tick = 50ms; // 20 Hz simulation and snapshots.

// Text datagrams keep the protocol visible without struct padding/endianness issues.
// HELLO -> WELCOME <id>
// INPUT <sequence> <dx> <dy> -> STATE <tick> <id> <x> <y> ...
// Each input describes a direction, NOT a requested position or distance.
struct Player {
    sockaddr_in address{};
    unsigned id = 0;
    uint64_t sequence = 0;
    int dx = 0, dy = 0;
    float x = 0, y = 0;
    Clock::time_point lastSeen = Clock::now();
    Clock::time_point lastInput = Clock::now();
};

bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// RAII ensures sockets are released on every return path.
struct Network {
    bool started = false;
    SOCKET socket = INVALID_SOCKET;
    ~Network() {
        if (socket != INVALID_SOCKET) closesocket(socket);
        if (started) WSACleanup();
    }
    bool Open() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            std::fprintf(stderr, "WSAStartup failed: %d\n", error);
            return false;
        }
        started = true;
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) return false;
        u_long nonblocking = 1;
        return ioctlsocket(socket, FIONBIO, &nonblocking) == 0;
    }
};

bool Send(SOCKET socket, const sockaddr_in& to, const std::string& message)
{
    const int sent = sendto(socket, message.data(), static_cast<int>(message.size()),
        0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        std::fprintf(stderr, "sendto failed: %d\n", WSAGetLastError());
        return false;
    }
    return true; // A dropped send is recovered by the next periodic update.
}

// 0 = no more packets, -1 = fatal error, 1 = packet (possibly empty).
int Receive(SOCKET socket, sockaddr_in& from, std::string& message)
{
    char buffer[1024];
    int length = sizeof(from);
    const int count = recvfrom(socket, buffer, sizeof(buffer), 0,
        reinterpret_cast<sockaddr*>(&from), &length);
    if (count == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return 0;
        // Ignore oversized packets and ICMP errors from peers that have quit.
        if (error == WSAEMSGSIZE || error == WSAECONNRESET) {
            message.clear();
            return 1;
        }
        std::fprintf(stderr, "recvfrom failed: %d\n", error);
        return -1;
    }
    message.assign(buffer, count);
    return 1;
}

int Server(SOCKET socket)
{
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(Port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        std::fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        return 1;
    }
    std::puts("UDP movement server: 127.0.0.1:27016, 20 Hz, max 2 players. Ctrl+C to stop.");
    std::vector<Player> players;
    unsigned nextId = 1;
    uint64_t tick = 0;
    auto nextTick = Clock::now() + Tick;
    while (true) {
        // Bound packet processing so incoming traffic cannot starve simulation.
        for (int i = 0; i < 128; ++i) {
            sockaddr_in from{};
            std::string message;
            const int result = Receive(socket, from, message);
            if (result < 0) return 1;
            if (result == 0) break;
            auto player = std::find_if(players.begin(), players.end(),
                [&](const Player& p) { return SameAddress(p.address, from); });
            if (message == "HELLO") {
                if (player == players.end()) {
                    if (players.size() >= 2) {
                        Send(socket, from, "FULL");
                        continue;
                    }
                    Player joined;
                    joined.address = from;
                    joined.id = nextId++;
                    players.push_back(joined);
                    player = players.end() - 1;
                    std::printf("Player %u joined.\n", player->id);
                }
                player->lastSeen = Clock::now();
                Send(socket, from, "WELCOME " + std::to_string(player->id));
                continue;
            }
            if (player == players.end()) continue;
            std::istringstream input(message);
            std::string type, extra;
            // Signed parsing rejects negative sequences. A 64-bit counter lasts
            // far beyond this exercise; wraparound handling is omitted.
            int64_t sequence = 0;
            int dx = 0, dy = 0;
            if (!(input >> type >> sequence >> dx >> dy) || type != "INPUT"
                || (input >> extra) || sequence <= 0 || dx < -1 || dx > 1 || dy < -1 || dy > 1)
                continue;
            if (static_cast<uint64_t>(sequence) <= player->sequence) continue;
            player->sequence = static_cast<uint64_t>(sequence);
            player->dx = dx;
            player->dy = dy;
            player->lastSeen = player->lastInput = Clock::now();
        }

        const auto now = Clock::now();
        if (now >= nextTick) {
            // Do not replay a long backlog after a debugger pause.
            nextTick += Tick;
            if (nextTick <= now) nextTick = now + Tick;
            players.erase(std::remove_if(players.begin(), players.end(), [&](const Player& p) {
                if (now - p.lastSeen <= 5s) return false;
                std::printf("Player %u timed out.\n", p.id);
                return true;
            }), players.end());
            std::ostringstream snapshot;
            snapshot << "STATE " << ++tick;
            snapshot.precision(2);
            snapshot << std::fixed;
            for (Player& p : players) {
                // Missing inputs must not leave a player moving forever.
                if (now - p.lastInput > 250ms) p.dx = p.dy = 0;
                const float length = std::sqrt(static_cast<float>(p.dx * p.dx + p.dy * p.dy));
                if (length > 0) {
                    const float distance = Speed * 0.05f;
                    p.x = std::clamp(p.x + p.dx / length * distance, -20.0f, 20.0f);
                    p.y = std::clamp(p.y + p.dy / length * distance, -20.0f, 20.0f);
                }
                snapshot << ' ' << p.id << ' ' << p.x << ' ' << p.y;
            }
            for (const Player& p : players) Send(socket, p.address, snapshot.str());
        }
        std::this_thread::sleep_for(1ms);
    }
}

int Client(SOCKET socket)
{
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.sin_port = htons(Port);
    unsigned id = 0;
    uint64_t sequence = 0, lastTick = 0;
    int dx = 0, dy = 0;
    auto nextSend = Clock::now();
    auto lastServer = Clock::now();
    auto nextPrint = Clock::now();
    std::puts("W/A/S/D: set movement direction, SPACE: stop, Q: quit (no Enter needed).");
    std::puts("Movement continues after releasing a key. Only server positions are displayed.");
    while (true) {
        while (_kbhit()) {
            const int key = _getch();
            if (key == 0 || key == 224) { _getch(); continue; }
            if (key == 'q' || key == 'Q') return 0;
            if (key == 'w' || key == 'W') { dx = 0; dy = 1; }
            if (key == 's' || key == 'S') { dx = 0; dy = -1; }
            if (key == 'a' || key == 'A') { dx = -1; dy = 0; }
            if (key == 'd' || key == 'D') { dx = 1; dy = 0; }
            if (key == ' ') dx = dy = 0;
        }
        const auto now = Clock::now();
        if (now >= nextSend) {
            const std::string message = id == 0 ? "HELLO" :
                "INPUT " + std::to_string(++sequence) + " " + std::to_string(dx) + " " + std::to_string(dy);
            if (!Send(socket, server, message)) return 1;
            nextSend = now + (id == 0 ? 500ms : Tick);
        }
        for (int i = 0; i < 128; ++i) {
            sockaddr_in from{};
            std::string message;
            const int result = Receive(socket, from, message);
            if (result < 0) return 1;
            if (result == 0) break;
            if (!SameAddress(from, server)) continue;
            if (message == "FULL") {
                std::fprintf(stderr, "Server full (2 players). Try again after a player times out.\n");
                return 1;
            }
            std::istringstream input(message);
            std::string type;
            input >> type;
            if (type == "WELCOME" && id == 0) {
                if (input >> id && id != 0) {
                    lastServer = now;
                    std::printf("You are player %u. STATE format: tick [id x y] ...\n", id);
                }
            }
            else if (type == "STATE" && id != 0) {
                uint64_t receivedTick = 0;
                if (!(input >> receivedTick) || receivedTick <= lastTick) continue;
                lastTick = receivedTick;
                lastServer = now;
                if (now >= nextPrint) {
                    std::printf("%s\n", message.c_str());
                    nextPrint = now + 200ms;
                }
            }
        }
        if (now - lastServer > 5s) {
            std::fprintf(stderr, "Server timed out. Start the server, then restart this client.\n");
            return 1;
        }
        std::this_thread::sleep_for(1ms);
    }
}
} // namespace

int RunUdpMovement(bool server)
{
    Network network;
    if (!network.Open()) {
        std::fprintf(stderr, "UDP initialization failed: %d\n", WSAGetLastError());
        return 1;
    }
    return server ? Server(network.socket) : Client(network.socket);
}
