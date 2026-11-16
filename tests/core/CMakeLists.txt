add_executable(b4nd_core_tests
    TestMain.cpp
    TempoMapTests.cpp
    OutputRoutingTests.cpp
    TimeFormatTests.cpp
    MidiTests.cpp)

target_link_libraries(b4nd_core_tests PRIVATE b4nd_core)
add_test(NAME b4nd_core_tests COMMAND b4nd_core_tests)
