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
target_link_libraries(mock-rpc-server PRIVATE xgi_common)

# End-to-end test: needs PowerShell 7 (pwsh, any OS) or Windows PowerShell 5.1.
find_program(XGI_PWSH NAMES pwsh pwsh-preview)
if(NOT XGI_PWSH AND WIN32)
    find_program(XGI_PWSH NAMES powershell)
endif()
if(XGI_PWSH)
    add_test(NAME e2e
        COMMAND ${XGI_PWSH} -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run-e2e.ps1
                -Xgi $<TARGET_FILE:xgi> -Xi $<TARGET_FILE:xi>
                -MockLlama $<TARGET_FILE:mock-llama-server> -MockRpc $<TARGET_FILE:mock-rpc-server>)
    set_tests_properties(e2e PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
else()
    message(STATUS "pwsh not found: the full e2e test is not registered (install PowerShell 7 to enable it)")
endif()

# POSIX smoke test: sh + curl only (Linux, macOS).
if(NOT WIN32)
    find_program(XGI_CURL curl)
    if(XGI_CURL)
        add_test(NAME smoke
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/e2e/smoke.sh
                    $<TARGET_FILE:xgi> $<TARGET_FILE:xi>
                    $<TARGET_FILE:mock-llama-server> $<TARGET_FILE:mock-rpc-server>)
        set_tests_properties(smoke PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
endif()
