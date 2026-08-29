cmake_minimum_required(VERSION 3.16)
project(eggvision VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EGGVISION_BUILD_TESTS "Build unit tests" ON)
option(EGGVISION_ENABLE_TEST_HOOKS "Enable deterministic runtime fault-injection hooks" OFF)
option(EGGVISION_PI4_TUNING "Tune EggVision core for Raspberry Pi 4 Cortex-A72" OFF)
if(EGGVISION_PI4_TUNING AND
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "EGGVISION_PI4_TUNING requires an AArch64 target")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-allocators-1.0
    gstreamer-video-1.0
    gstreamer-rtsp-server-1.0)

# The Raspberry Pi image installs these config packages outside the default path.
list(APPEND CMAKE_PREFIX_PATH "/usr/local/runtime/cmake" "/usr/local/lib/cmake/opencv4")
find_package(OpenVINO REQUIRED COMPONENTS Runtime)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs dnn)
find_package(Threads REQUIRED)

add_library(eggvision_core
    src/camera_capture.cpp
    src/dma_buf_sync.cpp
    src/encoded_ring_buffer.cpp
    src/event_recorder.cpp
    src/h264_bitstream.cpp
    src/h264_encoder.cpp
    src/i420.cpp
    src/inference.cpp
    src/rtsp_server.cpp
    src/snapshot.cpp)

target_include_directories(eggvision_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${OpenCV_INCLUDE_DIRS})

target_link_libraries(eggvision_core
    PUBLIC
        PkgConfig::LIBCAMERA
        PkgConfig::GSTREAMER
        openvino::runtime
        ${OpenCV_LIBS}
        Threads::Threads)

target_compile_options(eggvision_core PRIVATE -Wall -Wextra -Wpedantic)
if(EGGVISION_PI4_TUNING)
    target_compile_options(eggvision_core PRIVATE -mtune=cortex-a72)
    message(STATUS "EggVision Pi 4 tuning enabled: -mtune=cortex-a72")
endif()
if(EGGVISION_ENABLE_TEST_HOOKS)
    # RtspServer's test-only state is declared in a public header, so every
    # translation unit that constructs it must see the same class layout.
    target_compile_definitions(eggvision_core PUBLIC EGGVISION_ENABLE_TEST_HOOKS=1)
endif()

add_executable(eggvision_app app/main.cpp)
target_link_libraries(eggvision_app PRIVATE eggvision_core)

if(EGGVISION_BUILD_TESTS)
    enable_testing()
    add_executable(eggvision_tests tests/test_core.cpp)
    target_link_libraries(eggvision_tests PRIVATE eggvision_core)
    add_test(NAME eggvision_core_tests COMMAND eggvision_tests)

    add_executable(eggvision_event_core_tests tests/event_core_test.cpp)
    target_link_libraries(eggvision_event_core_tests PRIVATE eggvision_core)
    add_test(NAME eggvision_event_core_tests COMMAND eggvision_event_core_tests)

    add_executable(eggvision_model_smoke tests/model_smoke.cpp)
    target_link_libraries(eggvision_model_smoke PRIVATE openvino::runtime Threads::Threads)

    add_executable(eggvision_normalization_benchmark tests/normalization_benchmark.cpp)
    target_link_libraries(eggvision_normalization_benchmark PRIVATE eggvision_core)

    add_executable(eggvision_normalization_model_compare tests/normalization_model_compare.cpp)
    target_link_libraries(eggvision_normalization_model_compare PRIVATE eggvision_core)

    if(EGGVISION_ENABLE_TEST_HOOKS)
        # Hardware integration harness: run manually on a Pi with a camera.
        # It reuses one RtspServer across normal and fault-rollback epochs.
        add_executable(eggvision_rtsp_restart_test tests/rtsp_restart_harness.cpp)
        target_link_libraries(eggvision_rtsp_restart_test PRIVATE eggvision_core)

        add_executable(eggvision_snapshot_test tests/snapshot_harness.cpp)
        target_link_libraries(eggvision_snapshot_test PRIVATE eggvision_core)

        add_executable(eggvision_event_recording_test tests/event_recording_harness.cpp)
        target_link_libraries(eggvision_event_recording_test PRIVATE eggvision_core)
    endif()
endif()

install(TARGETS eggvision_app RUNTIME DESTINATION bin)
