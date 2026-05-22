cmake_minimum_required(VERSION 3.20)

project(shmfx
    VERSION 0.1.0
    DESCRIPTION "Embeddable Linux shared-memory framework"
    LANGUAGES CXX)

option(SHMFX_BUILD_TESTS "Build shmfx unit tests" ON)
option(SHMFX_BUILD_EXAMPLES "Build shmfx examples" ON)
option(SHMFX_BUILD_LOGGING "Build shmfx logging reference app" ON)
option(SHMFX_BUILD_BENCHMARKS "Build shmfx benchmark executables" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(shmfx
    src/shm_lifecycle.cpp
    src/shm_manager.cpp
    src/shm_registry.cpp
    src/shm_ring.cpp
    src/shm_security.cpp
    src/shm_sync.cpp)

add_library(shmfx::shmfx ALIAS shmfx)

target_include_directories(shmfx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(shmfx PUBLIC Threads::Threads)

if(SHMFX_BUILD_LOGGING)
    add_library(shmfx_logging
        apps/logging/src/log_center.cpp
        apps/logging/src/logger.cpp)

    add_library(shmfx::logging ALIAS shmfx_logging)

    target_include_directories(shmfx_logging
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/apps/logging/include>
            $<INSTALL_INTERFACE:include>)

    target_link_libraries(shmfx_logging PUBLIC shmfx::shmfx)
endif()

if(SHMFX_BUILD_EXAMPLES)
    add_executable(raw_segment_example examples/raw_segment.cpp)
    target_link_libraries(raw_segment_example PRIVATE shmfx::shmfx)

    add_executable(ring_mpsc_example examples/ring_mpsc.cpp)
    target_link_libraries(ring_mpsc_example PRIVATE shmfx::shmfx)

    if(SHMFX_BUILD_LOGGING)
        add_executable(logging_demo_producer apps/logging/examples/demo_producer.cpp)
        target_link_libraries(logging_demo_producer PRIVATE shmfx::logging)

        add_executable(logging_demo_multi_producer apps/logging/examples/demo_multi_producer.cpp)
        target_link_libraries(logging_demo_multi_producer PRIVATE shmfx::logging)
    endif()
endif()

if(SHMFX_BUILD_BENCHMARKS)
    add_executable(ring_benchmark benchmark/ring_benchmark.cpp)
    target_link_libraries(ring_benchmark PRIVATE shmfx::shmfx)
endif()

if(SHMFX_BUILD_TESTS)
    enable_testing()

    add_executable(phase3_manager_registry_test
        tests/phase3_manager_registry_test.cpp)
    target_link_libraries(phase3_manager_registry_test PRIVATE shmfx::shmfx)
    add_test(NAME phase3_manager_registry_test COMMAND phase3_manager_registry_test)

    add_executable(oi07_mutex_guard_test
        tests/oi07_mutex_guard_test.cpp
        src/shm_lifecycle.cpp)
    target_include_directories(oi07_mutex_guard_test PRIVATE include)
    target_link_libraries(oi07_mutex_guard_test PRIVATE Threads::Threads)
    add_test(NAME oi07_mutex_guard_test COMMAND oi07_mutex_guard_test)

    add_executable(phase4_lifecycle_test
        tests/phase4_lifecycle_test.cpp)
    target_link_libraries(phase4_lifecycle_test PRIVATE shmfx::shmfx)
    add_test(NAME phase4_lifecycle_test COMMAND phase4_lifecycle_test)

    add_executable(phase5_security_test
        tests/phase5_security_test.cpp)
    target_link_libraries(phase5_security_test PRIVATE shmfx::shmfx)
    add_test(NAME phase5_security_test COMMAND phase5_security_test)

    add_executable(phase6_ring_test
        tests/phase6_ring_test.cpp)
    target_link_libraries(phase6_ring_test PRIVATE shmfx::shmfx)
    add_test(NAME phase6_ring_test COMMAND phase6_ring_test)

    if(SHMFX_BUILD_LOGGING)
        add_executable(phase7_logger_test
            tests/phase7_logger_test.cpp)
        target_link_libraries(phase7_logger_test PRIVATE shmfx::logging)
        add_test(NAME phase7_logger_test COMMAND phase7_logger_test)

        add_executable(phase8_log_center_test
            tests/phase8_log_center_test.cpp)
        target_link_libraries(phase8_log_center_test PRIVATE shmfx::logging)
        add_test(NAME phase8_log_center_test COMMAND phase8_log_center_test)

        add_executable(phase10_ipc_logging_test
            tests/phase10_ipc_logging_test.cpp)
        target_link_libraries(phase10_ipc_logging_test PRIVATE shmfx::logging)
        add_test(NAME phase10_ipc_logging_test COMMAND phase10_ipc_logging_test)
    endif()

    add_executable(phase10_mpsc_stress_test
        tests/phase10_mpsc_stress_test.cpp)
    target_link_libraries(phase10_mpsc_stress_test PRIVATE shmfx::shmfx)
    add_test(NAME phase10_mpsc_stress_test COMMAND phase10_mpsc_stress_test)
endif()
