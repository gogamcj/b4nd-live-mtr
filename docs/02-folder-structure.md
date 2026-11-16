# 02. 전체 폴더 구조

## 1. 저장소 구조

```
b4nd-live-mtr/
├─ CMakeLists.txt              # 최상위 빌드 (JUCE FetchContent, 옵션)
├─ CMakePresets.json           # windows-msvc / linux-ninja 프리셋
├─ README.md
├─ .github/workflows/
│  └─ build.yml                # Windows(MSVC) + Linux CI 빌드·테스트
├─ docs/                       # 설계 문서 01~10
├─ cmake/                      # 보조 CMake 스크립트
├─ src/
│  ├─ core/                    # JUCE GUI에 의존하지 않는 순수 로직 (단위 테스트 대상)
│  │  ├─ TempoMap.h/.cpp       # 템포/박자 ↔ 시간 변환
│  │  ├─ OutputRouting.h/.cpp  # 버스→물리 출력 매핑, 클릭 보호 규칙
│  │  ├─ TimeFormat.h/.cpp     # 시간/마디 표기
│  │  └─ RtQueue.h             # (Phase 2) SPSC lock-free 큐
│  ├─ app/                     # 애플리케이션 진입점, 메인 창, 명령
│  │  ├─ Main.cpp
│  │  ├─ MainWindow.h/.cpp
│  │  ├─ MainComponent.h/.cpp
│  │  ├─ AppState.h/.cpp       # UI가 보는 프로젝트/트랜스포트 상태 (Phase 1 모형)
│  │  └─ CommandIds.h          # 단축키/메뉴 명령 ID
│  ├─ ui/                      # 화면 구성 요소
│  │  ├─ B4ndLookAndFeel.h/.cpp
│  │  ├─ Theme.h               # 색상·폰트 토큰
│  │  ├─ TransportBar.*        # 상단
│  │  ├─ SidePanel.*           # 좌측
│  │  ├─ TimelineView.*        # 중앙
│  │  ├─ MixerPanel.*          # 하단 믹서
│  │  ├─ StatusBar.*           # 하단 장치 상태
│  │  ├─ LevelMeter.*
│  │  └─ LiveModeView.*        # 전체 화면 공연 모드
│  ├─ audio/                   # (Phase 2~3) 장치 관리, 오디오 엔진, 리더
│  ├─ midi/                    # (Phase 4, 6) MIDI 엔진, 장치, 외부 모듈
│  ├─ plugins/                 # (Phase 5) VST3 호스트
│  ├─ mixer/                   # (Phase 7) 채널 스트립, 버스, 클릭
│  ├─ project/                 # (Phase 3+) SQLite, 패키지, 자동 저장
│  ├─ live/                    # (Phase 12) 공연 엔진, 풋스위치 매핑
│  ├─ setlist/                 # (Phase 13)
│  └─ infra/                   # 로깅, 모니터링, 크래시 복구
├─ tests/
│  ├─ core/                    # core 단위 테스트 (CTest)
│  └─ soak/                    # (Phase 14) 장시간 재생 테스트 하니스
├─ analyzer/                   # Python AI 분석기 (Phase 8~10)
│  ├─ pyproject.toml
│  ├─ b4nd_analyzer/
│  │  ├─ cli.py                # job.json 입력 → result.json 출력
│  │  ├─ tempo.py  key.py  chords.py  structure.py
│  │  ├─ stems.py  transcribe.py
│  │  └─ models.py             # 모델 다운로드/검증/버전 관리
│  └─ tests/
├─ resources/                  # 아이콘, 클릭 샘플, 폰트
└─ third_party/                # sqlite 등 (JUCE는 FetchContent로 가져옴)
```

규칙: `core/`는 `juce_core`까지만 의존하거나 의존하지 않는다. UI(`ui/`)는 엔진 내부를 직접 만지지 않고 `AppState`/명령 인터페이스를 통해서만 접근한다.

## 2. 프로젝트 패키지 구조 (사용자 데이터)

```
MySong.b4ndproj/               # 폴더 = 프로젝트 하나 (곡 여러 개 + 세트리스트 포함 가능)
├─ project.db                  # SQLite (08 문서)
├─ media/
│  ├─ original/                # 원곡 또는 사용자 오디오 (원본 보존, 수정 금지)
│  ├─ stems/<song-id>/         # vocals.wav drums.wav bass.wav guitar.wav piano.wav other.wav
│  └─ cache/                   # 재생용 변환본 (WAV, 프로젝트 샘플레이트). 삭제해도 재생성 가능
├─ midi/<song-id>/*.mid
├─ plugins/states/*.vststate   # 플러그인 상태 바이너리 (플러그인 바이너리는 포함하지 않음)
├─ analysis/<song-id>/result.json
├─ backups/                    # 자동 백업 project.db 사본 (시간순 회전)
└─ logs/
```

- 다른 PC로 옮길 때는 폴더 전체를 복사하거나 `.b4ndpkg`(zip)로 내보낸다. 내보내기 시 `media/cache/`는 제외 가능.
- 모든 경로는 패키지 루트 기준 **상대 경로**로 DB에 저장한다.

## 3. 사용자 PC 설치 경로 (Windows)

```
C:\Program Files\B4ND LIVE MTR\           실행 파일
%APPDATA%\B4ND LIVE MTR\settings.xml       장치·단축키·전역 설정
%APPDATA%\B4ND LIVE MTR\plugin-cache.xml   VST3 스캔 결과, 블랙리스트
%APPDATA%\B4ND LIVE MTR\logs\              전역 로그 (일자별 회전)
%LOCALAPPDATA%\B4ND LIVE MTR\analyzer\     Python 환경 및 AI 모델
```
