#include "ChatCommon.h"
#include "PacketLoss.h"
#include <atomic>
#include <iostream>
#include <thread>

int RunChatClient(const char* host)
{
    // 콘솔 입력/출력과 네트워크 메시지의 문자 인코딩을 UTF-8로 맞춘다.
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    chat::Network network;
    if (!network.Start()) return 1;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const int result = getaddrinfo(host, "27017", &hints, &addresses);
    if (result != 0) {
        std::fprintf(stderr, "getaddrinfo failed: %d\n", result);
        return 1;
    }

    SOCKET connection = INVALID_SOCKET;
    for (auto* address = addresses; address; address = address->ai_next) {
        connection = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connection == INVALID_SOCKET) continue;
        if (connect(connection, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
        closesocket(connection);
        connection = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (connection == INVALID_SOCKET) {
        std::fprintf(stderr, "Cannot connect to %s:27017. Start chat-server first.\n", host);
        return 1;
    }
    const DWORD timeout = 2000;
    if (setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        closesocket(connection);
        return 1;
    }

    std::printf("Connected. Enter messages (max 1024 UTF-8 bytes). /quit to leave.\n");
    std::printf("/loss [1-5]: real inbound TCP packet loss for N seconds (default 3, Administrator required).\n");
    std::atomic<bool> connected{ true };
    // 수신 스레드: 사용자가 입력 중이어도 다른 사람의 메시지를 받는다.
    // 소켓은 메인 스레드가 이 스레드의 종료를 확인한 뒤 닫는다.
    std::thread receiver([&] {
        std::string pending;
        char buffer[512];
        while (connected) {
            const int count = recv(connection, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            std::vector<std::string> messages;
            if (!chat::ExtractLines(pending, buffer, count, messages, 2048)) break;
            for (const auto& message : messages) std::printf("%s\n", message.c_str());
        }
        if (connected.exchange(false)) {
            std::printf("Disconnected from server. Press Enter to exit.\n");
        }
    });

    // 메인 스레드: 한 줄 입력 -> 개행 추가 -> send. /quit은 로컬 종료 명령.
    std::string message;
    int exitCode = 0;
    while (connected && std::getline(std::cin, message)) {
        // 파이프 입력(PowerShell 등)의 첫 줄에 붙는 UTF-8 BOM을 제거해야 명령어가 인식된다.
        if (message.compare(0, 3, "\xEF\xBB\xBF") == 0) message.erase(0, 3);
        if (!connected || message == "/quit") break;
        if (message.empty()) continue;
        if (message == "/loss" || message.compare(0, 6, "/loss ") == 0) {
            unsigned seconds = 3;
            if (message != "/loss" && !chat::ParseLossSeconds(message.substr(6), seconds)) {
                std::printf("Usage: /loss [1-5 seconds]\n");
                continue;
            }
            if (!RunPacketLossTest(connection, seconds, connected)) { exitCode = 1; break; }
            continue;
        }
        if (message.size() > chat::MaxMessage) {
            std::printf("Message too long (maximum 1024 UTF-8 bytes).\n");
            continue;
        }
        if (!chat::SendAll(connection, message + "\n")) {
            std::fprintf(stderr, "send failed: %d\n", WSAGetLastError());
            exitCode = 1;
            break;
        }
    }

    connected = false;
    shutdown(connection, SD_BOTH); // recv에서 대기 중인 수신 스레드를 깨운다.
    receiver.join();
    closesocket(connection);
    return exitCode;
}
