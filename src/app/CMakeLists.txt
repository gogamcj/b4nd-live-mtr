juce_add_gui_app(B4ND_LIVE_MTR
    PRODUCT_NAME "B4ND LIVE MTR"
    COMPANY_NAME "B4ND"
    VERSION ${PROJECT_VERSION})

target_sources(B4ND_LIVE_MTR PRIVATE
    Main.cpp
    MainWindow.cpp
    MainComponent.cpp
    AppState.cpp
    ProjectSession.cpp
    ../ui/B4ndLookAndFeel.cpp
    ../ui/TransportBar.cpp
    ../ui/SidePanel.cpp
    ../ui/TimelineView.cpp
    ../ui/MixerPanel.cpp
    ../ui/StatusBar.cpp
    ../ui/LevelMeter.cpp
    ../ui/LiveModeView.cpp
    ../ui/AudioSettingsPanel.cpp
    ../ui/ProjectCommands.cpp
    ../audio/AudioDeviceController.cpp
    ../media/MediaLoader.cpp)

target_include_directories(B4ND_LIVE_MTR PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(B4ND_LIVE_MTR PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:B4ND_LIVE_MTR,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:B4ND_LIVE_MTR,JUCE_VERSION>"
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_USE_MP3AUDIOFORMAT=1)

if(UNIX AND NOT APPLE)
    # Linux is a development target only; JACK makes headless engine testing possible (jackd -d dummy).
    target_compile_definitions(B4ND_LIVE_MTR PRIVATE JUCE_JACK=1)
endif()

if(B4ND_ENABLE_ASIO)
    if(NOT WIN32)
        message(FATAL_ERROR "ASIO is only available on Windows")
    endif()
    if(NOT EXISTS "${B4ND_ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "B4ND_ASIO_SDK_DIR must point to the Steinberg ASIO SDK (common/iasiodrv.h not found)")
    endif()
    target_compile_definitions(B4ND_LIVE_MTR PRIVATE JUCE_ASIO=1)
    target_include_directories(B4ND_LIVE_MTR PRIVATE "${B4ND_ASIO_SDK_DIR}/common")
    message(STATUS "B4ND: ASIO backend enabled (${B4ND_ASIO_SDK_DIR})")
else()
    target_compile_definitions(B4ND_LIVE_MTR PRIVATE JUCE_ASIO=0)
endif()

target_link_libraries(B4ND_LIVE_MTR
    PRIVATE
        b4nd_core
        b4nd_engine
        b4nd_project
        juce::juce_gui_extra
        juce::juce_audio_devices
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
