# B4ND LIVE MTR

AI Song Analyzer + Hybrid MIDI/VSTi + Professional Live MTR.
B4ND(기타·베이스·드럼·보컬 + 곡에 따라 키보드) 카피 공연을 위한 Windows 전용 멀티트랙 재생 프로그램입니다.
**기능 개수보다 실제 공연 중 끊김 없는 재생을 최우선**으로 합니다.

## 설계 문서

| # | 문서 |
|---|------|
| 01 | [시스템 아키텍처](docs/01-system-architecture.md) |
| 02 | [전체 폴더 구조](docs/02-folder-structure.md) |
| 03 | [오디오 엔진 구조](docs/03-audio-engine.md) |
| 04 | [MIDI 라우팅 구조](docs/04-midi-routing.md) |
| 05 | [VST3 Host 구조](docs/05-vst3-host.md) |
| 06 | [Audio Routing 설계도](docs/06-audio-routing.md) |
| 07 | [AI 분석 모델 선정](docs/07-ai-model-selection.md) |
| 08 | [데이터베이스 설계](docs/08-database-design.md) |
| 09 | [단계별 개발 계획](docs/09-development-plan.md) |
| 10 | [테스트 계획](docs/10-test-plan.md) |

## 현재 상태: Phase 3b (프로젝트 저장/열기)

- 다크 테마 메인 화면: 상단 트랜스포트/BPM/Key/박자, 좌측 프로젝트·AI 메뉴, 중앙 멀티트랙 타임라인, 하단 믹서·8채널 출력 미터·장치 상태
- 전체 화면 공연 모드 (F11)
- `b4nd_core`: 템포맵(템포·박자 변경), 클릭 보호 출력 라우팅 (+ 단위 테스트)
- 오디오 장치(ASIO 우선) 선택·설정, 장치 분리 감지와 같은 장치 재연결, 공연 전 출력 테스트(상태바 클릭 또는 Ctrl+,)
- 오디오 파일(WAV/AIFF/FLAC/MP3)을 타임라인에 끌어다 놓으면 트랙이 되고, 재생 전에 RAM으로 미리 불러옵니다
- 클릭은 템포맵을 따라 생성되며 클릭/가이드는 PA 출력에 절대 나가지 않습니다
- 출력 2개 장치(UR22C 등)는 자동으로 OUT1 = MR 모노(PA), OUT2 = 클릭+가이드(인이어)로 나뉩니다
- 트랜스포트는 오디오 샘플 클럭으로 동작합니다. 장치가 없으면 화면 확인용 가상 트랜스포트로 동작합니다.
- 프로젝트는 `<이름>.b4nd` 폴더(SQLite `project.db`)로 저장되며, 변경 5초 후 자동 저장, 30분마다 백업, 비정상 종료 감지, 시작 시 마지막 프로젝트 자동 열기를 지원합니다
- 프로젝트가 없으면 데모 곡 목록이 표시됩니다

![재생 화면 (8출력)](docs/images/phase3-playback-8out.png)

![재생 화면 (UR22C 같은 2출력 장치)](docs/images/phase3-playback-2out.png)

![메인 화면](docs/images/phase1-edit.png)

![공연 모드](docs/images/phase1-live.png)

### 단축키

| 키 | 동작 |
|----|------|
| Space | 재생 / 일시정지 |
| Esc | 비상 정지 |
| Home | 처음으로 |
| Enter | 정지 |
| PageDown / PageUp | 다음 곡 / 이전 곡 |
| → | 다음 큐 포인트 |
| L | 루프 |
| F11 | 공연 모드 전환 |
| Ctrl+, | 오디오 설정 |
| Ctrl+S / Ctrl+Shift+S | 저장 / 다른 이름으로 저장 |
| Ctrl+O / Ctrl+N | 프로젝트 열기 / 새 프로젝트 |

## 빌드 (Windows 10/11 x64)

필요: Visual Studio 2022 (C++ 데스크톱 개발), CMake 3.22+, Git. JUCE 8.0.9는 CMake가 자동으로 받습니다.

```bat
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --test-dir build/windows -C Release
"build\windows\src\app\B4ND_LIVE_MTR_artefacts\Release\B4ND LIVE MTR.exe"
```

ASIO 사용 (Phase 2부터 필요): Steinberg 사이트에서 ASIO SDK를 받아 압축을 풀고,

```bat
set B4ND_ASIO_SDK_DIR=C:\SDK\asiosdk
cmake --preset windows-msvc-asio
cmake --build --preset windows-msvc-asio
```

GitHub Actions가 매 push/PR마다 Windows(MSVC)와 Linux에서 빌드·테스트하고, Windows 실행 파일과 UI 스크린샷을 Artifact로 올립니다.

## 라이선스 메모

JUCE는 AGPLv3/상용 이중 라이선스입니다. 밴드 내부 사용은 문제없지만 외부 배포 시 검토가 필요합니다. ASIO SDK는 JUCE에 포함되지 않으며 Steinberg 조건에 따라 별도로 받아야 합니다. AI 모델별 라이선스는 [07 문서](docs/07-ai-model-selection.md)를 참고하세요.
