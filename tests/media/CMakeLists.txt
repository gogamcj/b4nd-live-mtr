# Needs JUCE (audio formats), so it is only built together with the app.
juce_add_console_app(b4nd_media_tests PRODUCT_NAME "b4nd_media_tests")

target_sources(b4nd_media_tests PRIVATE
    ../core/TestMain.cpp
    MediaLoaderTests.cpp
    ../../src/media/MediaLoader.cpp)

target_include_directories(b4nd_media_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(b4nd_media_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(b4nd_media_tests PRIVATE b4nd_engine juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME b4nd_media_tests COMMAND b4nd_media_tests)
