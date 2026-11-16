# Separate executable: it replaces global operator new to prove the audio callback never allocates.
add_executable(b4nd_engine_tests
    ../core/TestMain.cpp
    AllocationGuard.cpp
    SpscQueueTests.cpp
    XrunDetectorTests.cpp
    AudioEngineTests.cpp
    PlaybackTests.cpp
    MidiEngineTests.cpp)

target_include_directories(b4nd_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core)
target_link_libraries(b4nd_engine_tests PRIVATE b4nd_engine)
add_test(NAME b4nd_engine_tests COMMAND b4nd_engine_tests)
