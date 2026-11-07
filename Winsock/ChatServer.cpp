#include "ChatCommon.h"
#include <algorithm>

namespace {
struct Client {
    SOCKET socket;
    unsigned id;
    std::string pending; // 아직 개행이 도착하지 않은 수신 데이터
    bool disconnected = false;
};

// 서버는 단일 스레드이므로 clients를 보호하는 mutex가 필요 없다.
// 보낸 사람에게도 전달하여 자기 메시지가 서버에 도착했는지 알 수 있다.
void Broadcast(std::vector<Client>& clients, const std::string& message)
{
    std::printf("%s", message.c_str());
    for (auto& client : clients) {
        if (!client.disconnected && !chat::SendAll(client.socket, message)) {
            client.disconnected = true;
        }
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
        shutdown(found->socket, SD_BOTH);
        closesocket(found->socket);
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

    // 2. bind: 이 PC의 127.0.0.1:27017에 소켓을 연결
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

    std::printf("Chat server listening on 127.0.0.1:%u. Ctrl+C to stop.\n", chat::Port);
    std::vector<Client> clients;
    unsigned nextId = 1;
    while (true) {
        // select: 여러 소켓 중 accept/recv가 가능한 소켓만 골라준다.
        // 매 호출에서 집합이 수정되므로 루프마다 다시 구성해야 한다.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        for (const auto& client : clients) FD_SET(client.socket, &readable);
        if (select(0, &readable, nullptr, nullptr, nullptr) == SOCKET_ERROR) {
            std::fprintf(stderr, "select failed: %d\n", WSAGetLastError());
            break;
        }

        if (FD_ISSET(listener, &readable)) {
            // 4. accept: 통신용 새 소켓 반환. listener는 계속 접속을 받는다.
            SOCKET socket = accept(listener, nullptr, nullptr);
            if (socket != INVALID_SOCKET) {
                // 느린 수신자가 서버 전체를 무기한 멈추지 않도록 전송 제한.
                const DWORD timeout = 2000;
                if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR
                    || clients.size() >= FD_SETSIZE - 1) {
                    closesocket(socket);
                }
                else {
                    const unsigned id = nextId++;
                    clients.push_back({ socket, id, {}, false });
                    if (!chat::SendAll(socket, "[server] You are user" + std::to_string(id)
                        + ". Type a message; /quit to leave.\n")) {
                        clients.back().disconnected = true;
                    }
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
                client.disconnected = true;
                continue;
            }
            std::vector<std::string> messages;
            if (!chat::ExtractLines(client.pending, buffer, count, messages)) {
                chat::SendAll(client.socket, "[server] Message too long; disconnecting.\n");
                client.disconnected = true;
                continue;
            }
            for (const auto& message : messages) {
                if (message == "/quit") { client.disconnected = true; break; }
                if (message.empty()) continue;
                // 6. send: Broadcast -> SendAll -> send 순서로 전체에게 전달
                Broadcast(clients, "[user" + std::to_string(client.id) + "] " + message + "\n");
                if (client.disconnected) break;
            }
        }
        RemoveDisconnected(clients);
    }

    for (const auto& client : clients) closesocket(client.socket);
    closesocket(listener);
    return 1;
}
