add_executable(xgi_tests
    unit/test_main.cpp
    unit/test_commands.cpp
    unit/test_config.cpp
    unit/test_placement.cpp
    unit/test_scheduling.cpp
)
target_link_libraries(xgi_tests PRIVATE xi_lib xgi_lib)
add_test(NAME unit COMMAND xgi_tests)

add_executable(mock-llama-server mocks/mock_llama_server.cpp)
target_link_libraries(mock-llama-server PRIVATE xgi_common)

add_executable(mock-rpc-server mocks/mock_rpc_server.cpp)
target_link_libraries(mock-rpc-server PRIVATE ws2_32)

add_test(NAME e2e
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run-e2e.ps1
            -Xgi $<TARGET_FILE:xgi> -Xi $<TARGET_FILE:xi>
            -MockLlama $<TARGET_FILE:mock-llama-server> -MockRpc $<TARGET_FILE:mock-rpc-server>)
set_tests_properties(e2e PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
