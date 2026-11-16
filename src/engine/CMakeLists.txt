add_library(b4nd_engine STATIC
    AudioEngine.cpp)

target_include_directories(b4nd_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(b4nd_engine PUBLIC b4nd_core)

find_package(Threads REQUIRED)
target_link_libraries(b4nd_engine PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(b4nd_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(b4nd_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
