# SQLite 3.54.1 amalgamation (public domain, https://www.sqlite.org/copyright.html).
# Vendored so builds never depend on the network. CI compares this file with the
# official amalgamation zip from sqlite.org (see .github/workflows/build.yml).
add_library(b4nd_sqlite STATIC sqlite3.c)
target_include_directories(b4nd_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
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
