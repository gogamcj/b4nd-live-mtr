# The plugin scanner runs every plugin file in its own process (docs/05 §2).
juce_add_console_app(b4nd_plugin_scanner PRODUCT_NAME "b4nd_plugin_scanner")

target_sources(b4nd_plugin_scanner PRIVATE
    ScannerMain.cpp
    PluginCatalog.cpp
    PluginInstrument.cpp)

target_include_directories(b4nd_plugin_scanner PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(b4nd_plugin_scanner PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_LV2=0
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(b4nd_plugin_scanner PRIVATE b4nd_engine juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
