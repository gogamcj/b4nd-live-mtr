# Needs JUCE (MIDI devices / files), so it is only built together with the app.
juce_add_console_app(b4nd_midi_tests PRODUCT_NAME "b4nd_midi_tests")

target_sources(b4nd_midi_tests PRIVATE
    ../core/TestMain.cpp
    MidiOutputTests.cpp
    ../../src/midi/MidiOutputService.cpp
    ../../src/midi/MidiFileImport.cpp)

target_include_directories(b4nd_midi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(b4nd_midi_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
if(UNIX AND NOT APPLE)
    target_compile_definitions(b4nd_midi_tests PRIVATE JUCE_ALSA=1 JUCE_JACK=0)
endif()
target_link_libraries(b4nd_midi_tests PRIVATE b4nd_engine juce::juce_audio_devices
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME b4nd_midi_tests COMMAND b4nd_midi_tests)
