find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(diagnostics_tests diagnostics_tests.cpp)
add_test(diagnostics_tests diagnostics_tests)
target_link_libraries(diagnostics_tests PRIVATE GTest::Main diagnostics)

add_executable(diagnostics_server_main main.cpp $<TARGET_OBJECTS:logging_dev>)
target_link_libraries(diagnostics_server_main PRIVATE diagnostics Threads::Threads)
