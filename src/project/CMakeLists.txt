# Project storage (SQLite). JUCE-free so it is unit-tested with the core libraries.
add_library(b4nd_project STATIC
    ProjectDatabase.cpp
    ProjectPackage.cpp)

target_include_directories(b4nd_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(b4nd_project PUBLIC b4nd_core PRIVATE b4nd_sqlite)
target_compile_features(b4nd_project PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(b4nd_project PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(b4nd_project PRIVATE -Wall -Wextra -Wpedantic)
endif()
