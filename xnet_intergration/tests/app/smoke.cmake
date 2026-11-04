file(MAKE_DIRECTORY "${WORK}/config")
file(COPY "${MODEL}" DESTINATION "${WORK}")
file(WRITE "${WORK}/config/runner.json"
    "{\"interface_name\":\"CAN1\",\"model_path\":\"../device.json\",\"persistence_path\":\"../device.lss.json\"}")
execute_process(COMMAND "${APP}" --lib "${LIB}" --config "${WORK}/config/runner.json" --duration 2
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "App smoke test failed (${result}):\n${output}\n${errors}")
endif()
string(REGEX MATCHALL "[^\n]*runtime_state=[^\n]*" snapshots "${output}")
list(LENGTH snapshots count)
if(count LESS 4 OR count GREATER 5)
    message(FATAL_ERROR "Expected 4-5 snapshots over two seconds, got ${count}:\n${output}")
endif()
set(previous_ms "")
foreach(snapshot IN LISTS snapshots)
    foreach(field runtime_state nmt_state active_node_id pending_node_id active_bitrate_kbps pending_bitrate_kbps
                  can_error_status node_guarding_timeout reset_node_count reset_communication_count)
        if(NOT snapshot MATCHES " ${field}=")
            message(FATAL_ERROR "Status missing ${field}:\n${snapshot}")
        endif()
    endforeach()
    string(REGEX MATCH "T([0-9][0-9]):([0-9][0-9]):([0-9][0-9])\\.([0-9][0-9][0-9])Z" stamp "${snapshot}")
    if(NOT stamp)
        message(FATAL_ERROR "Status missing millisecond UTC timestamp:\n${snapshot}")
    endif()
    math(EXPR current_ms
        "(((1${CMAKE_MATCH_1}-100)*60 + 1${CMAKE_MATCH_2}-100)*60 + 1${CMAKE_MATCH_3}-100)*1000 + 1${CMAKE_MATCH_4}-1000")
    if(NOT previous_ms STREQUAL "")
        math(EXPR interval "(${current_ms} - ${previous_ms} + 86400000) % 86400000")
        if(interval LESS 400 OR interval GREATER 750)
            message(FATAL_ERROR "Expected approximately 500 ms between snapshots, got ${interval} ms:\n${output}")
        endif()
    endif()
    set(previous_ms "${current_ms}")
endforeach()
if(NOT output MATCHES "runtime_state=RUNNING\\(2\\).*nmt_state=PRE_OPERATIONAL\\(127\\).*active_node_id=10"
   OR NOT output MATCHES "active_bitrate_kbps=125"
   OR NOT errors MATCHES "Device destroyed; cleanup complete")
    message(FATAL_ERROR "Unexpected status or cleanup:\n${output}\n${errors}")
endif()

execute_process(COMMAND "${APP}" --lib "${WORK}/missing-library" --config "${WORK}/config/runner.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(result STREQUAL "0" OR NOT errors MATCHES "(LoadLibrary|dlopen) failed")
    message(FATAL_ERROR "Missing library was not diagnosed:\n${output}\n${errors}")
endif()
execute_process(COMMAND "${APP}" --lib "${APP}" --config "${WORK}/config/runner.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(result STREQUAL "0")
    message(FATAL_ERROR "App accepted a non-library file")
endif()

file(WRITE "${WORK}/config/invalid.json" "{\"interface_name\":\"CAN1\"}")
execute_process(COMMAND "${APP}" --lib "${LIB}" --config "${WORK}/config/invalid.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(result STREQUAL "0" OR NOT errors MATCHES "model_path")
    message(FATAL_ERROR "Missing config field was not diagnosed:\n${output}\n${errors}")
endif()
file(WRITE "${WORK}/bad-model.json" "{\"schema_version\":999}")
file(WRITE "${WORK}/config/bad-model.json"
    "{\"interface_name\":\"CAN1\",\"model_path\":\"../bad-model.json\",\"persistence_path\":\"../device.lss.json\"}")
execute_process(COMMAND "${APP}" --lib "${LIB}" --config "${WORK}/config/bad-model.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(result STREQUAL "0" OR NOT errors MATCHES "xnet_canopen_initialize returned"
   OR NOT errors MATCHES "Latest native diagnostic" OR NOT errors MATCHES "cleanup complete")
    message(FATAL_ERROR "Initialization failure was not diagnosed/cleaned up:\n${output}\n${errors}")
endif()
foreach(duration 0 -1 nan 1.5 4294967296)
    execute_process(COMMAND "${APP}" --lib "${LIB}" --config "${WORK}/config/runner.json" --duration "${duration}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
    if(result STREQUAL "0")
        message(FATAL_ERROR "App accepted invalid duration ${duration}")
    endif()
endforeach()
message(STATUS "Device app status, relative paths, timed shutdown and failure checks passed")
