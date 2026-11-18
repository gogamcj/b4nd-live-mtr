add_library(b4nd_core STATIC
    TempoMap.cpp
    OutputRouting.cpp
    TimeFormat.cpp
    XrunDetector.cpp
    Midi.cpp
    SampleClock.cpp
    MidiChase.cpp
    LatencyProbe.cpp
    Limiter.cpp
    MidiNoteEdit.cpp
    Footswitch.cpp)

target_include_directories(b4nd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(b4nd_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(b4nd_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(b4nd_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
