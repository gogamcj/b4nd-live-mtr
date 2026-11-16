# Small VST3 plugins used to test the plugin host (scan, blacklist, certification, playback).
# They are test fixtures, not products: built only with the app and tests.

function(b4nd_test_plugin target code name variant is_synth)
    juce_add_plugin(${target}
        COMPANY_NAME "B4ND Test"
        PLUGIN_MANUFACTURER_CODE B4tt
        PLUGIN_CODE ${code}
        FORMATS VST3
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT ${is_synth}
        VST3_CATEGORIES ${ARGN}
        COPY_PLUGIN_AFTER_BUILD FALSE
        PRODUCT_NAME "${name}")
    target_sources(${target} PRIVATE TestPlugin.cpp)
    target_compile_definitions(${target} PUBLIC
        B4ND_TEST_VARIANT=${variant}
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(${target} PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    # Collect the bundles in one folder: <build>/tests/plugins/vst3
    add_custom_command(TARGET ${target}_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vst3"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:${target}_VST3>/../.."
                "${CMAKE_CURRENT_BINARY_DIR}/vst3/${name}.vst3")
endfunction()

b4nd_test_plugin(b4nd_test_synth   Bsyn "B4ND Test Synth"   0 TRUE  Instrument Synth)
b4nd_test_plugin(b4nd_test_crash   Bcrs "B4ND Test Crash"   1 TRUE  Instrument Synth)
b4nd_test_plugin(b4nd_test_slow    Bslo "B4ND Test Slow"    2 TRUE  Instrument Synth)
b4nd_test_plugin(b4nd_test_badstate Bbst "B4ND Test BadState" 3 TRUE Instrument Synth)

set(B4ND_TEST_PLUGIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/vst3" CACHE INTERNAL "")
