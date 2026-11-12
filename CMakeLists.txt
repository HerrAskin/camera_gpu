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
find_package(Threads REQUIRED)
pkg_check_modules(FFmpeg REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

add_library(camera_core src/camera/frame.cpp src/camera/mock_capture.cpp src/camera/video_capture.cpp src/camera/video_writer.cpp src/camera/frame_queue.cpp)
target_include_directories(camera_core PUBLIC include ${OpenCV_INCLUDE_DIRS} ${FFmpeg_INCLUDE_DIRS})
target_link_libraries(camera_core PUBLIC ${OpenCV_LIBS})
target_link_libraries(camera_core PRIVATE PkgConfig::FFmpeg)
target_link_libraries(camera_core PUBLIC Threads::Threads)
target_compile_features(camera_core PUBLIC cxx_std_17)
set_target_properties(camera_core PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(camera_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(camera_demo apps/main.cpp apps/arguments_parser.cpp apps/session.cpp apps/pipeline.cpp)
target_link_libraries(camera_demo PRIVATE camera_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_pipeline tests/test_pipeline.cpp)
    target_link_libraries(test_pipeline PRIVATE camera_core)
    add_test(NAME pipeline COMMAND test_pipeline)

    add_executable(test_frame_queue tests/test_frame_queue.cpp)
    target_link_libraries(test_frame_queue PRIVATE camera_core)
    add_test(NAME frame_queue COMMAND test_frame_queue)
    set_tests_properties(frame_queue PROPERTIES TIMEOUT 10)

    add_executable(test_writer_queue tests/test_writer_queue.cpp)
    target_link_libraries(test_writer_queue PRIVATE camera_core)
    add_test(NAME writer_queue COMMAND test_writer_queue)
    set_tests_properties(writer_queue PROPERTIES TIMEOUT 10)

    if(UNIX)
        add_executable(test_io_cancel tests/test_io_cancel.cpp)
        target_link_libraries(test_io_cancel PRIVATE camera_core Threads::Threads)
        add_test(NAME io_cancel COMMAND test_io_cancel)
        set_tests_properties(io_cancel PROPERTIES TIMEOUT 10)
    endif()

    option(CAMERA_ENABLE_CPU_TESTS "Enable CPU-only FFmpeg integration tests" OFF)
    option(CAMERA_ENABLE_GPU_TESTS "Enable GPU-backed FFmpeg integration tests" OFF)
    if(CAMERA_ENABLE_CPU_TESTS OR CAMERA_ENABLE_GPU_TESTS)
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        find_program(FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)
        find_program(FFPROBE_EXECUTABLE NAMES ffprobe REQUIRED)
    endif()
    if(CAMERA_ENABLE_GPU_TESTS)
        add_test(NAME gpu_pipeline
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gpu_pipeline.py
                --demo $<TARGET_FILE:camera_demo>
                --ffmpeg ${FFMPEG_EXECUTABLE}
                --ffprobe ${FFPROBE_EXECUTABLE}
                --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/tmp/gpu_pipeline_tests)
        set_tests_properties(gpu_pipeline PROPERTIES LABELS gpu TIMEOUT 120)
    endif()
    if(CAMERA_ENABLE_CPU_TESTS OR CAMERA_ENABLE_GPU_TESTS)
        add_test(NAME cpu_pipeline
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cpu_pipeline.py
                --demo $<TARGET_FILE:camera_demo>
                --ffmpeg ${FFMPEG_EXECUTABLE}
                --ffprobe ${FFPROBE_EXECUTABLE}
                --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/tmp/cpu_pipeline_tests)
        set_tests_properties(cpu_pipeline PROPERTIES LABELS cpu TIMEOUT 60)
        add_test(NAME multi_source
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_multi_source.py
                --demo $<TARGET_FILE:camera_demo>
                --ffmpeg ${FFMPEG_EXECUTABLE}
                --ffprobe ${FFPROBE_EXECUTABLE}
                --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/tmp/multi_source_tests)
        set_tests_properties(multi_source PROPERTIES LABELS cpu TIMEOUT 60)
    endif()
    if(CAMERA_ENABLE_GPU_TESTS)
        if(UNIX)
            add_test(NAME rtsp_faults
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rtsp_faults.py
                    --demo $<TARGET_FILE:camera_demo>
                    --ffmpeg ${FFMPEG_EXECUTABLE}
                    --ffprobe ${FFPROBE_EXECUTABLE}
                    --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/tmp/rtsp_fault_tests)
            set_tests_properties(rtsp_faults PROPERTIES LABELS "gpu;network" TIMEOUT 120)
        endif()
    endif()
endif()
