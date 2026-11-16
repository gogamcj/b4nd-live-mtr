cmake_minimum_required(VERSION 3.22)

project(B4ND_LIVE_MTR VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(B4ND_BUILD_APP   "Build the GUI application" ON)
option(B4ND_BUILD_TESTS "Build unit tests" ON)
option(B4ND_ENABLE_ASIO "Enable the ASIO backend (Windows, requires the Steinberg ASIO SDK)" OFF)
set(B4ND_ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK root (folder containing common/iasiodrv.h)")

# --- Core library: pure logic, no JUCE dependency, unit-tested on every platform ---
add_subdirectory(src/core)
add_subdirectory(src/engine)

if(B4ND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/core)
    add_subdirectory(tests/engine)
endif()

if(B4ND_BUILD_APP)
    # JUCE: pass -DFETCHCONTENT_SOURCE_DIR_JUCE=<path> to use a local checkout.
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    add_subdirectory(src/app)
    if(B4ND_BUILD_TESTS)
        add_subdirectory(tests/media)
    endif()
endif()
