# LAN 채팅과 TCP 로그

`build/Chat/Winsock.exe server`를 실행하면 채팅과 TCP 소켓 로그가 함께 표시됩니다.
각 로그의 `user번호 IP:포트`로 연결을 구별합니다. user번호는 접속 순서이며 사람의 이름이 아닙니다.

- `ACCEPT`: OS가 연결을 성립시킨 뒤 서버가 연결을 받았습니다.
- `SEND`: Winsock에 넘긴 바이트 수입니다. 상대 수신이나 ACK 도착을 보장하지 않습니다.
- `RECV`: 앱이 읽은 스트림 바이트 수입니다. TCP 패킷 하나의 크기와 같다는 뜻이 아닙니다.
- `RECV EOF`: 상대 송신 방향의 정상 종료를 확인했습니다.
- `SHUTDOWN`: 서버 송신 방향을 닫았습니다.
- `CLOSE`: 소켓을 해제했습니다. 마지막 ACK 수신이나 TIME_WAIT 완료를 확인했다는 뜻이 아닙니다.
- `WSA=10054`: 연결이 리셋되었습니다. 정상 종료와 구분합니다.

## 실제 SYN / ACK / FIN 보기

소켓 API는 핸드셰이크 패킷을 직접 반환하지 않습니다. 아래는 Windows 기본 도구인
Packet Monitor로 서버 PC에서 실제 LAN 패킷을 관찰하는 방법입니다.
클라이언트가 접속하기 **전에** 관리자 PowerShell에서 실행하세요.

```powershell
pktmon status
pktmon filter list
pktmon filter add WinsockChat27017 -t TCP -p 27017
pktmon start --capture --comp nics --pkt-size 128 --log-mode real-time
```

이미 다른 캡처가 실행 중이면 먼저 그 작업을 마친 후 진행하세요.
기존 필터가 있으면 해당 필터의 트래픽도 함께 나올 수 있습니다.
실시간 모드는 파일을 만들지 않습니다. 다른 LAN PC에서 채팅 클라이언트를 접속하고
메시지를 보낸 후 `/quit`으로 종료하세요. 같은 PC의 루프백 통신은 NIC 캡처에 보이지 않을 수 있습니다.
캡처 종료는 Ctrl+C이며, 종료 후 이 작업의 필터만 지웁니다.

```powershell
pktmon filter remove WinsockChat27017
```

일반적인 연결 흐름(설명용이며 서버가 출력한 실측 패킷 로그가 아닙니다):

```text
클라이언트 → 서버   SYN
서버 → 클라이언트   SYN, ACK
클라이언트 → 서버   ACK
```

클라이언트가 먼저 정상 종료하는 일반적인 흐름:

```text
클라이언트 → 서버   FIN, ACK
서버 → 클라이언트   ACK
서버 → 클라이언트   FIN, ACK
클라이언트 → 서버   ACK
```

ACK와 FIN이 합쳐지거나 재전송이 발생할 수 있으므로 종료가 항상 정확히 네 패킷으로 보이지는 않습니다.
강제 종료나 오류에서는 RST가 나타날 수 있습니다. 서버 로그의 접속자 IP와 임시 포트를
패킷의 주소·포트와 맞춰 보세요. NIC가 여러 개이면 같은 패킷이 중복 표시될 수도 있습니다.

## 파일로 저장하기

실시간 캡처 대신 다음을 실행합니다. 아래 파일 이름은 실행마다 바꿔 기존 캡처를 보존하세요.
필터는 위와 같이 먼저 등록하고, 접속 전 캡처를 시작합니다.

```powershell
pktmon start --capture --comp nics --pkt-size 128 --file-name chat-tcp.etl --file-size 32
# 다른 LAN PC에서 접속, 채팅, /quit 실행 후:
pktmon stop
pktmon etl2txt chat-tcp.etl --out chat-tcp.txt --verbose
pktmon etl2pcap chat-tcp.etl --out chat-tcp.pcapng
pktmon filter remove WinsockChat27017
```

텍스트 파일로 보거나, Wireshark가 있다면 pcapng 파일을 열고 `tcp.port == 27017`로 필터링합니다.

참고: [Microsoft Packet Monitor 명령](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/pktmon-start),
[Winsock 정상 종료](https://learn.microsoft.com/en-us/windows/win32/winsock/graceful-shutdown-linger-options-and-socket-closure-2).

## 실제 패킷 손실과 재전송 관찰 (`/loss`)

클라이언트를 **관리자 권한**으로 실행한 뒤 채팅창에 `/loss [1-5]`를 입력합니다(기본 3초).

1. 클라이언트가 서버에 `/loss-probe N`을 보냅니다.
2. 클라이언트가 WFP 필터로 **이 연결의 클라이언트 → 서버 패킷(ACK 포함)** 을 N초 동안 실제로 버립니다.
   다른 접속자와 다른 연결은 영향을 받지 않습니다. 필터는 dynamic 세션이라 프로그램이 죽어도 자동으로 제거됩니다.
3. 서버는 500ms 뒤 probe 데이터(270바이트)를 보냅니다. 데이터는 도착하지만 ACK가 돌아오지 않으므로
   서버 OS의 TCP가 RTO 만료 후 같은 데이터를 **재전송**합니다(간격이 두 배씩 늘어나는 backoff).
4. 서버는 250ms마다 `SIO_TCP_INFO`의 `BytesRetrans`/`TimeoutEpisodes` 카운터를 읽어 증가하면
   `RETRANSMISSION detected`를 출력하고, N+10초 후 `LOSS-TEST observation complete`로 결과를 요약합니다.

```text
[TCP ... user1 ...] LOSS-TEST requested: probe in 500ms; ...
[TCP ... user1 ...] LOSS-TEST probe: sending 270 bytes; expecting ACK loss -> RTO retransmission
[TCP ... user1 ...] RETRANSMISSION detected: bytes_delta=294, bytes_total=342, timeout_episodes_delta=0, ...
[TCP ... user1 ...] LOSS-TEST observation complete: retransmitted_bytes=342, ... (retransmission observed)
```

수신 방향(서버 → 클라이언트)을 막지 않는 이유: Windows에서는 inbound transport 차단 시 로컬 TCP가
연결을 리셋(WSA=10054)해 재전송을 관찰할 수 없었습니다. 위 `pktmon` 캡처를 함께 켜 두면 같은 시퀀스 번호의
세그먼트가 반복 전송되는 것을 패킷 수준에서도 확인할 수 있습니다. 자동 검증: `Tests/PacketLoss.Smoke.ps1`.
