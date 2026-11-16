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
