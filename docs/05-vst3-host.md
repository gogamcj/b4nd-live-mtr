# 05. VST3 Host 구조

## 1. 구성

```
PluginManager
 ├─ PluginScanner      (별도 프로세스로 스캔: b4nd-live-mtr.exe --scan <path>)
 ├─ KnownPluginList    (%APPDATA%/plugin-cache.xml, 블랙리스트 포함)
 ├─ PluginSlotPool     (세트리스트 전체에서 필요한 인스턴스 풀)
 │    └─ PluginSlot    { instance, state blob, bus layout, latency, status }
 ├─ PresetLibrary      (악기 프리셋: Piano / EP / Organ / Strings / Brass / Pad / Lead / Choir / Orchestra …)
 └─ PluginEditorHost   (플러그인 GUI 창, Message 스레드)
```

## 2. 스캔과 검사

- 스캔은 **자식 프로세스**에서 수행한다. 플러그인 하나가 스캔 중 크래시해도 본체는 안전하며, 해당 플러그인은 블랙리스트에 기록된다 (JUCE `PluginDirectoryScanner` + 크래시 시 dead-man's-pedal 파일).
- 검사(Validation) 항목: 로드 성공, 버스 레이아웃(스테레오/멀티아웃), 48k/96k에서 `prepareToPlay` 성공, 무음 블록 100회 처리 시간 측정, 상태 저장/복원 왕복 일치.
- 검사를 통과한 플러그인에만 **"Live Certified"** 표시. Live Mode는 Certified가 아닌 플러그인이 포함된 곡에 경고를 띄운다.

## 3. 인스턴스 수명

| 시점 | 동작 |
|------|------|
| 프로젝트/세트리스트 로드 | 필요한 모든 인스턴스 생성 → 상태 복원 → `prepareToPlay` (Prepare 스레드, 메시지 스레드 필요한 플러그인은 메시지 스레드) |
| 곡 전환 | 인스턴스는 유지. 슬롯에 해당 곡 프리셋 상태를 **정지 상태에서** 복원하거나, 곡마다 별도 인스턴스를 미리 준비 (설정: 메모리 vs 전환 속도) |
| 공연 중 | 생성/삭제 없음. 바이패스/뮤트만 허용 |
| 종료 | 상태 저장 후 해제 |

- 기본 전략은 "곡별 인스턴스 사전 생성"(전환 시 상태 복원 지연 없음). 메모리가 부족하면 동일 플러그인·동일 프리셋을 공유한다.
- 샘플 기반 대형 악기(Kontakt 등)는 로딩 완료 신호를 확인할 수 없는 경우가 있으므로, 준비 단계에서 무음 블록을 일정 시간 처리하며 CPU가 안정되는지 확인 후 Ready로 표시한다.

## 4. 오디오/MIDI 연결

- MIDI 입력: 트랙의 MIDI 이벤트(04 문서) → 블록별 `MidiBuffer`.
- 실시간 MIDI 입력(키보디스트의 마스터 키보드)도 선택한 슬롯으로 전달 가능 (라이브 연주용).
- MIDI 출력: 플러그인이 생성한 MIDI(아르페지에이터 등)는 선택적으로 외부 포트로 전달.
- 오디오 출력: 플러그인 메인 출력 → 채널 스트립(볼륨/팬/인서트) → 지정 버스. 멀티아웃 플러그인은 출력 쌍마다 채널 스트립 생성.
- 인서트 이펙트: 채널당 최대 4슬롯, 역시 사전 생성.
- 플러그인 지연(PDC): 오디오 트랙을 플러그인 지연만큼 지연시켜 정렬 (Phase 6).

## 5. 크래시 격리 전략 (현실적인 한계 포함)

| 수준 | 방식 | 장점 | 비용 |
|------|------|------|------|
| L0 (기본) | In-process + 방어 코드 | 지연 추가 없음 | 플러그인의 메모리 손상/크래시는 전체 앱 종료로 이어질 수 있음 |
| L1 | SEH(`__try/__except`)로 process 호출 감싸기 → 해당 슬롯 영구 무음 처리 | 일부 접근 위반을 버팀 | 손상된 힙은 복구 불가, 완전한 보호 아님 |
| L2 (Phase 5b) | 플러그인 별도 프로세스(`b4nd-pluginhost.exe`) + 공유 메모리 오디오 | 플러그인 크래시가 본체에 영향 없음 | 1블록 추가 지연, 구현 복잡도 |

**결정**: Phase 5는 L0+L1로 구현하고, Live Certified 검사와 장시간 테스트로 불안정 플러그인을 사전에 걸러낸다. 실제 사용 중 크래시 사례가 나오면 L2를 Phase 5b로 진행한다. 또한 본체 크래시 대비로 **자동 저장 + 빠른 재시작(마지막 곡/위치 복원)**을 제공한다 (09, 10 문서).

## 6. 상태 저장

- ~~`getStateInformation()` 결과를 `plugins/states/<slot-uuid>.vststate`로 저장하고 DB에 해시와 함께 기록.~~
  **변경 (Phase 5)**: 상태는 DB의 `plugin_slot.state_blob`에 직접 저장한다. 별도 파일이면 백업(.db 복사)과 저장 트랜잭션에 포함되지 않아, 정전 시 DB와 상태 파일이 서로 다른 시점이 될 수 있기 때문이다.
- 프로젝트를 다른 PC에서 열 때 플러그인이 없으면 "Missing Plugin"으로 표시하고 해당 트랙은 무음, 대체 플러그인 지정 가능.
- 플러그인 바이너리·상용 샘플 라이브러리는 패키지에 포함하지 않는다 (라이선스).

## 7. Phase 5 구현 결과 (2026-11-16)

구현한 구조 (§1의 이름과 대응):

| 설계 | 구현 |
|------|------|
| PluginScanner | `b4nd_plugin_scanner(.exe)`: 앱 옆에 있는 별도 실행 파일. `--scan <플러그인> --out <xml>` 으로 파일 하나를 로드하고 검사한 뒤 보고서를 쓴다. 앱의 `PluginScanner` 스레드가 파일마다 이 프로그램을 실행한다 |
| KnownPluginList | `PluginCatalog`: `%APPDATA%/B4ND LIVE MTR/plugin-cache.xml` (임시 파일에 쓴 뒤 교체). 상태 Certified / NotCertified / Blacklisted, 부하 측정값, 파일 수정 시각 |
| PluginSlotPool | 트랙마다 인스턴스 하나 (`TrackInfo::instrument`). 프로젝트를 열 때 세트리스트 전체의 인스턴스를 만든다 (곡 전환 시 생성 없음) |
| PluginEditorHost | `PluginHost::showEditor` (메시지 스레드 창). Live Mode에 들어가면 모두 닫히고 열 수 없다 |
| PresetLibrary | 미구현 (이후 단계) |

**스캔과 블랙리스트**
- 보고서에 `complete="1"`이 없으면(스캐너가 죽음) "crashed while loading", 120초 안에 끝나지 않으면 "hung"으로 블랙리스트에 올린다. 블랙리스트 플러그인은 앱 안으로 절대 로드하지 않는다.
- **스캔한 적이 없는 플러그인도 앱 안으로 로드하지 않는다.** 다른 PC에서 만든 프로젝트를 열었는데 플러그인이 설치는 되어 있지만 스캔 전이면 "NOT SCANNED"로 표시하고 무음이다. 스캔이 끝나면 자동으로 다시 로드한다.
- 같은 uid가 없으면 이름 + 제조사가 같은 플러그인을 찾는다 (버전/PC가 달라도).

**Live Certified 검사** (스캐너 프로세스 안에서)
- 오디오 출력이 있을 것
- 48 kHz, 96 kHz (256 샘플)에서 `prepareToPlay` 성공
- 노트를 넣은 120블록 처리, 처음 20블록은 워밍업으로 제외, 평균 처리 시간이 실시간의 50% 이하
- 출력에 NaN / Inf 없음
- `getState → setState → getState` 결과가 같을 것

**실행 중 안전 장치**
- `PluginInstrument::render`는 잠금을 *시도*만 한다. 메시지 스레드가 장치 변경으로 플러그인을 다시 준비하는 동안에는 그 플러그인만 무음이다. 오디오 스레드는 기다리지 않는다.
- L1 격리: Windows에서 `processBlock`을 SEH로 감싸고, 모든 OS에서 C++ 예외를 잡는다. 오류가 난 플러그인은 "PLUGIN FAILED"로 표시되고 무음이 되며, 다른 트랙은 계속 재생된다. 메모리를 망가뜨린 크래시는 막지 못한다 (L2가 필요).
- 출력 정리: NaN/Inf는 0으로, 값은 ±4로 제한한다 (PA 보호).
- 플러그인 상태는 **정지 상태에서만** 읽는다. 일부 플러그인은 재생 중 `getStateInformation` 호출에 소리가 끊긴다. 재생 중 편집기에서 바꾼 값은 정지 후 저장된다.
- 렌더 경로의 할당 없음: 미리 잡은 MidiBuffer와 버퍼 뷰를 쓴다. 테스트는 *호스트 쪽* 할당만 셀 수 있다 (플러그인 내부 할당은 플러그인 책임이며 부하 측정으로 간접 확인).

**화면**: Plugins 창 (Ctrl+P, 사이드 패널 PLUGINS 버튼), 상태 표시줄 VSTi 항목, Live Mode의 VSTi 표시 (사용 불가 = 빨강, Certified 아님 = 노랑).

**검증 범위**
- Linux: 테스트 플러그인 4종(정상 / 로딩 중 크래시 / 너무 느림 / 상태 불량)으로 스캔, 블랙리스트, 인증, 캐시, 실제 VST3 렌더링을 자동 테스트. 앱에서 JACK 더미 장치로 VSTi 트랙 재생과 저장 후 다시 열기를 확인.
- Windows CI: 같은 테스트를 MSVC로 빌드하고 실행 (SEH 경로 컴파일 포함).
- **확인하지 못한 것**: 상용 VSTi(Kontakt, Keyscape 등)와 실제 오디오 인터페이스에서의 동작, 플러그인 편집기 창 표시(CI에는 화면이 없음), SEH가 실제 접근 위반을 버티는지. 이는 실제 PC에서 확인해야 한다.
