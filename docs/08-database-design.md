# 08. 데이터베이스 설계

## 1. 원칙

- 프로젝트 패키지마다 `project.db` (SQLite 3) 하나. 전역 설정(장치, 단축키, 플러그인 캐시)은 DB가 아닌 `%APPDATA%`의 XML.
- `PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;`
- **DB 접근은 IO Worker 스레드에서만.** 오디오 스레드는 DB를 절대 만지지 않는다. 재생은 DB에서 만든 메모리 모델(PlaybackPlan)로만 한다.
- 파일 경로는 패키지 루트 기준 상대 경로. 모든 ID는 UUID 텍스트(다른 프로젝트 간 곡 복사 시 충돌 방지).
- `schema_version` 테이블로 마이그레이션 관리.
- 시간 값: 오디오 위치는 `*_samples`(프로젝트 샘플레이트 기준 정수)가 아닌 **초(REAL) + 원본 기준**으로 저장하고, MIDI는 tick(PPQ 960)으로 저장한다. 샘플레이트 변경에 독립적이게 하기 위함.

## 2. ER 개요

```
project 1─* song 1─* track 1─* clip
                 │        └─ midi_clip ─* (SMF 파일 참조)
                 ├─* tempo_point
                 ├─* time_sig_point
                 ├─* cue_point
                 ├─* chord_event
                 ├─ analysis_run *
                 └─ click_settings 1
track *─1 plugin_slot      track *─1 module_profile
bus 1─* output_assignment          setlist 1─* setlist_entry *─1 song
media_file (원본/스템/캐시)        footswitch_mapping
```

## 3. 테이블 정의

```sql
CREATE TABLE schema_version (version INTEGER NOT NULL);

CREATE TABLE project (
  id TEXT PRIMARY KEY, name TEXT NOT NULL,
  sample_rate INTEGER NOT NULL DEFAULT 48000,
  created_at TEXT NOT NULL, modified_at TEXT NOT NULL,
  app_version TEXT NOT NULL
);

CREATE TABLE media_file (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK (kind IN ('original','stem','cache','guide','click_sample')),
  rel_path TEXT NOT NULL, sha256 TEXT, duration_sec REAL,
  sample_rate INTEGER, channels INTEGER,
  source_media_id TEXT REFERENCES media_file(id)   -- 스템/캐시의 원본
);

CREATE TABLE song (
  id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES project(id) ON DELETE CASCADE,
  title TEXT NOT NULL, artist TEXT,
  base_bpm REAL, bpm_confidence REAL,
  song_key TEXT, key_confidence REAL,
  time_sig_num INTEGER DEFAULT 4, time_sig_den INTEGER DEFAULT 4,
  original_media_id TEXT REFERENCES media_file(id),
  length_sec REAL, start_offset_sec REAL DEFAULT 0,
  last_position_sec REAL DEFAULT 0,          -- Playback Position
  notes TEXT
);

CREATE TABLE tempo_point (
  song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  beat REAL NOT NULL, bpm REAL NOT NULL,
  curve TEXT NOT NULL DEFAULT 'step' CHECK (curve IN ('step','linear')),
  PRIMARY KEY (song_id, beat)
);

CREATE TABLE time_sig_point (
  song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  bar INTEGER NOT NULL, numerator INTEGER NOT NULL, denominator INTEGER NOT NULL,
  PRIMARY KEY (song_id, bar)
);

CREATE TABLE cue_point (
  id TEXT PRIMARY KEY, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  section TEXT CHECK (section IN ('intro','verse','pre_chorus','chorus','bridge','solo','outro','custom')),
  position_sec REAL NOT NULL, color TEXT, sort_order INTEGER,
  source TEXT DEFAULT 'user' CHECK (source IN ('user','ai'))
);

CREATE TABLE chord_event (
  song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  start_sec REAL NOT NULL, end_sec REAL NOT NULL, chord TEXT NOT NULL, confidence REAL,
  PRIMARY KEY (song_id, start_sec)
);

CREATE TABLE bus (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK (kind IN ('main','click','guide','extra','monitor','aux')),
  name TEXT NOT NULL, channels INTEGER NOT NULL CHECK (channels IN (1,2)),
  volume_db REAL DEFAULT 0, limiter_on INTEGER DEFAULT 0
);

CREATE TABLE output_assignment (
  physical_output INTEGER PRIMARY KEY,            -- 0-based 물리 채널
  bus_id TEXT REFERENCES bus(id), bus_channel INTEGER,
  is_pa INTEGER NOT NULL DEFAULT 0,              -- 클릭 보호 플래그
  label TEXT
);

CREATE TABLE plugin_slot (
  id TEXT PRIMARY KEY, plugin_uid TEXT NOT NULL, plugin_name TEXT NOT NULL,
  vendor TEXT, format TEXT DEFAULT 'VST3', version TEXT,
  state_rel_path TEXT, state_sha256 TEXT, preset_name TEXT
);

CREATE TABLE module_profile (
  id TEXT PRIMARY KEY, device_name TEXT NOT NULL, midi_port TEXT NOT NULL,
  midi_channel INTEGER NOT NULL CHECK (midi_channel BETWEEN 1 AND 16),
  bank_msb INTEGER CHECK (bank_msb BETWEEN 0 AND 127),
  bank_lsb INTEGER CHECK (bank_lsb BETWEEN 0 AND 127),
  program INTEGER CHECK (program BETWEEN 0 AND 127),
  volume_cc INTEGER DEFAULT 7, volume_value INTEGER DEFAULT 100,
  pan_cc INTEGER DEFAULT 10, pan_value INTEGER DEFAULT 64,
  sustain_enabled INTEGER DEFAULT 1, delay_offset_ms REAL DEFAULT 0,
  send_order TEXT DEFAULT 'msb,lsb,pc', gap_ms INTEGER DEFAULT 20, settle_ms INTEGER DEFAULT 200,
  audio_input_channel INTEGER                     -- LINE IN 채널 (오디오는 별도 관리)
);

CREATE TABLE track (
  id TEXT PRIMARY KEY, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  name TEXT NOT NULL, sort_order INTEGER NOT NULL,
  kind TEXT NOT NULL CHECK (kind IN ('audio','midi_vsti','midi_external','click','guide','line_in')),
  media_id TEXT REFERENCES media_file(id),
  plugin_slot_id TEXT REFERENCES plugin_slot(id),
  module_profile_id TEXT REFERENCES module_profile(id),
  midi_rel_path TEXT, midi_channel INTEGER,
  volume_db REAL DEFAULT 0, pan REAL DEFAULT 0, gain_db REAL DEFAULT 0,
  mute INTEGER DEFAULT 0, solo INTEGER DEFAULT 0, mono INTEGER DEFAULT 0,
  output_bus_id TEXT REFERENCES bus(id),
  delay_offset_ms REAL DEFAULT 0,
  ai_confidence REAL                              -- 스템/MIDI 변환 신뢰도
);

CREATE TABLE insert_effect (
  track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  slot_index INTEGER NOT NULL, plugin_slot_id TEXT NOT NULL REFERENCES plugin_slot(id),
  bypass INTEGER DEFAULT 0, PRIMARY KEY (track_id, slot_index)
);

CREATE TABLE click_settings (
  song_id TEXT PRIMARY KEY REFERENCES song(id) ON DELETE CASCADE,
  enabled INTEGER DEFAULT 1, count_in_bars INTEGER DEFAULT 1 CHECK (count_in_bars IN (0,1,2,4)),
  accent_first INTEGER DEFAULT 1, sound TEXT DEFAULT 'woodblock',
  volume_db REAL DEFAULT -6, subdivision INTEGER DEFAULT 1
);

CREATE TABLE setlist (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, venue TEXT, show_date TEXT
);
CREATE TABLE setlist_entry (
  setlist_id TEXT NOT NULL REFERENCES setlist(id) ON DELETE CASCADE,
  position INTEGER NOT NULL, song_id TEXT NOT NULL REFERENCES song(id),
  auto_advance INTEGER DEFAULT 0, gap_sec REAL DEFAULT 0,
  PRIMARY KEY (setlist_id, position)
);

CREATE TABLE footswitch_mapping (
  id TEXT PRIMARY KEY, midi_port TEXT, message_type TEXT CHECK (message_type IN ('cc','note')),
  channel INTEGER, number INTEGER, action TEXT NOT NULL, double_press INTEGER DEFAULT 0
);

CREATE TABLE analysis_run (
  id TEXT PRIMARY KEY, song_id TEXT NOT NULL REFERENCES song(id) ON DELETE CASCADE,
  started_at TEXT, finished_at TEXT, status TEXT, tasks TEXT,
  result_rel_path TEXT, model_versions TEXT       -- JSON
);
```

## 4. 저장·백업·복구

| 기능 | 방식 |
|------|------|
| 자동 저장 | 변경 발생 후 5초 디바운스, IO Worker에서 트랜잭션. 재생 중에도 가능하지만 RT와 무관 |
| 백업 | 프로젝트 열 때 + 30분마다 `sqlite3_backup` API로 `backups/project-YYYYMMDD-HHMM.db`, 최근 20개 유지 |
| 비정상 종료 복구 | 실행 시 `session.lock` 파일이 남아 있으면 마지막 자동 저장/백업 중 선택 |
| 무결성 | 열 때 `PRAGMA quick_check`, 실패 시 최신 백업 제안 |
| 누락 미디어 | 열 때 모든 `media_file` 존재·크기 확인 (sha256은 백그라운드) |

## 5. 구현 메모 (Phase 3b, 2026-11-16)

- 코드: `src/project` (`ProjectDatabase`, `ProjectPackage`, JUCE 비의존·단위 테스트), 앱 쪽 `src/app/ProjectSession` (프로젝트 IO 스레드).
- SQLite 3.54.1 amalgamation은 빌드 시 sqlite.org에서 받으며, sqlite.org가 공개한 SHA3-256으로 검증한다 (`third_party/sqlite/CMakeLists.txt`). 오프라인 빌드는 `-DFETCHCONTENT_SOURCE_DIR_SQLITE=<sqlite3.c가 있는 폴더>`.
- 스키마 v1 = 위 정의 + 다음 추가 사항
  - `clip` 테이블 (track_id, media_id, start_sec, offset_sec, length_sec): 오디오 트랙의 실제 배치. ER 개요의 track 1─* clip에 해당
  - `track.destination_label`: Phase 4/5 전까지 MIDI 트랙 목적지 표시용 텍스트
  - `output_assignment`에 CHECK 제약: PA 출력에 click/guide/monitor 버스 금지 (앱 검사와 별개로 DB도 거부)
  - bus 6개는 고정 행 (`bus-main`, `bus-click` ...)
- 저장은 트랜잭션 하나로 전체를 교체한다. 중간에 실패하면 롤백되어 직전 저장 상태가 그대로 남는다 (테스트로 확인).
- 패키지 폴더: `<이름>.b4nd/` 안에 `project.db`, `media/`, `backups/`, `session.lock`. 패키지 안의 미디어는 상대 경로, 밖의 미디어는 절대 경로로 저장한다. 미디어를 패키지로 모으는 기능은 이후 단계.
- 백업 파일 이름은 `project-YYYYMMDD-HHMMSS-NNN.db` (같은 초에 여러 개일 때 NNN).
- 아직 저장하지 않는 것: 데모 MIDI 구간(데모 VSTi 트랙의 구간 표시용), 출력 라우팅(장치 기본 프리셋 사용, 라우팅 편집 UI와 함께 저장 예정), 마스터 볼륨.

### 스키마 v2 (Phase 4)

- `module_profile`을 실제로 저장·로드한다. 추가 열 `midi_port_id`는 OS 포트 식별자로, 이름보다 안정적이다
- `track.module_profile_id`, `track.midi_channel`(0 = 모듈 채널), `track.midi_rel_path`(.mid 파일 경로, 미디어와 같은 상대/절대 규칙)를 사용한다
- v1 파일을 열면 트랜잭션 하나로 v2로 올린다 (`ALTER TABLE` + `schema_version` 갱신). 단계별 마이그레이션 표(`ProjectDatabase::migrate`)에 이어 붙이는 방식이다
- v1 파일에서 올린 뒤 데이터가 그대로이고 새 열에 저장할 수 있는지 테스트로 확인했다
- 열 때 없는 .mid 파일은 누락 미디어 경고에 포함된다

### 스키마 v3 (Phase 5)

- `plugin_slot`에 `state_blob BLOB`(플러그인 `getStateInformation` 결과)과 `description_xml TEXT`(다른 PC에서 같은 플러그인을 찾기 위한 전체 설명)을 추가했다. 처음 설계한 `state_rel_path` 파일 방식은 쓰지 않는다: 상태가 DB 안에 있어야 저장 트랜잭션과 백업에 항상 함께 들어간다 (05 문서 §6)
- VSTi 트랙은 `track.plugin_slot_id`로 슬롯 하나를 가리키고, `.mid` 경로와 채널은 v2의 열을 그대로 쓴다
- 플러그인이 없는 PC에서 열고 저장해도 읽어 온 상태를 그대로 다시 저장한다
- v1/v2 파일은 트랜잭션 하나로 v3까지 올린다

### 스키마 v4 (Phase 6)

- `module_profile`에 `clock_out INTEGER DEFAULT 0`(이 포트로 MIDI Clock을 보낼지)과 `clock_offset_ms REAL DEFAULT 0`(클럭만 앞당기거나 늦추는 값, ±50 ms로 제한)을 추가했다
- 지연 측정 결과는 기존 `delay_offset_ms`에 들어간다. 루프백 왕복 지연은 장치·샘플레이트·버퍼 크기에 따라 달라지므로 프로젝트에 저장하지 않는다 (앱을 켤 때마다 다시 잰다)
- v1–v3 파일은 트랜잭션 하나로 v4까지 올린다. 테스트: v4 열 왕복, v1 파일에서 올린 뒤 기본값
