# 01. 시스템 아키텍처

B4ND LIVE MTR의 최상위 설계 원칙과 모듈 구조를 정의한다.

## 1. 설계 원칙 (우선순위 순)

1. **공연 중 끊김 없는 재생** — 다른 모든 기능보다 우선한다.
2. **실시간 경로와 비실시간 경로의 완전 분리** — 오디오 콜백은 할당·락·파일 I/O·로그 출력을 하지 않는다.
3. **AI 분석은 별도 프로세스** — Python 분석기는 MTR 본체와 다른 프로세스로 실행되며, Live Mode에서는 실행 자체가 차단된다.
4. **준비(Prepare)와 재생(Play)의 분리** — 곡 재생에 필요한 미디어 디코딩, 플러그인 인스턴스 생성, 외부 모듈 프리셋 전송은 모두 재생 전에 끝낸다.
5. **안전한 기본값** — 클릭/가이드는 메인 PA로 나가지 않는 것이 기본이며, 재생 중 장치 자동 전환을 하지 않는다.
6. **실패는 국소화** — 플러그인·MIDI 포트·미디어 파일 하나의 실패가 전체 재생을 멈추지 않는다.

## 2. 프로세스 구성

```
┌──────────────────────────────────────────────────────────────┐
│ B4ND_LIVE_MTR.exe  (C++ / JUCE, 실시간 재생 본체)              │
│                                                              │
│  Message Thread (UI)   Audio RT Thread   MIDI Out Thread      │
│  Prepare Worker        Disk/Prefetch     Monitor/Logger       │
└───────────▲──────────────────────────────────────────────────┘
            │ JSON Job 파일 + stdout 진행률 (로컬 전용, 네트워크 없음)
            ▼
┌──────────────────────────────────────────────────────────────┐
│ b4nd-analyzer (Python 3.11, 오프라인 AI 분석기)                │
│  BPM/Beat · Key · Chord · Structure · Stem · Audio→MIDI       │
└──────────────────────────────────────────────────────────────┘

(Phase 5b 선택) b4nd-pluginhost.exe : 불안정한 VST3를 격리 실행하는 보조 프로세스
```

- 분석기는 MTR이 자식 프로세스로 띄우며 (`juce::ChildProcess`), 결과는 프로젝트 패키지 폴더에 파일로 남긴다. MTR은 분석기 크래시와 무관하게 동작한다.
- Live Mode 진입 시 실행 중인 분석 작업은 일시정지 또는 취소를 요구하며, Live Mode 동안 분석 작업 시작 버튼은 비활성화된다.

## 3. 17개 모듈과 책임

| # | 모듈 | 책임 | 실행 스레드 | 도입 Phase |
|---|------|------|-------------|-----------|
| 1 | Application UI Engine | 메인 창, 다크 테마, 레이아웃, 단축키 | Message | 1 |
| 2 | Audio Engine | 오디오 콜백, 트랙 렌더링, 타임라인 진행 | Audio RT | 2–3 |
| 3 | ASIO Device Manager | 백엔드 검색(ASIO→WASAPI→DirectSound), 설정, 끊김 감지 | Message | 2 |
| 4 | Multi Output Routing Engine | 버스→물리 출력 매트릭스, 클릭 보호 | Audio RT(적용) / Message(편집) | 1(모델)·3 |
| 5 | MIDI Engine | 타임스탬프 이벤트 스케줄링, Clock, Note 추적 | Audio RT + MIDI Out | 4, 6 |
| 6 | MIDI Device Manager | 포트 검색, 핫플러그, 재연결 | Message | 4 |
| 7 | VST3 Plugin Host | 스캔, 인스턴스, 상태 저장, GUI | Prepare / Audio RT | 5 |
| 8 | External Sound Module Manager | 모듈 프로필, Bank/Program 사전 전송, 지연 보정 | Prepare / MIDI Out | 4 |
| 9 | Audio Mixer Engine | 채널 스트립, 버스, 미터, 리미터 | Audio RT | 7 |
| 10 | AI Song Analysis Engine | BPM/Key/Chord/구조 (Python) | 별도 프로세스 | 8 |
| 11 | Stem Separation Engine | Demucs 기반 분리 (Python) | 별도 프로세스 | 9 |
| 12 | Audio to MIDI Engine | 악기별 MIDI 변환 (Python) | 별도 프로세스 | 10 |
| 13 | MIDI Editor | 피아노롤 | Message | 11 |
| 14 | Live Performance Engine | 공연 화면, 풋스위치, 큐 포인트 | Message + RT 명령 | 12 |
| 15 | Setlist Manager | 세트리스트, 다음 곡 사전 준비 | Message + Prepare | 13 |
| 16 | Project Manager | SQLite, 패키지, 자동 저장, 백업 | Message + IO Worker | 1(골격)·3 |
| 17 | Logging and Recovery Engine | 로그, 언더런·CPU·메모리 감시, 복구 | Logger / Monitor | 1(로그)·14 |

## 4. 모듈 간 통신 규칙

```
            ┌──────────── Command Queue (SPSC, lock-free) ───────────┐
UI/Message ─┤                                                         ├─▶ Audio RT
            └──── PlaybackPlan 포인터 교체 (atomic, 재생 정지 시점) ──┘
Audio RT ──▶ Telemetry (atomics: 미터, 위치, 언더런 카운터) ──▶ UI 30Hz 폴링
Audio RT ──▶ MIDI Event FIFO (SPSC, 타임스탬프 포함) ──▶ MIDI Out Thread
Audio RT ──▶ RT Log FIFO (고정 크기 레코드) ──▶ Logger Thread ──▶ 로그 파일
```

- **Command**: Play / Stop / Pause / Locate / EmergencyStop / SetGain 등 고정 크기 POD 구조체. UI는 큐에 넣기만 하고 결과는 Telemetry로 확인한다.
- **PlaybackPlan**: 곡 하나를 재생하는 데 필요한 모든 것(트랙 리더, 플러그인 인스턴스 포인터, MIDI 이벤트 배열, 템포맵, 라우팅 행렬)을 Prepare 스레드가 미리 만들어 두고, 오디오 스레드는 포인터 교체만 한다. 이전 Plan의 해제는 오디오 스레드가 아닌 GC 스레드에서 한다.
- **Telemetry**: 오디오 스레드가 쓰고 UI가 읽는 `std::atomic` 값들. 락 없음.

## 5. 동작 모드

| 모드 | 허용 작업 | 차단 작업 |
|------|-----------|-----------|
| Edit Mode | 모든 편집, AI 분석, 플러그인 스캔 | — |
| Rehearsal Mode | 재생, 믹스 조절, 큐 포인트 편집 | AI 분석(경고 후 허용) |
| Live Mode | 재생 제어, 볼륨/뮤트, 곡 전환 | AI 분석, 플러그인 스캔/추가/삭제, 장치 변경(확인 필요), 라우팅 변경 |

## 6. 실패 처리 전략 요약

| 실패 | 동작 |
|------|------|
| 오디오 장치 분리 | 재생 정지, 모든 MIDI Note Off 전송, 붉은 배너 표시, 같은 장치 재연결 대기. 다른 장치로 자동 전환하지 않음 |
| 언더런(xrun) | 카운트·로그, 상태바 경고. 재생은 계속 |
| MIDI 포트 분리 | 해당 트랙만 비활성, 재연결 시 프로그램/뱅크 재전송 후 복귀 |
| 플러그인 예외/크래시 | 해당 채널 무음 처리 및 표시 (in-process 한계는 05 문서 참고) |
| 미디어 누락 | 프로젝트 열 때 검사, Live Mode 진입 차단 |
| 프로그램 비정상 종료 | 다음 실행 시 자동 저장본 복구 제안 |

## 7. 기술 스택 및 라이선스 메모

| 구성 | 선택 | 메모 |
|------|------|------|
| 프레임워크 | JUCE 8.0.x | AGPLv3 또는 상용 라이선스. 내부 사용/비배포면 AGPL로 충분, 외부 배포 시 검토 필요 |
| 빌드 | CMake 3.22+, MSVC 2022 (x64) | Linux 빌드는 개발·CI 검증용 |
| ASIO | Steinberg ASIO SDK | JUCE에 포함되지 않음. 사용자가 SDK를 받아 `B4ND_ASIO_SDK_DIR`로 지정 |
| VST3 | JUCE 내장 VST3 SDK | — |
| DB | SQLite 3 (amalgamation 내장) | Public domain |
| AI | Python 3.11 + PyTorch | 모델별 라이선스는 07 문서 |
