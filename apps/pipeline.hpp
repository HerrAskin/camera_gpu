#pragma once

#include "arguments_parser.hpp"
#include "camera/video_capture.hpp"
#include "camera/processor.hpp"
#include "camera/video_writer.hpp"

#include <chrono>
#include <exception>
#include <atomic>
#include <thread>
#include <memory>

namespace demo
{
/** Итоговые счётчики и полное время работы, включая инициализацию и завершение. */
struct PipelineStatistics
{
    std::size_t processedFrames = 0;
    std::size_t droppedFrames = 0;
    double elapsedSeconds = 0;
};

/** Один источник и его Processor/Writer; start запускает отдельный рабочий поток. */
class Pipeline
{
public:
    Pipeline(Command command, const std::atomic<bool>& interrupted);
    ~Pipeline();
    bool init();
    void start();
    bool isRunning() const noexcept;
    /** Безопасно вызывается повторно и после неуспешного init. */
    void stop() noexcept;
    int exitCode() const noexcept;
    /** Статистика доступна управляющему потоку после stop/join. */
    const PipelineStatistics& statistics() const noexcept;

private:
    void run();
    void finishWorker(std::chrono::steady_clock::time_point startTime);
    bool initializePipeline();
    void reportError(const std::exception& error) noexcept;

    // Session передаёт общий атомарный запрос остановки всем конвейерам.
    const std::atomic<bool>& interrupted;
    Command command;
    std::unique_ptr<camera::VideoCapture> capture;
    camera::Processor processor;
    std::unique_ptr<camera::VideoWriter> writer;
    bool initialized = false;
    bool started = false;
    bool stopped = false;
    int result = 1;
    std::atomic<bool> running{false};
    std::thread worker;
    PipelineStatistics metrics;
};
}
