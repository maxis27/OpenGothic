# Runs NetTransportTest as two concurrent processes (host and client).
# Usage: cmake -DTEST_EXE=<path> -DPORT=<port> -P run_nettransport_test.cmake
execute_process(
  COMMAND ${TEST_EXE} host ${PORT}
  COMMAND ${TEST_EXE} connect 127.0.0.1 ${PORT}
  RESULTS_VARIABLE results
  TIMEOUT 60)
message(STATUS "exit codes (host;client): ${results}")
if(NOT results STREQUAL "0;0")
  message(FATAL_ERROR "NetTransport test failed")
endif()
