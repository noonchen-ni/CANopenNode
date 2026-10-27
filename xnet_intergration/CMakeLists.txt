cmake_minimum_required(VERSION 3.16)
project(xnet_canopen VERSION 0.1.0 LANGUAGES C CXX)

option(XNET_CANOPEN_BUILD_TESTS "Build hardware-independent tests with a fake XNET API" OFF)
option(XNET_CANOPEN_BUILD_LIBRARY "Build the shared library linked to NI-XNET" ON)
set(XNET_LIBRARY "" CACHE FILEPATH "Path to the target NI-XNET shared library")
find_package(Threads REQUIRED)

set(STACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(STACK_SOURCES
    "${STACK_ROOT}/CANopen.c"
    "${STACK_ROOT}/301/CO_ODinterface.c"
    "${STACK_ROOT}/301/CO_NMT_Heartbeat.c"
    "${STACK_ROOT}/301/CO_Node_Guarding.c"
    "${STACK_ROOT}/301/CO_Emergency.c"
    "${STACK_ROOT}/301/CO_SDOserver.c"
    "${STACK_ROOT}/301/CO_SYNC.c"
    "${STACK_ROOT}/301/CO_PDO.c"
    "${STACK_ROOT}/305/CO_LSSslave.c")
set(PORT_SOURCES src/model.cpp src/persistence.cpp src/driver.cpp src/device.cpp)

add_library(xnet_canopen_objects OBJECT ${STACK_SOURCES} ${PORT_SOURCES})
set_target_properties(xnet_canopen_objects PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_compile_features(xnet_canopen_objects PUBLIC c_std_99 cxx_std_17)
target_include_directories(xnet_canopen_objects PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${STACK_ROOT}")
target_link_libraries(xnet_canopen_objects PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(xnet_canopen_objects PRIVATE /W4)
else()
    target_compile_options(xnet_canopen_objects PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(XNET_CANOPEN_BUILD_LIBRARY)
    if(NOT XNET_LIBRARY)
        find_library(XNET_LIBRARY_FOUND NAMES nixnet)
        if(NOT XNET_LIBRARY_FOUND)
            message(FATAL_ERROR "Set XNET_LIBRARY to the target NI-XNET library, or disable XNET_CANOPEN_BUILD_LIBRARY for mock tests")
        endif()
        set(XNET_LIBRARY "${XNET_LIBRARY_FOUND}")
    endif()
    add_library(xnet_canopen SHARED $<TARGET_OBJECTS:xnet_canopen_objects>)
    target_link_libraries(xnet_canopen PRIVATE Threads::Threads "${XNET_LIBRARY}")
    target_link_options(xnet_canopen PRIVATE "$<$<PLATFORM_ID:Linux>:-Wl,--no-undefined>")
    set_target_properties(xnet_canopen PROPERTIES VERSION 0.1.0 SOVERSION 0)
    install(TARGETS xnet_canopen LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
    install(FILES include/xnet_canopen.h DESTINATION include)
endif()

if(XNET_CANOPEN_BUILD_TESTS)
    enable_testing()
    add_executable(xnet_canopen_tests tests/test_integration.cpp tests/fake_xnet.cpp tests/api_c.c)
    target_link_libraries(xnet_canopen_tests PRIVATE xnet_canopen_objects Threads::Threads)
    target_compile_definitions(xnet_canopen_tests PRIVATE
        XNET_CO_EXAMPLE_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/examples/device.json")
    add_test(NAME xnet_canopen_tests COMMAND xnet_canopen_tests)
endif()
