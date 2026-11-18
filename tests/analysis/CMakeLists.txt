# AnalyzerRunner: the app side of the Python analyzer (docs/07). Needs JUCE (child process,
# WAV writer). Runs the real analyzer when a Python with numpy + scipy is found (CI sets
# B4ND_REQUIRE_ANALYZER=1 so that a missing Python fails instead of skipping).
juce_add_console_app(b4nd_analysis_tests PRODUCT_NAME "b4nd_analysis_tests")

target_sources(b4nd_analysis_tests PRIVATE
    ../core/TestMain.cpp
    AnalyzerRunnerTests.cpp
    ../../src/app/AnalyzerRunner.cpp)

target_include_directories(b4nd_analysis_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(b4nd_analysis_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1
    B4ND_SOURCE_ANALYZER_DIR="${PROJECT_SOURCE_DIR}/analyzer")
target_link_libraries(b4nd_analysis_tests PRIVATE b4nd_engine b4nd_project juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME b4nd_analysis_tests COMMAND b4nd_analysis_tests)
