# SQLite 3.54.1 amalgamation (public domain, https://www.sqlite.org/copyright.html),
# downloaded from sqlite.org and verified with the SHA3-256 published on
# https://www.sqlite.org/download.html. Offline builds: pass
# -DFETCHCONTENT_SOURCE_DIR_SQLITE=<folder with sqlite3.c / sqlite3.h>.
include(FetchContent)
FetchContent_Declare(sqlite
    URL      https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite)   # the zip has no CMakeLists.txt: this only downloads and extracts

add_library(b4nd_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(b4nd_sqlite PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(b4nd_sqlite PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0)
set_target_properties(b4nd_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(b4nd_sqlite PRIVATE /w)
else()
    target_compile_options(b4nd_sqlite PRIVATE -w)
    find_package(Threads REQUIRED)
    target_link_libraries(b4nd_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
