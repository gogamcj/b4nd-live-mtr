# 07. AI 분석 모델 선정

모든 AI 처리는 **로컬 실행**, **오프라인(공연 전) 전용**, **별도 Python 프로세스**로 한다.
결과는 항상 "초안"이며 사용자가 수정할 수 있다. 각 결과에는 신뢰도를 함께 저장한다.

> 아래 모델의 정확도·Windows 설치 가능성은 Phase 8~10 시작 시 실제 곡 샘플(B4ND 카피곡 10곡)로 재평가한다. 현재 표는 공개 자료 기준의 1차 선정이다.

## 1. 선정표

| 기능 | 1순위 | 대안 | 라이선스 메모 |
|------|-------|------|---------------|
| BPM / 비트 / 다운비트 | **Beat This!** (CPJKU, 2024) | madmom DBN, librosa beat_track | Beat This! MIT · madmom 코드 BSD, **모델은 CC BY-NC-SA(비상업)** |
| 박자(Time Signature) | 다운비트 간 비트 수 통계 (Beat This! 출력 기반) | madmom DBNDownBeatTracker | — |
| 템포 변화 / Tempo Map | 비트 간격 → 이동 중앙값 → 구간화 | — | 자체 구현 |
| Key | 크로마 + Krumhansl/Temperley 템플릿 (자체, librosa) | madmom CNN Key | librosa ISC. Essentia는 AGPL이므로 제외 |
| Chord | 크로마 기반 템플릿 + HMM 스무딩 (자체) → 이후 딥러닝 모델 비교 | madmom CNN/CRF Chord, Chordino(Vamp) | Chordino GPL (외부 프로세스 실행 시 고려) |
| 곡 구조 (Intro/Verse/Chorus…) | **All-In-One (allin1)** | 자기유사행렬 기반 분할(자체) | allin1 MIT. 의존성(NATTEN, madmom)의 Windows 설치 검증 필요 |
| Stem 분리 | **Demucs `htdemucs_6s`** | `htdemucs_ft`(4 stem, 품질↑) | MIT. 6s 모델이 요구사항의 Vocal/Drums/Bass/Guitar/Piano/Other와 정확히 일치 |
| Audio→MIDI (피아노/기타 다성) | **Basic Pitch** (Spotify) | 피아노 전용: piano_transcription_inference(ByteDance) | Basic Pitch Apache-2.0 |
| Audio→MIDI (베이스 단음) | **torchcrepe / pYIN F0 → 노트 분할** | Basic Pitch + 단음 후처리 | torchcrepe MIT |
| Audio→MIDI (드럼) | **ADTOF** 계열 모델 | 온셋 검출 + 대역 분류(킥/스네어/하이햇/심벌) | 라이선스 확인 필요 |

## 2. 처리 파이프라인

```
원곡.mp3
  ├─ 1) 디코딩 → 44.1k WAV (원본 보존)
  ├─ 2) Beat This! → beats, downbeats  ──▶ BPM(+신뢰도), 박자, Tempo Map
  ├─ 3) Key / Chord (크로마)           ──▶ key(+신뢰도), chord 타임라인
  ├─ 4) allin1 → 구조 구간 라벨         ──▶ Intro/Verse/Chorus/Bridge/Solo/Outro → Cue Point 초안
  ├─ 5) Demucs htdemucs_6s              ──▶ stems/*.wav
  │       └ 스템별 품질 지표 (에너지 비율, 잔차) → 낮으면 경고
  └─ 6) 스템별 Audio→MIDI
          bass  → crepe 단음
          piano → Basic Pitch
          guitar→ Basic Pitch (코드/아르페지오)
          drums → ADTOF → GM 드럼맵 (36 킥, 38 스네어, 42/46 하이햇, 49/57 크래시, 51 라이드)
```

## 3. 신뢰도 표시 정의

| 항목 | 산출 방법 | UI 표시 |
|------|-----------|---------|
| BPM | 비트 간격 분산 + 모델 활성화 피크 강도 | 0–100%, 70% 미만 노란색 |
| 템포 변화 | 구간별 BPM 차이 > 2% 지속 시 "가변 템포" 표기 | Tempo Map 그래프 |
| Key | 1위/2위 템플릿 상관 차이 | 상위 3개 후보 제시 |
| Chord | 프레임 사후확률 평균 | 구간별 색상 |
| Stem | 분리 후 재합성 오차, 스템 에너지 | 경고 아이콘 |
| MIDI 노트 | 모델 노트 확률 | 피아노롤 벨로시티 색상과 별도로 신뢰도 투명도 |

## 4. 분석기 인터페이스 (MTR ↔ Python)

```
b4nd-analyzer run --job job.json
  job.json   : { "input": "media/original/x.mp3", "tasks": ["tempo","key","chords","structure","stems","midi"],
                 "device": "cuda|cpu", "out": "analysis/<song-id>/" }
  stdout     : 줄 단위 JSON 진행률 {"task":"stems","progress":0.42}
  result.json: { "bpm":{"value":128.0,"confidence":0.93}, "tempoMap":[...], "timeSig":"4/4",
                 "key":{"value":"E minor","candidates":[...]}, "chords":[...], "sections":[...],
                 "stems":{...}, "midi":{...}, "versions":{"demucs":"4.0.1", ...} }
```
- 종료 코드로 성공/실패 구분, 실패해도 부분 결과 보존.
- 모델 버전과 파라미터를 결과에 기록해 재현 가능하게 한다.

## 5. 설치·모델 관리

- `%LOCALAPPDATA%\B4ND LIVE MTR\analyzer\` 아래에 임베디드 Python(또는 venv)과 모델 가중치를 둔다. 설치는 MTR의 "AI 모델 관리" 화면에서 수행 (최초 1회 인터넷 필요, 이후 오프라인).
- GPU(NVIDIA CUDA)가 있으면 사용, 없으면 CPU. CPU에서 Demucs 6s는 곡당 수 분 소요 예상 → 진행률 표시 및 백그라운드 큐.
- 비상업 라이선스 모델(madmom 모델 등)은 기본 비활성, 사용자가 동의 후 활성화.

## 6. Phase 8 구현 (2026-11-18)

Phase 8에서는 위 선정표의 1순위 모델(Beat This!, allin1)을 아직 넣지 않았다. 둘 다 PyTorch가 필요하고 Windows 설치 검증이 안 되어 있어, 먼저 **numpy·scipy만 쓰는 자체 구현**으로 전체 흐름(분석 → 검토 → 적용 → 저장)을 완성했다. 같은 job/result 형식으로 나중에 모델을 바꿔 끼울 수 있다.

| 항목 | 방법 (analyzer/b4nd_analyzer) |
|------|------|
| 온셋 | STFT(1024/256, 22.05 kHz) 로그 스펙트럼 플럭스. 윈도 때문에 12 ms 일찍 잡히는 것을 보정 (클릭으로 측정) |
| BPM | 온셋 자기상관 + 120 BPM 중심 로그 정규 사전분포, 2배 지연 보강 |
| 비트 | 동적 계획법 비트 추적(Ellis 2007), 피크 보간 |
| Tempo Map | 9비트 이동 중앙값이 2% 넘게 8비트 이상 벗어나면 새 구간. 구간마다 직선 맞춤, 다음 구간 시작에 정확히 닿도록 BPM 보정 (MTR이 BPM과 박만으로 시간을 다시 계산해도 어긋나지 않게) |
| 박자·첫 다운비트 | 3/4와 4/4의 모든 위상에서 킥 강세 + 화성 변화가 마디 첫 박에 모이는 정도. 4/4에 가중치 |
| 마디 1 위치 | 첫 구간 직선에서 음악이 시작되는 곳의 다운비트. 그 앞(리드인)은 음수 박 |
| Key | 스펙트럼 피크 기반 크로마 + Krumhansl-Kessler. 나란한조(장조/단조)는 으뜸화음이 얼마나 오래 울리는지, 처음과 끝 코드로 판정 |
| 코드 | 박 단위 크로마, 장·단3화음 24개 + N, Viterbi 스무딩. 조에 맞춰 ♯/♭ 표기 |
| 구조 | 마디 단위 크로마·음색·음량 특징, 앞뒤 4마디 비교 변화량 + 음량 단차로 경계, 반복 구간 묶기, 반복·음량·위치로 Intro/Verse/Chorus/Bridge/Outro 이름 |

- 4분 곡 분석 약 1.5초 (CPU 하나).
- 결과에 NaN/Inf가 생기면 null로 바꿔 저장한다. MTR이 나머지를 읽을 수 있게 하기 위해서다.
- 종료 코드: 0 완료, 3 일부 실패(나머지 결과는 저장), 2 작업 파일 오류나 읽을 수 없는 오디오.
- 실행 방법은 두 가지다.
  - 단독 실행 분석기 `b4nd-analyzer\b4nd-analyzer.exe`: CI가 PyInstaller로 만들어 앱 옆에 둔다. 밴드 PC에 Python이 필요 없다.
  - Python 3 + numpy + scipy: 앱 옆의 `analyzer\b4nd_analyzer` 패키지를 쓴다.
  - 분석 창의 ANALYZER... 버튼으로 둘 중 하나를 직접 고를 수도 있다.
- 앱은 분석할 트랙을 곡 시간 기준 모노 WAV로 써서 넘긴다(트랙 시작 전은 무음). 그래서 결과 시간이 그대로 곡 시간이다. WAV는 분석이 끝나면 지운다.
- 알려진 한계
  - 템포 2배/절반 혼동: 사전분포가 120 근처를 고른다(예: 174 BPM 드럼앤베이스가 87로 나옴). 결과에 2배/절반 후보를 함께 주고, 분석 창의 x2 / /2 버튼으로 고친다.
  - 화음은 장·단3화음만 판정한다(7th, sus 등은 가장 가까운 3화음으로 표시).
  - 구조 이름은 규칙 기반 초안이다. 신뢰도가 낮게 표시되며, 이름은 분석 창에서 고친다.
  - 실제 곡 10곡으로 정확도를 재는 작업은 아직 하지 않았다. 테스트는 정답을 아는 합성 곡으로 한다.
- 모델 관리 화면, Beat This!/allin1 연결은 실제 곡 평가 뒤에 한다(Phase 8b).

## 7. Phase 9 구현: 스템 분리 (2026-11-18)

- 모델: Demucs 4.0.1 `htdemucs_6s` (drums, bass, other, vocals, guitar, piano). PyTorch CPU로 동작하고, GPU가 있으면 더 빠르다.
- 분석기 `stems.py`
  - 입력: 앱이 트랙 오디오를 스테레오 24비트 WAV(`stems_input.wav`, 미디어 시간)로 써서 넘긴다. 분석용 모노 WAV와 따로 쓴다. 둘 다 끝나면 지운다.
  - 44.1 kHz로 바꾼 뒤 40초 단위로 나눠 분리하고, 2초 겹침을 크로스페이드로 잇는다. 긴 곡도 메모리가 일정하고 진행률이 나온다.
  - 결과 파일: `analysis/<song-id>/stems/<name>.wav` (24비트 스테레오, 원본과 같은 길이).
  - 품질 지표(§3 표)
    - 스템별 에너지: 스템 RMS / 믹스 RMS (dB). -45 dB 미만은 "empty"(그 악기가 곡에 없음)로 표시하고 트랙을 만들지 않는다.
    - 재합성 잔차: (믹스 − 스템 합) / 믹스 (dB).
    - 앱의 신뢰도: 잔차 -40 dB 이하 90%, -10 dB 이상 30%, 그 사이는 직선. 50% 미만이면 경고.
    - 0 dBFS를 넘어 잘린 스템은 경고.
  - 오프라인 동작: 모델 파일을 `models/` 폴더(`htdemucs_6s.yaml` + `.th`)로 같이 배포한다. 단독 실행 분석기 옆의 `models/`, 패키지 옆의 `models/`, 환경 변수 `B4ND_DEMUCS_REPO` 순서로 찾는다. 없으면 처음 실행할 때 인터넷에서 받는다. `analyzer/tools/fetch_demucs_model.py`가 이 폴더를 만든다.
  - `info`는 torch를 불러오지 않고 Demucs 설치 여부만 본다. 앱이 분석기를 확인할 때 오래 기다리지 않게 하기 위해서다.
  - Demucs가 없으면 스템 작업만 "Demucs is not installed" 오류로 끝나고, 같이 요청한 다른 분석 결과는 저장된다.
- 앱
  - 분석 창에 Stems 작업(기본 꺼짐, 오래 걸리므로)과 결과 줄(스템 목록, 잔차, 신뢰도, 경고), 적용 항목 STEMS를 추가했다. 좌측 Stem Separation 버튼은 Stems를 켠 채로 분석 창을 연다.
  - 스템 작업이 있으면 시간 초과를 2시간으로 늘린다 (CPU가 느린 PC).
  - APPLY 하면
    - 스템 폴더를 `stems/<song-id>/<시각>/`으로 옮긴다. 실행마다 새 폴더라서 이미 열려 있는 파일을 덮어쓰지 않는다.
    - 비어 있지 않은 스템마다 음소거된 오디오 트랙을 맨 뒤에 추가한다. 시작 위치는 원본 트랙과 같다. 원본 트랙은 건드리지 않는다.
    - 같은 원본을 다시 분리하면 기존 스템 트랙의 파일만 새 것으로 바꾼다. 이름, 페이더, 음소거는 그대로 둔다.
    - 파일 이동이 실패하면 같이 적용하던 템포·키·큐·코드도 되돌린다.
  - UNDO APPLY: 트랙 목록을 적용 전으로 돌리고, 스템 폴더를 분석 폴더로 다시 옮긴다(다시 적용할 수 있다).
  - DB: `media_file.kind = 'stem'`, `source_media_id`(원본), `track.ai_confidence`. 스키마 변경 없음.
  - 타임라인 트랙 머리에 보라색 "AI xx%" 배지, 경고가 있으면 노란 "!". 믹서 스트립에 "AI STEM" 표시.
- 알려진 한계
  - 모델 README대로 piano 스템 품질이 낮고 guitar도 섞임이 있다. 두 스템에는 항상 "귀로 확인" 경고를 붙인다.
  - 잔차는 "스템을 합치면 원래 소리가 되는가"만 본다. 악기가 다른 스템에 섞여 들어간 정도(분리 품질 자체)는 정답 스템이 없으면 잴 수 없다. 실제 곡을 귀로 확인해야 한다.
  - 다시 분리하면 이전 실행의 스템 폴더는 디스크에 남는다(약 곡당 200 MB). 지금은 손으로 지운다.
  - Windows 단독 실행 분석기는 PyTorch가 들어가서 크다(수백 MB).

## 8. Phase 10 구현: Audio to MIDI (2026-11-18)

- 스템별 변환 (`analyzer/b4nd_analyzer/transcribe.py`)
  | 스템 | 방법 | MIDI |
  |------|------|------|
  | piano, guitar, other | Basic Pitch (다성) | 채널 1 |
  | bass (28–420 Hz), vocals (75–1100 Hz) | Basic Pitch를 음역으로 제한한 뒤 한 번에 한 음으로 정리 | 채널 1 |
  | drums | 자체: 3개 대역(저역/중역+노이즈/고역) 온셋 → 킥 36, 스네어 38, 닫힌 하이햇 42 | 채널 10 |
- Basic Pitch
  - 모델 `models/basic_pitch/nmp.onnx`(230 KB, Apache-2.0, basic-pitch 0.4.0에서 그대로 복사)를 onnxruntime으로 실행한다. 저장소와 배포본에 들어 있어 다운로드가 필요 없다.
  - TensorFlow, librosa 없이 창 나누기와 노트 해석(onset/frame 임계값, melodia)을 numpy로 다시 썼다. 창 사이 시간은 정확히 계산한다(원본은 근사).
  - 해석된 노트 시작이 11 ms 빠르게 나와 보정했다(합성 음으로 측정).
  - 다른 음의 어택 때문에 같은 음이 다시 잡히는 경우를 합친다(이어지는 같은 음이고 모델 onset이 0.7 미만일 때).
  - 신뢰도 0.35 미만의 다성 노트는 대부분 배음이라 버린다.
- 베이스·보컬: docs/07 선정표는 torchcrepe였지만, 모델 하나로 배포와 검증을 단순하게 하려고 Basic Pitch + 단음 정리로 바꿨다. 실제 곡 평가에서 부족하면 다시 검토한다.
- 신뢰도: 노트마다 모델 확률(0–1). 벨로시티는 확률 × 127 (최소 20). 트랙 신뢰도는 평균이며, 50% 미만이면 경고.
- 앱
  - 분석 창의 "Audio to MIDI (from stems)" 작업, 좌측 Audio to MIDI 버튼. 곡의 스템 트랙을 변환한다. Stems와 같이 켜면 방금 분리한 스템을 바로 변환한다.
  - 트랙 오디오가 필요 없는 작업이라 입력 WAV를 쓰지 않는다.
  - APPLY 하면
    - 노트를 곡의 템포맵으로 박에 맞춰 `midi/<song-id>/<시각>/<part>.mid`(960 PPQ)에 쓴다. 스템 트랙의 시작 위치를 반영한다.
    - 파트마다 음소거된 VSTi 트랙(악기 없음)을 추가한다. 악기를 고르면 바로 연주한다. 외부 모듈로 보내려면 MIDI 파일을 외부 MIDI 트랙에 끌어다 놓는다.
    - 다시 변환하면 같은 파트의 트랙은 파일만 바꾸고 악기·페이더·음소거는 그대로 둔다. 노트가 없는 파트는 트랙을 만들지 않는다.
    - UNDO APPLY로 트랙을 되돌린다(파일은 남는다).
  - DB: 트랙의 `ai_confidence`로 AI MIDI 트랙을 표시한다. 스키마 변경 없음.
- 알려진 한계
  - 마디 1보다 앞의 노트(픽업)는 MIDI 파일에 음수 시간이 없어서 빠진다. 경고에 개수를 적는다.
  - 프로그램 체인지는 쓰지 않는다. 외부 모듈의 음색을 바꾸지 않게 하기 위해서다(GM 추천 번호는 결과에만 있다).
  - 드럼은 킥/스네어/하이햇 세 가지만. 탐/심벌/오픈 하이햇은 없다. 킥과 스네어가 동시에 치면 킥을 놓칠 수 있다.
  - 피치 벤드 없음. 합성 음 기준 다성 정확도는 재현율 100%, 정밀도 약 66%(배음이 추가 노트로 나옴). 실제 악기는 따로 평가해야 한다.
