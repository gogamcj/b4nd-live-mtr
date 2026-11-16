add_executable(b4nd_project_tests
    ../core/TestMain.cpp
    ProjectDatabaseTests.cpp)

target_include_directories(b4nd_project_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core)
target_link_libraries(b4nd_project_tests PRIVATE b4nd_project b4nd_sqlite)
if(MSVC)
    target_compile_options(b4nd_project_tests PRIVATE /utf-8)
endif()
add_test(NAME b4nd_project_tests COMMAND b4nd_project_tests)
