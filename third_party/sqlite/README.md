# SQLite amalgamation

- Version: 3.54.1 (`SQLITE_SOURCE_ID` 2026-10-16 14:45:20 555e07ef33f6...)
- Files: `sqlite3.c`, `sqlite3.h`, unmodified
- License: public domain (https://www.sqlite.org/copyright.html)
- sha256: sqlite3.c `75a4760a7b9727a4c4c877aec92eacf39e52a8f2921ce00bd33be6d3a8e8e254`, sqlite3.h `d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c`

The CI job `Linux` downloads `https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip`
and fails if these files differ from the official release.
