#include "ChatCommon.h"
#include <algorithm>
#include <mstcpip.h>

namespace {
struct Client {
    SOCKET socket;
    unsigned id;
    std::string pending; // 아직 개행이 도착하지 않은 수신 데이터
    bool disconnected = false;
    std::string peer;
    TCP_INFO_v0 tcpInfo{};
    bool hasTcpInfo = false;
    bool tcpInfoFailed = false;
    ULONGLONG lossTestEnd = 0;
    ULONG lossTestBaseline = 0;
    ULONGLONG probeAt = 0; // 클라이언트가 ACK 차단 필터를 설치할 시간을 준 뒤 probe를 보낸다.
};

void TcpLog(const Client& client, const std::string& event)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::printf("[TCP %02u:%02u:%02u.%03u user%u %s] %s\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        client.id, client.peer.c_str(), event.c_str());
}

bool ReadTcpInfo(Client& client, TCP_INFO_v0& info)
{
    DWORD version = 0, returned = 0;
    if (WSAIoctl(client.socket, SIO_TCP_INFO, &version, sizeof(version),
        &info, sizeof(info), &returned, nullptr, nullptr) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (!client.tcpInfoFailed) {
            TcpLog(client, "TCP_INFO unavailable: WSA=" + std::to_string(error)
                + "; retransmission detection unavailable on this connection");
        }
        client.tcpInfoFailed = true;
        return false;
    }
    client.tcpInfoFailed = false;
    return true;
}

void PollTcpInfo(Client& client)
{
    TCP_INFO_v0 info{};
    if (!ReadTcpInfo(client, info)) return;
    if (client.hasTcpInfo) {
        // ULONG의 모듈러 차이로 누적 카운터의 wraparound도 처리한다.
        const ULONG retrans = info.BytesRetrans - client.tcpInfo.BytesRetrans;
        const ULONG timeouts = info.TimeoutEpisodes - client.tcpInfo.TimeoutEpisodes;
        if (retrans || timeouts) {
            TcpLog(client, "RETRANSMISSION detected: bytes_delta=" + std::to_string(retrans)
                + ", bytes_total=" + std::to_string(info.BytesRetrans)
                + ", timeout_episodes_delta=" + std::to_string(timeouts)
                + ", bytes_in_flight=" + std::to_string(info.BytesInFlight)
                + " (OS counters; not an exact lost-packet count)");
        }
    }
    client.tcpInfo = info;
    client.hasTcpInfo = true;
    if (client.lossTestEnd && GetTickCount64() >= client.lossTestEnd) {
        const ULONG retrans = info.BytesRetrans - client.lossTestBaseline;
        TcpLog(client, "LOSS-TEST observation complete: retransmitted_bytes=" + std::to_string(retrans)
            + ", bytes_in_flight=" + std::to_string(info.BytesInFlight)
            + (retrans ? " (retransmission observed)" : " (no retransmission observed; loss NOT confirmed)"));
        client.lossTestEnd = 0;
    }
}

bool SendToClient(Client& client, const std::string& message)
{
    if (!chat::SendAll(client.socket, message)) {
        const int error = WSAGetLastError();
        TcpLog(client, "SEND failed: WSA=" + std::to_string(error));
        client.disconnected = true;
        return false;
    }
    TcpLog(client, "SEND " + std::to_string(message.size())
        + " bytes accepted by Winsock (not a packet/ACK observation)");
    return true;
}

// 서버는 단일 스레드이므로 clients를 보호하는 mutex가 필요 없다.
// 보낸 사람에게도 전달하여 자기 메시지가 서버에 도착했는지 알 수 있다.
void Broadcast(std::vector<Client>& clients, const std::string& message)
{
    std::printf("%s", message.c_str());
    for (auto& client : clients) {
        if (!client.disconnected) SendToClient(client, message);
    }
}

void RemoveDisconnected(std::vector<Client>& clients)
{
    // Broadcast에서도 전송 실패가 발생할 수 있으므로 매번 다시 찾는다.
    while (true) {
        const auto found = std::find_if(clients.begin(), clients.end(),
            [](const Client& client) { return client.disconnected; });
        if (found == clients.end()) break;
        const unsigned id = found->id;
        if (shutdown(found->socket, SD_SEND) == SOCKET_ERROR) {
            TcpLog(*found, "SHUTDOWN failed: WSA=" + std::to_string(WSAGetLastError()));
        }
        else {
            TcpLog(*found, "SHUTDOWN SD_SEND: local send closed; TCP handles FIN (capture to verify)");
        }
        if (closesocket(found->socket) == SOCKET_ERROR) {
            TcpLog(*found, "CLOSE failed: WSA=" + std::to_string(WSAGetLastError()));
        }
        else {
            TcpLog(*found, "CLOSE: socket released; final ACK/TIME_WAIT not observed by this app");
        }
        clients.erase(found);
        Broadcast(clients, "[server] user" + std::to_string(id) + " left\n");
    }
}
} // namespace

int RunChatServer()
{
    SetConsoleOutputCP(CP_UTF8);
    chat::Network network;
    if (!network.Start()) return 1;

    // 1. socket: TCP 연결 요청을 받을 소켓 생성
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        return 1;
    }

    // 2. bind: 이 PC의 모든 IPv4 인터페이스에서 LAN 접속을 받는다
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(chat::Port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        closesocket(listener);
        return 1;
    }

    // 3. listen: 새 연결을 기다리는 상태로 전환
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        std::fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
        closesocket(listener);
        return 1;
    }

    std::printf("Chat server listening on 0.0.0.0:%u (LAN enabled). Ctrl+C to stop.\n", chat::Port);
    std::printf("[TCP] Socket event logs only. Capture packets to see actual SYN/ACK/FIN/RST.\n");
    std::vector<Client> clients;
    unsigned nextId = 1;
    ULONGLONG nextStatsPoll = 0;
    while (true) {
        // select: 여러 소켓 중 accept/recv가 가능한 소켓만 골라준다.
        // 매 호출에서 집합이 수정되므로 루프마다 다시 구성해야 한다.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        for (const auto& client : clients) FD_SET(client.socket, &readable);
        timeval wait{ 0, 250000 }; // 수신이 없어도 OS 재전송 카운터를 주기적으로 관찰한다.
        if (select(0, &readable, nullptr, nullptr, &wait) == SOCKET_ERROR) {
            std::fprintf(stderr, "select failed: %d\n", WSAGetLastError());
            break;
        }

        if (FD_ISSET(listener, &readable)) {
            // 4. accept: 통신용 새 소켓 반환. listener는 계속 접속을 받는다.
            sockaddr_in peerAddress{};
            int peerLength = sizeof(peerAddress);
            SOCKET socket = accept(listener, reinterpret_cast<sockaddr*>(&peerAddress), &peerLength);
            if (socket != INVALID_SOCKET) {
                char ip[INET_ADDRSTRLEN]{};
                if (!inet_ntop(AF_INET, &peerAddress.sin_addr, ip, sizeof(ip))) {
                    std::snprintf(ip, sizeof(ip), "unknown");
                }
                const std::string peer = std::string(ip) + ":" + std::to_string(ntohs(peerAddress.sin_port));
                // 느린 수신자가 서버 전체를 무기한 멈추지 않도록 전송 제한.
                const DWORD timeout = 2000;
                if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR
                    || clients.size() >= FD_SETSIZE - 1) {
                    std::printf("[TCP %s] Connection rejected: setup failed or server full\n", peer.c_str());
                    closesocket(socket);
                }
                else {
                    const unsigned id = nextId++;
                    clients.push_back({ socket, id, {}, false, peer });
                    TcpLog(clients.back(), "ACCEPT: established connection; 3-way handshake handled by OS (not captured)");
                    PollTcpInfo(clients.back());
                    SendToClient(clients.back(), "[server] You are user" + std::to_string(id)
                        + ". Type a message; /quit to leave.\n");
                    Broadcast(clients, "[server] user" + std::to_string(id) + " joined\n");
                }
            }
        }

        for (auto& client : clients) {
            if (client.disconnected || !FD_ISSET(client.socket, &readable)) continue;
            char buffer[512];
            // 5. recv: 수신 바이트를 읽는다. 0은 상대의 정상 연결 종료.
            const int count = recv(client.socket, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                if (count == 0) {
                    TcpLog(client, "RECV EOF: peer gracefully closed its send side (FIN processed by TCP)");
                }
                else {
                    const int error = WSAGetLastError();
                    TcpLog(client, "RECV failed: WSA=" + std::to_string(error)
                        + (error == WSAECONNRESET ? " (connection reset; not a graceful close)" : ""));
                }
                client.disconnected = true;
                continue;
            }
            TcpLog(client, "RECV " + std::to_string(count) + " stream bytes (not a packet boundary)");
            std::vector<std::string> messages;
            if (!chat::ExtractLines(client.pending, buffer, count, messages)) {
                TcpLog(client, "DISCONNECT: message exceeds size limit");
                SendToClient(client, "[server] Message too long; disconnecting.\n");
                client.disconnected = true;
                continue;
            }
            for (const auto& message : messages) {
                if (message == "/quit") {
                    TcpLog(client, "DISCONNECT: /quit command received");
                    client.disconnected = true;
                    break;
                }
                if (message.empty()) continue;
                if (message == "/loss-probe" || message.compare(0, 12, "/loss-probe ") == 0) {
                    unsigned seconds = 0;
                    if (message.size() <= 12 || !chat::ParseLossSeconds(message.substr(12), seconds)) {
                        SendToClient(client, "[server] Usage: /loss-probe <1-5 seconds>\n");
                        continue;
                    }
                    if (client.lossTestEnd && GetTickCount64() < client.lossTestEnd) {
                        SendToClient(client, "[server] Loss observation still active; wait before repeating.\n");
                        continue;
                    }
                    PollTcpInfo(client);
                    if (client.tcpInfoFailed || !client.hasTcpInfo) {
                        SendToClient(client, "[server] TCP_INFO unavailable; cannot verify retransmissions.\n");
                        continue;
                    }
                    client.lossTestBaseline = client.tcpInfo.BytesRetrans;
                    client.lossTestEnd = GetTickCount64() + (seconds + 10) * 1000ULL;
                    client.probeAt = GetTickCount64() + 500;
                    TcpLog(client, "LOSS-TEST requested: probe in 500ms; watching real OS counters for "
                        + std::to_string(seconds + 10) + "s (request alone does not prove packet loss)");
                    continue;
                }
                // 6. send: Broadcast -> SendAll -> send 순서로 전체에게 전달
                Broadcast(clients, "[user" + std::to_string(client.id) + "] " + message + "\n");
                if (client.disconnected) break;
            }
        }
        const ULONGLONG now = GetTickCount64();
        for (auto& client : clients) {
            if (client.disconnected || !client.probeAt || now < client.probeAt) continue;
            client.probeAt = 0;
            // 이 데이터의 ACK는 클라이언트 필터에 의해 버려지므로 OS TCP가 재전송하게 된다.
            TcpLog(client, "LOSS-TEST probe: sending 270 bytes; expecting ACK loss -> RTO retransmission");
            SendToClient(client, "[loss-probe] " + std::string(256, 'X') + "\n");
        }
        if (now >= nextStatsPoll) {
            for (auto& client : clients) {
                if (!client.disconnected) PollTcpInfo(client);
            }
            nextStatsPoll = now + 250;
        }
        RemoveDisconnected(clients);
    }

    for (const auto& client : clients) closesocket(client.socket);
    closesocket(listener);
    return 1;
}
