cmake_minimum_required(VERSION 3.20)
project(CameraDemo VERSION 0.1.0 LANGUAGES CXX)

# Установленный OpenCV использует устаревший FindCUDA; совместимость нужна только ему.
cmake_policy(PUSH)
if(POLICY CMP0146)
    cmake_policy(SET CMP0146 OLD)
endif()
find_package(OpenCV REQUIRED COMPONENTS core)
cmake_policy(POP)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFmpeg REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

add_library(camera_core src/camera/video.cpp src/camera/file_video.cpp src/camera/processor.cpp src/camera/writer.cpp)
target_include_directories(camera_core PUBLIC include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(camera_core PUBLIC ${OpenCV_LIBS})
target_link_libraries(camera_core PRIVATE PkgConfig::FFmpeg)
target_compile_features(camera_core PUBLIC cxx_std_17)
set_target_properties(camera_core PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(camera_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(camera_demo apps/main.cpp)
target_link_libraries(camera_demo PRIVATE camera_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_pipeline tests/test_pipeline.cpp)
    target_link_libraries(test_pipeline PRIVATE camera_core)
    add_test(NAME pipeline COMMAND test_pipeline)

    if(UNIX)
        find_package(Threads REQUIRED)
        add_executable(test_io_cancel tests/test_io_cancel.cpp)
        target_link_libraries(test_io_cancel PRIVATE camera_core Threads::Threads)
        add_test(NAME io_cancel COMMAND test_io_cancel)
        set_tests_properties(io_cancel PROPERTIES TIMEOUT 10)
    endif()

    option(CAMERA_ENABLE_GPU_TESTS "Enable GPU-backed FFmpeg integration tests" OFF)
    if(CAMERA_ENABLE_GPU_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)
        find_program(FFPROBE_EXECUTABLE NAMES ffprobe REQUIRED)
        add_test(NAME gpu_pipeline
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gpu_pipeline.py
                --demo $<TARGET_FILE:camera_demo>
                --ffmpeg ${FFMPEG_EXECUTABLE}
                --ffprobe ${FFPROBE_EXECUTABLE}
                --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/tmp/gpu_pipeline_tests)
        set_tests_properties(gpu_pipeline PROPERTIES LABELS gpu TIMEOUT 120)
    endif()
endif()
