#include "PacketLoss.h"
#include <fwpmu.h>
#include <chrono>
#include <thread>

#pragma comment(lib, "Fwpuclnt.lib")

namespace {
struct FilterSession {
    HANDLE engine = nullptr;
    ~FilterSession() { if (engine) FwpmEngineClose0(engine); }
    DWORD Close() {
        if (!engine) return ERROR_SUCCESS;
        const DWORD error = FwpmEngineClose0(engine);
        if (error == ERROR_SUCCESS) engine = nullptr;
        return error;
    }
};
}

bool RunPacketLossTest(SOCKET connection, unsigned seconds, const std::atomic<bool>& connected)
{
    if (seconds < 1 || seconds > 5) return true;
    sockaddr_in local{}, remote{};
    int length = sizeof(local);
    if (getsockname(connection, reinterpret_cast<sockaddr*>(&local), &length) == SOCKET_ERROR) {
        std::printf("[loss] Cannot read local endpoint: WSA=%d\n", WSAGetLastError());
        return true;
    }
    length = sizeof(remote);
    if (getpeername(connection, reinterpret_cast<sockaddr*>(&remote), &length) == SOCKET_ERROR) {
        std::printf("[loss] Cannot read remote endpoint: WSA=%d\n", WSAGetLastError());
        return true;
    }
    // 1) 요청을 먼저 보낸다. 필터가 생기면 이 연결의 클라이언트 -> 서버 패킷이 모두 막히기 때문이다.
    //    서버는 요청을 받은 뒤 500ms 지나서 probe 데이터를 보낸다.
    if (!chat::SendAll(connection, "/loss-probe " + std::to_string(seconds) + "\n")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    FilterSession session;
    FWPM_SESSION0 settings{};
    settings.flags = FWPM_SESSION_FLAG_DYNAMIC;
    wchar_t sessionName[] = L"Winsock bounded packet loss experiment";
    settings.displayData.name = sessionName;
    DWORD error = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &settings, &session.engine);
    if (error != ERROR_SUCCESS) {
        std::printf("[loss] WFP unavailable: error=%lu. Run the client as Administrator. No filter installed.\n", error);
        return true;
    }

    FWPM_FILTER_CONDITION0 conditions[5]{};
    for (auto& condition : conditions) condition.matchType = FWP_MATCH_EQUAL;
    conditions[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    conditions[0].conditionValue.type = FWP_UINT8;
    conditions[0].conditionValue.uint8 = IPPROTO_TCP;
    conditions[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
    conditions[1].conditionValue.type = FWP_UINT32;
    conditions[1].conditionValue.uint32 = ntohl(local.sin_addr.s_addr);
    conditions[2].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    conditions[2].conditionValue.type = FWP_UINT32;
    conditions[2].conditionValue.uint32 = ntohl(remote.sin_addr.s_addr);
    conditions[3].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
    conditions[3].conditionValue.type = FWP_UINT16;
    conditions[3].conditionValue.uint16 = ntohs(local.sin_port);
    conditions[4].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    conditions[4].conditionValue.type = FWP_UINT16;
    conditions[4].conditionValue.uint16 = ntohs(remote.sin_port);

    FWPM_FILTER0 filter{};
    filter.displayData.name = sessionName;
    // 2) 클라이언트 -> 서버 방향(ACK 포함)을 버린다. 서버의 probe는 도착하지만 ACK가 돌아가지 않으므로
    //    서버 TCP는 RTO 만료 후 실제로 재전송한다. (수신 방향 차단은 로컬 TCP가 연결을 리셋해 사용하지 않는다.)
    filter.layerKey = FWPM_LAYER_OUTBOUND_TRANSPORT_V4;
    filter.subLayerKey = FWPM_SUBLAYER_UNIVERSAL;
    filter.action.type = FWP_ACTION_BLOCK;
    filter.flags = FWPM_FILTER_FLAG_SILENT_MODE;
    filter.weight.type = FWP_EMPTY;
    filter.numFilterConditions = 5;
    filter.filterCondition = conditions;
    UINT64 filterId = 0;
    error = FwpmFilterAdd0(session.engine, &filter, nullptr, &filterId);
    if (error != ERROR_SUCCESS) {
        std::printf("[loss] Filter creation failed: error=%lu. Run client as Administrator; no packet loss test started.\n", error);
        return true;
    }
    char localIp[INET_ADDRSTRLEN]{}, remoteIp[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &local.sin_addr, localIp, sizeof(localIp));
    inet_ntop(AF_INET, &remote.sin_addr, remoteIp, sizeof(remoteIp));
    std::printf("[loss] ACTIVE %us: dropping outbound TCP %s:%u -> %s:%u incl. ACKs (filter=%llu).\n",
        seconds, localIp, ntohs(local.sin_port), remoteIp, ntohs(remote.sin_port), filterId);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (connected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    error = session.Close(); // dynamic 세션 종료가 이 세션의 필터를 제거한다.
    if (error != ERROR_SUCCESS) {
        std::printf("[loss] Filter session close failed: error=%lu. Closing this chat connection.\n", error);
        return false;
    }
    std::printf("[loss] RESTORED: filter removed. Next server retransmission gets ACKed; check server RETRANSMISSION logs.\n");
    return true;
}
