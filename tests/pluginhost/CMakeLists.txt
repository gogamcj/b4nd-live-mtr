# Plugin host tests: real VST3 test plugins (tests/plugins), the scanner process, the catalog
# and VST3 instruments playing through the engine.
juce_add_console_app(b4nd_pluginhost_tests PRODUCT_NAME "b4nd_pluginhost_tests")

target_sources(b4nd_pluginhost_tests PRIVATE
    ../core/TestMain.cpp
    ../engine/AllocationGuard.cpp
    PluginHostTests.cpp
    ../../src/plugins/PluginCatalog.cpp
    ../../src/plugins/PluginInstrument.cpp)

target_include_directories(b4nd_pluginhost_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(b4nd_pluginhost_tests PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_LV2=0
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1
    B4ND_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/tests/plugins/vst3"
    B4ND_SCANNER_EXE="$<TARGET_FILE:b4nd_plugin_scanner>")
target_link_libraries(b4nd_pluginhost_tests PRIVATE b4nd_engine juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(b4nd_pluginhost_tests b4nd_plugin_scanner
    b4nd_test_synth_VST3 b4nd_test_crash_VST3 b4nd_test_latency_VST3 b4nd_test_slow_VST3 b4nd_test_badstate_VST3)
add_test(NAME b4nd_pluginhost_tests COMMAND b4nd_pluginhost_tests)
