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
