# 04. MIDI 라우팅 구조

## 1. 전체 흐름

```
 [SMF 가져오기 / AI 변환 / 피아노롤]
            │  (Edit 시점: tick 기반 저장, PPQ 960)
            ▼
 Prepare: tick → beat → sample 위치 변환 (TempoMap) 및 정렬 배열 생성
            │
            ▼  Audio RT 블록마다 [t0, t1) 구간 이벤트 조회 (이진 탐색 + 커서)
 ┌──────────┴──────────────────────────┐
 │ 목적지가 VSTi                         │ 목적지가 External Module
 ▼                                      ▼
 MidiBuffer(sample offset) → plugin     MIDI Out FIFO (SPSC, 64k 이벤트)
                                         │  {due_time_ns, port, bytes[3]}
                                         ▼
                                 MIDI Out Thread
                                  └ 고해상도 대기 후 sendMessageNow
                                         ▼
                                  USB MIDI / DIN 인터페이스 → 하드웨어 모듈
                                         ▼ (오디오)
                                  오디오 인터페이스 LINE IN → 믹서 입력 채널

 MIDI In (풋스위치, 키보드) → MIDI Input 콜백 → Control Mapper → Command Queue
```

## 2. 트랙 → 목적지 모델

| 필드 | 설명 |
|------|------|
| `destinationType` | `VSTi` / `ExternalModule` / `None` |
| `pluginSlotId` | VSTi일 때 플러그인 슬롯 |
| `moduleProfileId` | 외부 모듈 프로필 |
| `midiChannel` | 1–16 (트랙 단위로 덮어쓰기 가능) |
| `transpose`, `velocityScale` | 비파괴 변환 |
| `delayOffsetMs` | 트랙별 추가 보정 (±) |

하나의 외부 모듈을 여러 트랙이 다른 채널로 공유할 수 있다 (멀티팀버럴 모듈).

## 3. 외부 모듈 프로필 (External Sound Module Manager)

저장 항목: 장치 이름, MIDI 포트 이름(+식별자), 채널, Bank MSB(CC0), Bank LSB(CC32), Program Number, Volume CC(기본 7), Pan CC(기본 10), Sustain 사용 여부, MIDI Delay Offset(ms), 전송 순서/대기 시간.

**곡 시작 전 사전 전송 (Preset Recall)** — 곡이 "준비됨" 상태로 바뀔 때 (재생 시작 시점 아님):
```
for 각 (port, channel):
   CC0  = bankMSB      ─┐
   wait msbLsbGapMs     │ 순서는 프로필 설정 (MSB→LSB→PC 기본)
   CC32 = bankLSB       │
   wait lsbPcGapMs      │ 기본 20ms (일부 구형 모듈은 100ms 이상 필요)
   PC   = program      ─┘
   CC7 = volume, CC10 = pan
wait settleMs (기본 200ms)  → 상태 "Ready"
```
- 사전 전송이 완료되기 전에는 Live 화면에서 해당 모듈을 노란색 "Preparing"으로 표시한다.

## 4. 타이밍 및 지연 보정

오디오가 실제로 스피커에 나오는 시점 = 블록 처리 시각 + 출력 지연(output latency).
외부 모듈은 MIDI 수신 → 발음까지 지연(module latency)이 있고, 그 소리는 다시 오디오 인터페이스 입력 지연을 거친다.

```
MIDI 전송 시각 = 해당 샘플이 출력될 예정 시각 − moduleLatency + trackOffset
```
- 오디오 엔진은 **최소 1블록 + 최대 보정값만큼 앞서서(lookahead)** 외부 MIDI 이벤트를 FIFO에 넣는다. 따라서 "앞당기는" 보정도 가능하다.
- 기준 시계: 오디오 콜백 시작 시 `juce::Time::getHighResolutionTicks()`와 samplePos를 짝으로 기록 → 샘플 위치를 벽시계(ns)로 환산. 드리프트는 블록마다 갱신되는 짝으로 보정.
- MIDI Out 스레드는 `timeBeginPeriod(1)` 하에 잔여 시간 >2ms면 Sleep, 이하이면 spin-yield로 대기하여 ±1ms 이하 지터를 목표로 한다.
- **지연 측정 기능**: 테스트 노트를 보내고 LINE IN에서 온셋을 검출해 왕복 지연을 측정 → 모듈 프로필에 자동 기록 (Phase 6).

## 5. MIDI Clock / 트랜스포트 출력

- 선택 포트로 MIDI Clock(24 PPQN), Start(0xFA)/Stop(0xFC)/Continue(0xFB), Song Position Pointer(0xF2) 전송.
- 클럭 틱 시각은 TempoMap에서 계산하여 일반 이벤트와 같은 FIFO로 전송. `clockOffsetMs` 별도 설정.

## 6. Note 추적과 복구 (MIDI Note Recovery)

- `ActiveNoteTracker[port][channel][128]` 비트셋 + Sustain 상태를 RT에서 유지 (고정 크기 배열, 할당 없음).
- Stop / Pause / Locate / 곡 전환 / 루프 점프 / 포트 분리 / 비상 정지 시: 추적 중인 노트에 Note Off, CC64=0, 그리고 CC123(All Notes Off).
- Locate(큐 점프) 시 점프 지점에서 이미 눌려 있어야 할 긴 노트(패드 등)는 옵션에 따라 재발음(chase)한다. Program/Bank/CC도 직전 값으로 chase.

## 7. 포트 관리 (MIDI Device Manager)

- 1초 주기로 `MidiOutput::getAvailableDevices()` 비교 (Message 스레드).
- 포트 사라짐: 해당 프로필 상태 "Disconnected", MIDI Out 스레드는 해당 포트 이벤트 폐기, 로그.
- 같은 이름/식별자의 포트 재등장: 재오픈 → Preset Recall 재전송 → 상태 "Ready".
- Windows는 기본 WinMM 백엔드. 같은 포트를 다른 프로그램이 점유한 경우 오류 메시지로 안내.

## 8. 풋스위치 / MIDI 컨트롤 매핑

| 입력 | 기본 매핑 (변경 가능) |
|------|---------------------|
| CC 64 / Note 60 | PLAY/PAUSE 토글 |
| CC 65 / Note 62 | NEXT SONG |
| CC 66 / Note 64 | PREVIOUS SONG |
| CC 67 | 다음 큐 포인트 |
| 지정 CC 두 번 연속(500ms 이내) | EMERGENCY STOP (오조작 방지) |

- 학습 모드(MIDI Learn)로 지정. 매핑은 전역 설정 + 프로젝트별 덮어쓰기.
- 디바운스 50ms. 입력 처리는 MIDI 입력 콜백에서 Command Queue로만 전달한다.

## 9. 구현 메모 (Phase 4, 2026-11-16)

- 코드
  - `src/core/Midi`: 960 PPQ 이벤트, 템포맵 변환, Note 추적, Preset Recall 순서 (JUCE 비의존, 단위 테스트)
  - `src/engine/AudioEngine`: 오디오 스레드에서 MIDI 이벤트를 SPSC 큐로 넘김 (할당 0)
  - `src/midi/MidiOutputService`: MIDI Out 스레드와 포트 관리
  - `src/midi/MidiFileImport`: SMF 읽기
  - `src/ui/ModulePanel`: 모듈 화면 (Ctrl+M / 좌측 MIDI MODULES)
- 전송 시각
  - 계산: 오디오 콜백 시작 시각 + 블록 안의 위치 + 출력 지연(드라이버 보고값) − 모듈 지연(`delay_offset_ms`)
  - MIDI 스레드 동작: 2 ms보다 멀면 1 ms씩 잠들고, 가까우면 양보(yield)하며 기다린 뒤 최대 0.1 ms 일찍 보낸다. Windows에서는 `timeBeginPeriod(1)`을 쓴다
  - 같은 시각의 이벤트 순서: Note Off → 기타 → Note On
- 알려진 한계 (Phase 6에서 개선함, §10): 기준 시각이 콜백마다 측정한 벽시계라 콜백 지터가 그대로 MIDI 지터가 된다
- 포트 관리
  - 1초마다 목록을 확인한다. 포트는 식별자로 먼저 찾고, 없으면 이름으로 찾는다 (USB 구멍을 바꾸면 식별자가 바뀔 수 있다)
  - 포트가 사라지면 DISCONNECTED로 표시하고, 그 포트로 갈 이벤트는 버리고 개수를 센다
  - 포트가 돌아오면 다시 열고 CC64 0과 CC123을 보낸 뒤 Preset Recall을 하고, PREPARING → settle 시간 후 READY가 된다
  - 포트가 있지만 열 수 없으면 CANNOT OPEN으로 표시하고 1초마다 다시 시도한다 (다른 프로그램이 점유 중이거나, GS Wavetable처럼 오디오 출력이 없으면 열리지 않는 포트)
  - 한 포트를 여러 모듈 프로필이 공유하면(같은 인터페이스, 다른 채널) 장치 객체 하나를 같이 쓴다. Windows는 한 포트를 한 번만 열 수 있기 때문이다
- Note 복구
  - 엔진: Pause / Locate / 비상 정지 / 정지 페이드 끝 / 곡 끝 / 곡 교체 때 울리는 음마다 Note Off를 보내고, 쓴 채널마다 CC64 0과 CC123을 보낸다
  - MIDI 스레드: 자기가 보낸 메시지를 따로 추적한다. 오디오 장치가 끊겨 엔진이 멈추거나 비상 정지가 걸리면 `panic()`이 아직 보내지 않은 Note On을 버리고, 위와 같은 해제 메시지를 즉시 보낸다
- 트랙
  - `.mid` 파일을 타임라인에 끌어 놓으면 EXT MIDI 트랙이 된다. 기존 EXT MIDI 트랙 위에 놓으면 파일을 교체하고, 다른 곳에 놓으면 새 트랙을 만든다
  - 파일의 박자는 곡 템포맵 위에 놓인다: 파일의 1마디 1박 = 곡의 시작
  - 모든 이벤트는 트랙 채널로 보낸다. 트랙 채널이 0이면 모듈의 채널을 쓴다
  - SysEx와 메타 이벤트는 실시간으로 보내지 않는다. SMPTE 시간 형식 파일은 거부한다
- 트랙 페이더는 GM 곡선(dB = 40·log10(cc/127))으로 모듈 볼륨 CC를 보낸다. 모듈 프로필의 볼륨 값이 0 dB 기준이다
- 모듈 프로필에서 서스테인을 끄면 그 모듈로 가는 CC64를 걸러 낸다 (음이 걸리는 장비용)
- Live Mode: 모듈 편집은 잠기고, TEST NOTE와 SEND PRESET은 그대로 쓸 수 있다. 공연 화면 상태줄에 끊긴 모듈 이름이 빨간색으로 표시된다
- 아직 없는 것
  - MIDI Clock 출력 (Phase 6)
  - 모듈 오디오 리턴(LINE IN) 경로와 모니터링 (Phase 7 믹서)
  - 곡 시작 전 Preset Recall 자동 실행: 지금은 포트가 열릴 때와 프로필이 바뀔 때, 그리고 SEND PRESET을 누를 때만 보낸다. 곡별 프리셋은 Phase 13 세트리스트와 함께 다룬다
  - 풋스위치 입력 (Phase 12)

## 10. 구현 메모 (Phase 6, 2026-11-18)

- 오디오 시계 → 벽시계 (`core/SampleClock`)
  - 콜백 시각을 그대로 쓰지 않고, 샘플 수와 측정 시각으로 DLL(지연 고정 루프, 대역폭 0.5 Hz)을 돌려 "이 블록의 첫 샘플이 원래 나와야 할 시각"을 구한다
  - 시뮬레이션에서 콜백 지터 ±1 ms → MIDI 기준 시각 흔들림 약 ±0.3 ms. 장치 시계가 0.05% 어긋나도 따라간다
  - 2블록 + 2 ms 이상 어긋나면(드롭아웃, 드라이버 재시작) 다시 잡고 횟수를 센다 (`getClockRelockCount`)
- 느린 모듈의 lookahead: 모듈 지연(`delay_offset_ms`)이 오디오 출력 지연보다 크면, 그만큼 앞선 이벤트를 미리 큐에 넣는다. 그래서 출력 지연이 짧은 설정(작은 버퍼)에서도 느린 모듈의 음이 늦지 않는다
  - 엔진이 CC123을 보내면 MIDI 스레드는 같은 포트·채널에서 아직 보내지 않은 Note On을 버린다. 미리 넣어 둔 음이 정지 뒤에 울리지 않게 하는 규칙이다 (테스트로 확인: 이 규칙을 빼면 실패)
- MIDI Clock 출력
  - 모듈 프로필의 "Send MIDI Clock". 포트마다 하나만 보낸다
  - 재생 시작: 위치가 0이면 Start(0xFA), 아니면 다음 16분음표에서 Song Position Pointer + Continue(0xFB). 그 뒤 템포맵을 따라 24 PPQN으로 F8을 보낸다
  - 정지·일시정지·이동·비상 정지 때 Stop(0xFC). 이동 후에는 새 위치의 SPP + Continue
  - `Clock ms`(±50 ms): 클럭만 더 일찍(+) 또는 늦게(-) 보낸다. 시퀀서나 이펙터마다 반응이 달라서 따로 둔다
  - 스키마 v4에 저장 (08 문서)
- Locate chase (§6)
  - 곡을 열 때 트랙마다 chase 색인을 만든다 (컨트롤 이벤트 목록, 음 길이 구간)
  - 이동 후 재생, 일시정지 후 재생: 그 위치 직전의 Bank/Program/CC/Pitch Bend를 채널마다 보내고, 그 위치에 눌려 있어야 할 음 중 0.25초 이상 남은 음만 다시 친다
  - 오디오 스레드 안에서 탐색 범위(8192)와 보낼 메시지 수(512)를 제한해 할당 없이 끝낸다
  - 뮤트된 트랙은 컨트롤만 보내고 음은 치지 않는다. 비상 정지 페이드 중에는 하지 않는다
  - 외부 모듈과 VSTi 모두 같은 방식
- 플러그인 지연 보정(PDC): 05 문서 §4 참고. 오디오 트랙을 늦추지 않고, 지연이 있는 VSTi에 MIDI를 그만큼 먼저 넣는다
- 지연 측정 (`core/LatencyProbe`)
  - 루프백(오디오 설정 창 LATENCY): 인이어 출력에 -12 dBFS 짧은 틱을 내고, 케이블로 연결한 입력에서 시작점을 찾는다. 5번 재고 중앙값을 쓴다. 서로 1 ms 이상 다르면 실패
  - 모듈(MIDI MODULES 창 MEASURE DELAY): 모듈 채널로 테스트 음(10번 채널이면 38번 스네어, 아니면 60번)을 보내고, 모듈 출력이 연결된 입력에서 소리 시작점을 찾는다. 모듈 지연 = 측정값 - 왕복 지연. 결과를 모듈의 Delay ms에 바로 쓰고 이전 값을 함께 보여 준다
  - 왕복 지연을 먼저 재 두면 그 값을 쓰고, 없으면 드라이버가 알려 준 입출력 지연을 쓴다(덜 정확하다고 표시)
  - 안전 규칙: 정지 중, Live Mode 밖에서만 시작한다. PA 출력으로는 틱을 내지 않는다. 재생·이동·비상 정지·출력 테스트가 시작되면 즉시 취소한다. 입력이 시끄럽거나(-20 dBFS 이상) 소리가 줄지 않으면 실패로 보고한다
  - 입력이 열려 있어야 한다. ASIO와 JACK은 입력도 연다. Windows의 다른 드라이버(WASAPI 등)는 출력만 열어서 측정할 수 없다

## 11. MIDI 에디터 (Phase 11, 2026-11-18)

- 모델 (`core/MidiNoteEdit`): 트랙의 틱 이벤트를 노트(Note On + 짝 Note Off)와 나머지 이벤트로 나눈다
  - 짝 찾기는 채널·음 높이마다 먼저 들어온 순서대로. Note Off가 없는 음은 트랙 마지막 이벤트까지(최소 1박). 짝 없는 Note Off는 버린다
  - 노트마다 바뀌지 않는 id가 있어서 정렬해도 선택이 유지된다
  - 다시 이벤트로 바꿀 때: 같은 틱에서는 Note Off → 컨트롤/프로그램 → Note On 순서. 같은 채널·음이 겹치면 앞 음을 뒤 음 시작에서 끊는다
  - 묶음 이동은 하나라도 0~127, 0틱 밖으로 나가면 묶음 전체의 이동량을 줄인다 (모양이 깨지지 않게)
  - 최소 길이는 128분음표(30틱)
- 저장 (`AppState::saveMidiEdit`): 새 파일을 쓰고 다시 읽어 확인한 뒤 트랙의 `midi_path`를 바꾼다. 스키마 변경 없음
  - 파일 이름은 원래 이름을 따른다. AI 파트는 파일 이름(bass.mid)으로 찾기 때문이다
  - 프로젝트가 없으면 앱 데이터 폴더의 `midi-edits/`에 쓴다
- 엔진 쪽 변화 없음: 저장하면 재생 계획을 다시 만들고(현재 곡일 때만), 재생 중에는 저장을 막는다
- 명령줄(테스트용): `--midi-edit "<트랙 이름>" --midi-ops all,grid:240,quantize,transpose:12,first,vel:-10,delete,undo,redo,add:<틱>:<음>:<길이>:<벨로시티>,move:<틱>,play,stop,live,try-save,save`
