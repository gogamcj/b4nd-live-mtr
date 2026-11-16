# 03. 오디오 엔진 구조

## 1. 스레드 모델

| 스레드 | 우선순위 | 하는 일 | 금지 |
|--------|----------|---------|------|
| Audio RT (드라이버 콜백) | 드라이버 지정 (ASIO: 최고) | 트랙 렌더, 플러그인 process, 믹스, 미터 값 기록, MIDI 이벤트 산출 | 메모리 할당, 락 대기, 파일 I/O, 로그 문자열 생성, 예외 throw, 플러그인 생성/삭제 |
| Disk Prefetch | 높음 | 메모리 매핑 페이지 선행 접근(pre-touch), 스트리밍 링버퍼 채우기 | — |
| MIDI Out | 높음 (`THREAD_PRIORITY_TIME_CRITICAL`) | 외부 MIDI 타임스탬프 전송 | 무거운 연산 |
| Prepare Worker | 보통 | 미디어 디코딩/변환, PlaybackPlan 생성, 플러그인 인스턴스 준비 | — |
| GC | 낮음 | 교체된 Plan·버퍼 해제 | — |
| Monitor | 낮음 | CPU/메모리/장치/포트 상태 점검 (1Hz) | — |
| Message (UI) | 보통 | 화면, 사용자 입력 | 오디오 스레드 대기 |

## 2. 장치 관리 (ASIO Device Manager)

- `juce::AudioDeviceManager`는 **사용하지 않는다.** 현재 장치가 사라지면 AudioDeviceManager는 저장된 설정으로 재초기화하면서 기본 장치(노트북 스피커 등)를 자동으로 열어 버리기 때문이다. 대신 `AudioIODeviceType`/`AudioIODevice`를 직접 관리하는 `AudioDeviceController`(src/audio)를 둔다. Windows 빌드에서 `JUCE_ASIO=1`(SDK 필요), `JUCE_WASAPI=1`, `JUCE_DIRECTSOUND=1`.
- 백엔드 선택 순서: **ASIO → WASAPI(Exclusive) → WASAPI(Shared) → DirectSound**. 사용자가 고정 가능.
- 샘플레이트(44.1/48/88.2/96k)와 버퍼(64~1024)는 `AudioIODevice::getAvailableSampleRates()/getAvailableBufferSizes()`가 돌려준 값만 UI에 표시한다.
- 지연 표시: `getInputLatencyInSamples()`, `getOutputLatencyInSamples()` + 버퍼 크기 → ms.
- 장치 상태 머신:

```
 Closed ──open──▶ Running ──드라이버 오류/분리──▶ Lost
   ▲                 │                             │
   └────close────────┘◀──── 같은 장치 재연결 성공 ──┘
```
- 구현(Phase 2): 장치 목록 변경 알림, 드라이버 오류, 예기치 않은 정지, 1초 주기 감시로 `Lost`를 판단한다. ASIO 리셋 요청(드라이버 패널에서 버퍼 변경)은 장치를 잠깐 멈췄다 재시작하므로 2회 연속 정지일 때만 `Lost`로 본다.
- `Lost` 진입 시: 트랜스포트 Stop, MIDI 전 채널 All Notes Off + Sustain Off, 사용자에게 배너. **자동으로 다른 장치를 열지 않는다.** 같은 이름의 장치가 다시 나타나면 "재연결" 버튼 활성화 (Edit/Rehearsal 모드에서는 자동 재연결 옵션 허용).
- ASIO 드라이버의 `kAsioResetRequest`(버퍼 크기 변경 등)는 재생 정지 상태에서만 적용하고, 재생 중이면 곡 종료 후 적용을 예약한다.

## 3. 타임라인과 렌더링

```
audioDeviceIOCallbackWithContext(numSamples)
 ├─ 1. Command Queue 비우기 (Play/Stop/Locate/Gain…)  ← 고정 개수까지만 처리
 ├─ 2. Plan 포인터 교체 확인 (정지 상태일 때만)
 ├─ 3. 블록 시간 범위 계산 [samplePos, samplePos+numSamples)
 │      └─ TempoMap으로 beat 범위 계산 (MIDI·클릭용)
 ├─ 4. 트랙 렌더
 │      ├─ AudioTrack: 메모리 매핑 리더에서 복사 (+게인/팬)
 │      ├─ VSTi Track: 블록 내 MIDI 이벤트를 sample offset으로 MidiBuffer 구성 → plugin.processBlock
 │      ├─ ExtModule Track: MIDI 이벤트를 MIDI Out FIFO에 (전송 예정 시각 포함) push
 │      │                   + 오디오 인터페이스 LINE IN을 입력 채널 스트립으로 믹스
 │      └─ Click/Guide: 클릭 생성기 (사전 렌더된 샘플을 박 위치에 배치)
 ├─ 5. 채널 스트립 → 버스 (Main, Click, Guide, Extra, Monitor…)
 ├─ 6. 버스 → 물리 출력 매트릭스 (OutputRouting, 04/06 문서)
 ├─ 7. 미터 값(peak/RMS) atomics에 기록
 └─ 8. 소요 시간 측정 → CPU 부하 atomics, 예산 초과 시 RT 로그 레코드
```

- **샘플 위치(samplePos)가 단일 시간 기준**이다. 템포맵은 샘플 위치 ↔ 박(beat) 변환만 담당하며 오디오 트랙은 템포 변경과 무관하게 샘플 단위로 재생된다 (원곡 오디오는 원래 템포 그대로이므로, 템포맵을 원곡에 맞추는 것이 원칙).
- 재생 블록 경계에 걸친 이벤트(템포 변경, 루프 끝, 큐 점프)는 블록을 하위 구간으로 나눠 처리한다.

## 4. 미디어 준비 (재생 중 디코딩 금지)

- 가져온 MP3/FLAC/AIFF/WAV는 **Prepare 단계에서 프로젝트 샘플레이트의 WAV(32-bit float)로 변환**하여 `media/cache/`에 저장한다. 고품질 리샘플러 사용.
- 재생 시 `juce::MemoryMappedAudioFormatReader`로 매핑. 현재 곡과 다음 곡의 매핑 영역은 Prefetch 스레드가 순차 접근하여 페이지를 RAM에 올려 둔다 (페이지 폴트로 인한 RT 지연 방지). 필요시 `VirtualLock`으로 고정 (옵션).
- 메모리 예산 계산: 48kHz·스테레오·float = 약 23MB/분. 5분 곡 × 8트랙 ≈ 0.9GB. 현재+다음 곡 기준 2GB 이내를 목표로 하고, 초과 시 경고하며 스트리밍(링버퍼) 모드로 전환한다.
- 샘플레이트가 바뀌면 캐시를 재생성해야 하므로 Live Mode에서 샘플레이트 변경은 허용하지 않는다.

> **Phase 3 구현 메모 (2026-11-16)**: 첫 구현은 메모리 매핑 대신 **재생 전 RAM 전체 디코딩**을 쓴다 (`src/media/MediaLoader`).
> 백그라운드 스레드가 파일을 장치 샘플레이트로 변환해 float 배열로 메모리에 올리고, 오디오 스레드는 그 배열만 읽는다.
> 재생 중 디스크 접근이 전혀 없어서 페이지 폴트나 디스크 절전으로 인한 끊김이 원천적으로 없다. 세트리스트 전체를 미리 올려 곡 전환이 즉시 된다.
> 단점은 메모리 사용량이며(상태바 MEDIA 표시, 4GB 초과 시 경고), WAV 캐시 파일과 메모리 매핑 재생은 세트리스트가 커지면 추가한다.
> 리샘플링은 JUCE windowed-sinc 보간기를 쓴다. 다운샘플링(96k → 48k)에서 저역 통과 필터가 따로 없으므로, 가능하면 공연 샘플레이트에 맞춘 파일을 쓰는 것이 좋다.
> 언더런 감지는 장치 시작 직후 250ms를 제외한다(드라이버가 스트림 시작 시 블록을 불규칙하게 보내며, 이때 트랜스포트는 항상 정지 상태).

## 5. PlaybackPlan

```cpp
struct PlaybackPlan {          // Prepare 스레드가 만들고, RT는 읽기만
    TempoMap tempo;
    std::vector<AudioTrackRuntime> audioTracks;   // 매핑 리더, 게인
    std::vector<MidiTrackRuntime>  midiTracks;    // 정렬된 이벤트 배열 (sample 위치 사전 계산)
    std::vector<PluginSlot*>       plugins;       // 이미 prepareToPlay 완료된 인스턴스
    RoutingMatrix routing;
    ClickSettings click;
    std::vector<CuePoint> cues;
};
```
- 곡 전환: 다음 곡 Plan을 미리 완성 → 현재 곡 정지 시 (Note Off·꼬리 처리 후) 포인터 교체. 교체는 수 μs 수준의 atomic 연산.
- 플러그인 인스턴스는 세트리스트 전체 기준으로 공유/재사용하며 공연 중 생성·삭제하지 않는다 (05 문서).

## 6. 언더런 감지

- 콜백 간 실제 경과 시간과 기대 시간(numSamples/sampleRate) 비교, 1.5배 초과 시 xrun 후보로 카운트.
- 콜백 처리 시간이 버퍼 시간의 80%를 넘으면 "DSP 과부하" 카운트.
- ASIO 드라이버가 보고하는 오류(`kAsioOverload` 등)를 JUCE 오류 콜백으로 수집.
- 카운터는 atomics → UI 상태바 / 로그 파일 (곡명·위치 포함).

## 7. 비상 정지 (Emergency Stop)

1. RT: 다음 블록에서 모든 출력 0으로 페이드(5ms) 후 정지 플래그
2. 모든 VSTi에 All Notes Off/All Sound Off/Sustain Off 주입
3. 외부 MIDI: 16채널 × (CC123, CC120, CC64=0) 즉시 전송 + 추적 중인 Note On에 대해 개별 Note Off
4. UI는 키보드(Esc, 기본) / 풋스위치 / 화면 버튼에서 동일 명령 발행
5. 비상 정지 후 재생은 명시적인 PLAY로만 재개
