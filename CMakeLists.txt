cmake_minimum_required(VERSION 3.16)
project(bsaps_edge_device VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BSAPS_BUILD_TESTS "Build unit tests" ON)
option(BSAPS_ENABLE_TEST_HOOKS "Enable deterministic runtime fault-injection hooks" OFF)

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
find_package(OpenCV REQUIRED COMPONENTS core imgproc dnn)
find_package(Threads REQUIRED)

add_library(bsaps_core
    src/camera_capture.cpp
    src/inference.cpp
    src/rtsp_server.cpp)

target_include_directories(bsaps_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${OpenCV_INCLUDE_DIRS})

target_link_libraries(bsaps_core
    PUBLIC
        PkgConfig::LIBCAMERA
        PkgConfig::GSTREAMER
        openvino::runtime
        ${OpenCV_LIBS}
        Threads::Threads)

target_compile_options(bsaps_core PRIVATE -Wall -Wextra -Wpedantic)
if(BSAPS_ENABLE_TEST_HOOKS)
    # RtspServer's test-only state is declared in a public header, so every
    # translation unit that constructs it must see the same class layout.
    target_compile_definitions(bsaps_core PUBLIC BSAPS_ENABLE_TEST_HOOKS=1)
endif()

add_executable(bsaps_app app/main.cpp)
target_link_libraries(bsaps_app PRIVATE bsaps_core)

if(BSAPS_BUILD_TESTS)
    enable_testing()
    add_executable(bsaps_tests tests/test_core.cpp)
    target_link_libraries(bsaps_tests PRIVATE bsaps_core)
    add_test(NAME bsaps_core_tests COMMAND bsaps_tests)

    add_executable(bsaps_model_smoke tests/model_smoke.cpp)
    target_link_libraries(bsaps_model_smoke PRIVATE openvino::runtime Threads::Threads)
endif()

install(TARGETS bsaps_app RUNTIME DESTINATION bin)
