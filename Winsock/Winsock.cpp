#include "ChatCommon.h"
#include <cstring>

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc == 2 && (std::strcmp(argv[1], "server") == 0
        || std::strcmp(argv[1], "chat-server") == 0)) {
        return RunChatServer();
    }
    if ((argc == 2 || argc == 3) && (std::strcmp(argv[1], "client") == 0
        || std::strcmp(argv[1], "chat-client") == 0)) {
        return RunChatClient(argc == 3 ? argv[2] : "127.0.0.1");
    }

    std::fprintf(stderr, "Usage:\n"
        "  Winsock.exe server\n"
        "  Winsock.exe client [server-IP] (default: 127.0.0.1)\n"
        "Run the server on one PC, then a client on each PC.\n"
        "On other LAN PCs, use the server PC's LAN IPv4 address.\n"
        "Type /quit to leave the chat.\n");
    return 1;
}
