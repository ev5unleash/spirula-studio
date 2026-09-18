if(NOT EXISTS "${SS_TEST_EXECUTABLE}")
    message(FATAL_ERROR "Missing test executable: ${SS_TEST_EXECUTABLE}")
endif()

file(MAKE_DIRECTORY "${SS_TEST_ARTIFACTS}")
file(LOCK "${SS_TEST_ARTIFACTS}/allocation.lock" GUARD PROCESS TIMEOUT 10)
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef SS_TEST_NONCE)
set(SS_TEST_ROOT "${SS_TEST_ARTIFACTS}/${SS_TEST_NAME}-${SS_TEST_NONCE}")
if(EXISTS "${SS_TEST_ROOT}")
    message(FATAL_ERROR "Test scratch directory already exists: ${SS_TEST_ROOT}")
endif()
file(MAKE_DIRECTORY "${SS_TEST_ROOT}/tmp" "${SS_TEST_ROOT}/home"
    "${SS_TEST_ROOT}/config" "${SS_TEST_ROOT}/cache")
file(LOCK "${SS_TEST_ARTIFACTS}/allocation.lock" RELEASE)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "APPDATA=${SS_TEST_ROOT}/config"
        "LOCALAPPDATA=${SS_TEST_ROOT}/cache"
        "HOME=${SS_TEST_ROOT}/home"
        "XDG_CONFIG_HOME=${SS_TEST_ROOT}/config"
        "XDG_CACHE_HOME=${SS_TEST_ROOT}/cache"
        "TMP=${SS_TEST_ROOT}/tmp"
        "TEMP=${SS_TEST_ROOT}/tmp"
        "TMPDIR=${SS_TEST_ROOT}/tmp"
        "SS_NO_AUTO_FETCH=1"
        "${SS_TEST_EXECUTABLE}" ${SS_TEST_ARGS}
    WORKING_DIRECTORY "${SS_TEST_ROOT}"
    OUTPUT_FILE "${SS_TEST_ROOT}/process.log"
    ERROR_FILE "${SS_TEST_ROOT}/process.log"
    RESULT_VARIABLE SS_TEST_RESULT)
file(READ "${SS_TEST_ROOT}/process.log" SS_TEST_OUTPUT)
message("${SS_TEST_OUTPUT}")
if(NOT "${SS_TEST_RESULT}" STREQUAL "0")
    message(FATAL_ERROR
        "${SS_TEST_NAME} failed (${SS_TEST_RESULT}); artifacts: ${SS_TEST_ROOT}")
endif()
file(REMOVE_RECURSE "${SS_TEST_ROOT}")
