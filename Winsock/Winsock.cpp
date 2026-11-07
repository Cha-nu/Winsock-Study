#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <climits>
#include <charconv>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

constexpr const char* DEFAULT_PORT = "27015";

// The server owns one shared test target. Invalid commands never change its HP.
std::string HandleCommand(const std::string& command, int& health)
{
    if (command == "hp") {
        return "HP " + std::to_string(health) + "\n";
    }
    if (command.compare(0, 7, "attack ") != 0) {
        return "ERROR use: attack <positive damage> or hp\n";
    }

    int damage = 0;
    const char* first = command.data() + 7;
    const char* last = command.data() + command.size();
    const auto parsed = std::from_chars(first, last, damage);
    if (parsed.ec != std::errc{} || parsed.ptr != last || damage <= 0) {
        return "ERROR damage must be an integer from 1 to 2147483647\n";
    }

    health = damage >= health ? 0 : health - damage;
    std::printf("Attack: %d damage, remaining HP: %d\n", damage, health);
    return "HP " + std::to_string(health) + "\n";
}

int ServeClient(SOCKET clientSocket, int& health)
{
    // One command per connection. The client's SD_SEND marks the request end.
    // Accumulate TCP fragments before parsing; recv() is not a message boundary.
    constexpr size_t MAX_COMMAND_BYTES = 128;
    std::string command;
    bool tooLong = false;
    char buffer[512];
    int received = 0;
    while ((received = recv(clientSocket, buffer, sizeof(buffer), 0)) > 0) {
        if (!tooLong && command.size() + static_cast<size_t>(received) <= MAX_COMMAND_BYTES) {
            command.append(buffer, received);
        }
        else {
            tooLong = true;
        }
    }

    if (received == SOCKET_ERROR) {
        std::fprintf(stderr, "Server recv failed: %d\n", WSAGetLastError());
        return 1;
    }

    const std::string response = tooLong ? "ERROR command too long\n" : HandleCommand(command, health);
    int totalSent = 0;
    const int responseLength = static_cast<int>(response.size());
    while (totalSent < responseLength) {
        const int sent = send(clientSocket, response.data() + totalSent,
            responseLength - totalSent, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            std::fprintf(stderr, "Server send failed: %d\n",
                sent == SOCKET_ERROR ? WSAGetLastError() : WSAECONNRESET);
            return 1;
        }
        totalSent += sent;
    }
    if (shutdown(clientSocket, SD_SEND) == SOCKET_ERROR) {
        std::fprintf(stderr, "Server shutdown failed: %d\n", WSAGetLastError());
        return 1;
    }
    return 0;
}

int RunServer()
{
    WSADATA wsaData{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0) {
        std::fprintf(stderr, "WSAStartup failed: %d\n", startupResult);
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::fprintf(stderr, "Server socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Bind only to this PC, so the test server is not exposed to the network.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(27015);
    if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address),
        sizeof(address)) == SOCKET_ERROR) {
        std::fprintf(stderr, "bind failed: %d (is another server using port %s?)\n",
            WSAGetLastError(), DEFAULT_PORT);
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    int health = 100;
    std::printf("Game test server listening on 127.0.0.1:%s (HP: %d)\n"
        "Run another console: Winsock.exe client 127.0.0.1 \"attack 2\"\n"
        "Commands: attack <positive damage>, hp. Restart server to reset HP.\n"
        "Press Ctrl+C to stop the server.\n", DEFAULT_PORT, health);
    while (true) {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            std::fprintf(stderr, "accept failed: %d\n", WSAGetLastError());
            break;
        }

        std::printf("Client connected.\n");
        const int requestResult = ServeClient(clientSocket, health);
        closesocket(clientSocket);
        std::printf("Client %s. Waiting for the next connection.\n",
            requestResult == 0 ? "disconnected" : "connection failed");
    }

    closesocket(listenSocket);
    WSACleanup();
    return 1;
}

int RunClient(int argc, char* argv[])
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: %s <server> [message]\n"
            "Example: %s 127.0.0.1 \"attack 2\"\n"
            "The TCP server must be listening on port %s.\n",
            argv[0], argv[0], DEFAULT_PORT);
        return 1;
    }

    const char* message = argc == 3 ? argv[2] : "hp";
    const size_t messageLength = std::strlen(message);
    if (messageLength > INT_MAX) {
        std::fprintf(stderr, "Message is too long.\n");
        return 1;
    }

    WSADATA wsaData{};
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        return 1;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    iResult = getaddrinfo(argv[1], DEFAULT_PORT, &hints, &result);
    if (iResult != 0) {
        std::fprintf(stderr, "getaddrinfo failed: %d\n", iResult);
        WSACleanup();
        return 1;
    }

    // Try every resolved address, including both IPv4 and IPv6 candidates.
    SOCKET connectSocket = INVALID_SOCKET;
    int connectionError = WSAEHOSTUNREACH;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (connectSocket == INVALID_SOCKET) {
            connectionError = WSAGetLastError();
            continue;
        }

        if (connect(connectSocket, ptr->ai_addr,
            static_cast<int>(ptr->ai_addrlen)) != SOCKET_ERROR) {
            break;
        }

        connectionError = WSAGetLastError();
        closesocket(connectSocket);
        connectSocket = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (connectSocket == INVALID_SOCKET) {
        std::fprintf(stderr, "Unable to connect to %s:%s (error %d).\n",
            argv[1], DEFAULT_PORT, connectionError);
        WSACleanup();
        return 1;
    }

    std::printf("Connected to %s:%s\n", argv[1], DEFAULT_PORT);

    // TCP may send fewer bytes than requested, so send the remaining bytes.
    int totalSent = 0;
    const int bytesToSend = static_cast<int>(messageLength);
    while (totalSent < bytesToSend) {
        iResult = send(connectSocket, message + totalSent, bytesToSend - totalSent, 0);
        if (iResult == SOCKET_ERROR || iResult == 0) {
            const int error = iResult == SOCKET_ERROR ? WSAGetLastError() : WSAECONNRESET;
            std::fprintf(stderr, "send failed: %d\n", error);
            closesocket(connectSocket);
            WSACleanup();
            return 1;
        }
        totalSent += iResult;
    }
    std::printf("Bytes sent: %d\n", totalSent);

    // Signal the end of the request while keeping the receive side open.
    if (shutdown(connectSocket, SD_SEND) == SOCKET_ERROR) {
        std::fprintf(stderr, "shutdown failed: %d\n", WSAGetLastError());
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }

    char recvBuffer[512];
    while ((iResult = recv(connectSocket, recvBuffer, sizeof(recvBuffer), 0)) > 0) {
        std::printf("Received %d bytes: ", iResult);
        // Received data is not necessarily null-terminated.
        std::fwrite(recvBuffer, 1, iResult, stdout);
        std::putchar('\n');
    }

    const bool receiveFailed = iResult == SOCKET_ERROR;
    if (receiveFailed) {
        std::fprintf(stderr, "recv failed: %d\n", WSAGetLastError());
    }
    else {
        std::printf("Connection closed by server.\n");
    }

    closesocket(connectSocket);
    WSACleanup();
    return receiveFailed ? 1 : 0;
}

int RunUdpMovement(bool server);
int RunChatServer();
int RunChatClient(const char* host);

int main(int argc, char* argv[])
{
    // Show server events immediately, including when output is redirected.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc == 2 && std::strcmp(argv[1], "chat-server") == 0) {
        return RunChatServer();
    }
    if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "chat-client") == 0) {
        return RunChatClient(argc == 3 ? argv[2] : "127.0.0.1");
    }

    if (argc == 2 && std::strcmp(argv[1], "udp-server") == 0) {
        return RunUdpMovement(true);
    }
    if (argc == 2 && std::strcmp(argv[1], "udp-client") == 0) {
        return RunUdpMovement(false);
    }

    if (argc == 2 && std::strcmp(argv[1], "server") == 0) {
        return RunServer();
    }
    if (argc >= 3 && argc <= 4 && std::strcmp(argv[1], "client") == 0) {
        return RunClient(argc - 1, argv + 1);
    }
    // Keep the original "Winsock.exe <server> [message]" command working.
    if (argc >= 2 && argc <= 3 && std::strcmp(argv[1], "server") != 0
        && std::strcmp(argv[1], "client") != 0) {
        return RunClient(argc, argv);
    }

    std::fprintf(stderr, "Usage:\n"
        "  Winsock.exe chat-server\n"
        "  Winsock.exe chat-client [host] (default: 127.0.0.1)\n"
        "  Winsock.exe udp-server\n"
        "  Winsock.exe udp-client (run in two separate consoles)\n"
        "  %s server\n"
        "  %s client <server> [message]\n"
        "Local test (use two consoles):\n"
        "  %s server\n"
        "  %s client 127.0.0.1 \"attack 2\"\n",
        argv[0], argv[0], argv[0], argv[0]);
    return 1;
}
