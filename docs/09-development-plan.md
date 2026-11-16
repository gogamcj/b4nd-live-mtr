# 09. 단계별 개발 계획

각 Phase는 **빌드 → 자동 테스트 → 수동 검증 → 오류 수정** 후 완료로 표시한다. 실패한 기능은 완료로 표시하지 않는다.
검증 환경 표기: 🐧 = 개발 클라우드(Linux)에서 검증 가능, 🪟 = Windows 실기(오디오 인터페이스·MIDI 장비) 필요.

| Phase | 내용 | 산출물 / 완료 조건 | 검증 |
|-------|------|-------------------|------|
| 1 | Windows 앱 및 UI 기본 구조 | CMake+JUCE 앱, 다크 테마, 상단/좌측/중앙/하단 레이아웃, 공연 모드 화면 틀, 단축키, 로그 파일, core 라이브러리(TempoMap, OutputRouting)와 단위 테스트, Windows CI 빌드 | 🐧 빌드·테스트·스크린샷, 🪟 CI 빌드 + 실기 실행 |
| 2 | 오디오 인터페이스 및 ASIO 엔진 | 장치 검색/선택 UI, 백엔드 우선순위, SR/버퍼 제한, 지연 표시, 장치 분리 감지, Command Queue, 테스트 톤 | 🪟 ASIO 장치 실기 |
| 3 | 멀티트랙 오디오 재생 및 독립 출력 | WAV 캐시 변환, 메모리 매핑 재생, 8채널 출력 매트릭스, 클릭 보호, 출력 테스트, SQLite 저장/열기 | 🐧 오프라인 렌더 테스트, 🪟 8채널 실기 |
| 4 | MIDI 엔진 및 외부 모듈 연결 | 포트 관리, 모듈 프로필, Bank/PC 사전 전송, MIDI Out 스레드, Note 추적 | 🪟 실제 모듈 |
| 5 | VST3 Host | 별도 프로세스 스캔, 블랙리스트, 인스턴스 풀, 상태 저장, GUI, Live Certified 검사 | 🪟 VSTi 3개 이상 |
| 5b (조건부) | 플러그인 프로세스 격리 | b4nd-pluginhost | 🪟 |
| 6 | 오디오·MIDI 동기화 | 템포맵 기반 스케줄링, 지연 측정·보정, MIDI Clock, PDC, Locate chase | 🐧 타이밍 시뮬레이션, 🪟 루프백 측정 |
| 7 | 믹서 및 클릭 트랙 | 채널 스트립, 미터, 리미터, 클릭 생성기(카운트인, 악센트, 음색) | 🐧 오프라인 렌더, 🪟 |
| 8 | AI BPM/Key/코드/구조 분석 | Python 분석기 CLI, 모델 관리, 결과 편집 UI | 🐧 테스트 곡 세트 |
| 9 | AI Stem Separation | Demucs 6s, 스템 트랙 자동 생성, 품질 경고 | 🐧(CPU) |
| 10 | Audio to MIDI | 악기별 변환, 신뢰도 | 🐧 |
| 11 | MIDI Editor | 피아노롤, 파형 배경 | 🐧 UI 테스트 |
| 12 | Live Performance Mode | 전체 화면, 큰 버튼, 풋스위치 매핑, 큐 점프 | 🪟 풋스위치 |
| 13 | Setlist Manager | 세트리스트, 다음 곡 사전 로딩, 총 시간 | 🐧 + 🪟 |
| 14 | 안정성 및 실제 공연 테스트 | 2시간+ 연속 재생, 장애 주입, 리허설 실전 | 🪟 필수 |

## 의존 관계

```
1 → 2 → 3 → 4 → 6 → 7 → 12 → 13 → 14
          └→ 5 ─┘
1 → 8 → 9 → 10 → 11   (AI 트랙은 재생 엔진과 병렬 진행 가능, 결과물은 3의 프로젝트 포맷에 기록)
```

## Phase 1 세부 범위 (이번 구현)

1. CMake 프로젝트: JUCE 8 FetchContent, MSVC/Ninja 프리셋, `B4ND_ENABLE_ASIO` + `B4ND_ASIO_SDK_DIR` 옵션 (Phase 2 대비)
2. `b4nd_core` 정적 라이브러리: `TempoMap`, `OutputRouting`(클릭 보호 규칙), `TimeFormat`
3. 앱: 다크 `LookAndFeel`, 메인 창(최소 1280×720), 레이아웃
   - 상단 TransportBar: 프로젝트명, BPM, Key, 박자, 위치, PLAY/STOP/PAUSE/RESTART/EMERGENCY
   - 좌측 SidePanel: 프로젝트(곡 목록), AI 분석 메뉴(Phase 8 전까지 비활성 표시)
   - 중앙 TimelineView: 눈금자(마디), 트랙 헤더, 샘플 트랙 레인, 재생 헤드
   - 하단 MixerPanel: 채널 스트립 모형, 8개 출력 미터, StatusBar(장치/MIDI/VSTi/CPU/언더런)
   - LiveModeView: 전체 화면 전환(F11), 현재/다음 곡, 큰 버튼
4. 단축키: Space(재생/정지), Esc(비상 정지), Home(처음으로), F11(공연 모드)
5. 로그: `%APPDATA%/B4ND LIVE MTR/logs/` 파일 로그
6. Phase 1의 재생은 **가상 트랜스포트**(UI 타이머 기반 위치 진행)로, 오디오는 출력하지 않음. 실제 오디오는 Phase 2부터.
7. CI: GitHub Actions에서 Windows(MSVC) + Linux 빌드 및 core 테스트

## 일정 감각 (참고)

Phase 1~3이 "오디오가 나오는 MTR"의 최소 단위이며, 이 시점에서 연습실 실기 테스트를 한 번 해보는 것을 권장한다. MIDI/VST(4~7) 이후 Live Mode(12)를 앞당겨 리허설에서 사용하며 안정성을 축적하는 것이 공연 안정성 측면에서 유리하다.

## Phase 2 구현 결과 (2026-11-16)

- `b4nd_engine`(JUCE 비의존): 오디오 콜백 처리, SPSC 명령/이벤트 큐, 샘플 클럭 기반 트랜스포트, 5ms 페이드 정지·비상 정지, 출력 테스트 신호(Tone, Identify, Click/Guide Check, Walk), 출력별 피크, 언더런/과부하 감지
- `AudioDeviceController`: ASIO → WASAPI(Exclusive) → WASAPI(Shared) → DirectSound 순서, 저장 장치 복원, 저장 장치가 없으면 다른 장치로 대체하지 않음, 장치 분리 감지 후 같은 장치만 재연결(Live Mode에서는 수동)
- 오디오 설정 창(상태바 클릭 또는 Ctrl+,): 드라이버/장치/샘플레이트/버퍼(사양 값 ∩ 장치 지원 값), 지연, 드롭아웃, ASIO 드라이버 패널, 출력 테스트
- 검증: 엔진 단위 테스트(콜백 내 힙 할당 0 포함), Linux JACK 가상 8채널 장치로 열기/분리/재연결 자동 테스트(CI), Windows CI에서 ASIO SDK를 받아 ASIO 포함 빌드
- 남은 실기 검증(🪟): UR22C ASIO 실제 재생, USB 분리 시 동작, 드라이버 패널 버퍼 변경
- 제약: UR22C는 출력 2채널이라 8채널 출력 테스트는 장비 교체 후. 2채널용 분리 프리셋(OUT1 = MR 모노 PA, OUT2 = 클릭+가이드)은 Phase 3에서 추가
