#pragma once
#include "ChatCommon.h"
#include <atomic>

// 현재 연결의 클라이언트 -> 서버 패킷(ACK 포함)만 N초 동안 실제로 버린다.
// false는 송신/필터 해제 실패로 현재 연결을 종료해야 함을 뜻한다.
bool RunPacketLossTest(SOCKET connection, unsigned seconds, const std::atomic<bool>& connected);
