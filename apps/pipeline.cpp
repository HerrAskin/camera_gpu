#include "pipeline.hpp"
#include "camera/video_capture.hpp"
#include "camera/video_writer.hpp"
#include "camera/processor.hpp"
#include "camera/mock_capture.hpp"

#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace demo
{
namespace
{
std::unique_ptr<camera::VideoCapture> createCapture(const Command& command)
{
    if (command.mode == Mode::Mock)
    {
        return camera::createMockCapture();
    }
    camera::VideoCapture::Options options;
    options.queueDepth = command.queueDepth;
    options.reconnectAttempts = command.reconnectAttempts;
    options.preferGpu = !command.forceCpu;
    options.keepAllFrames = command.keepAllFrames;
    const bool writesFrames = command.mode == Mode::Decode || command.mode == Mode::Transcode;
    options.additionalHeldFrames = writesFrames && !command.forceCpu && command.writeQueueDepth ? command.writeQueueDepth + 2 : 0;
    auto capture = std::make_unique<camera::VideoCapture>();
    return capture->init(command.inputPath, options) ? std::move(capture) : nullptr;
}

camera::VideoWriter::Options writerOptions(const Command& command)
{
    camera::VideoWriter::Options options;
    options.format = command.mode == Mode::Transcode ? camera::VideoWriter::Format::H264Mp4 : camera::VideoWriter::Format::Csv;
    options.queueDepth = command.writeQueueDepth;
    options.preferGpu = !command.forceCpu;
    return options;
}

std::optional<camera::Frame> readFrame(camera::VideoCapture& capture, bool& finished, bool& failed)
{
    try
    {
        auto frame = capture.read();
        if (!frame && capture.isFinished())
        {
            // Последний кадр/ошибка могли появиться между read и isFinished.
            frame = capture.read();
            finished = !frame;
        }
        return frame;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[main] Ошибка чтения: " << error.what() << '\n';
        finished = failed = true;
        return std::nullopt;
    }
}

void printFrame(const camera::Frame& frame)
{
    std::cout << "[main] Размер: " << frame.width() << 'x' << frame.height()
              << ", формат: " << (frame.gpuImage ? "CUDA/GPU" : "CPU") << ", timestamp_us: ";
    if (frame.timestamp)
    {
        std::cout << frame.timestamp->count();
    }
    else
    {
        std::cout << "не задан";
    }
    std::cout << '\n';
}

int showFirstFrame(camera::VideoCapture& capture, const std::atomic<bool>& interrupted)
{
    bool finished = false;
    bool failed = false;
    while (!interrupted && !finished)
    {
        auto frame = readFrame(capture, finished, failed);
        if (frame)
        {
            printFrame(*frame);
            capture.requestStop();
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    capture.requestStop();
    if (interrupted)
    {
        std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
        return 0;
    }
    std::cerr << "[main] Видеопоток не содержит декодируемых кадров\n";
    return 1;
}

struct PipelineState
{
    std::size_t frameCount = 0;
    bool finished = false;
    bool readFailed = false;
    bool usedGpu = false;
    bool usedCpu = false;
    std::optional<std::uint64_t> generation;
};

void writeFrame(camera::VideoCapture& capture, camera::VideoWriter& writer, const camera::Frame& frame)
{
    try
    {
        writer.write(frame);
    }
    catch (...)
    {
        capture.requestStop();
        // Дописываем принятые кадры; ошибка записи остаётся терминальной.
        try
        {
            writer.finish();
        }
        catch (const std::exception& error)
        {
            std::cerr << "[main] Завершение записи после ошибки: " << error.what() << '\n';
        }
        throw;
    }
}

void processNextFrame(camera::VideoCapture& capture, camera::Processor& processor, camera::VideoWriter& writer, PipelineState& state, const Command& command, PipelineStatistics& metrics)
{
    auto frame = readFrame(capture, state.finished, state.readFailed);
    if (!frame)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    const bool newConnection = !state.generation || *state.generation != frame->streamGeneration;
    if (command.mode == Mode::Transcode && newConnection && !frame->timestamp)
    {
        std::cout << "[main] Пропущен начальный кадр без timestamp\n";
        return;
    }
    processor.process(*frame);
    writeFrame(capture, writer, *frame);
    state.generation = frame->streamGeneration;
    state.usedGpu = state.usedGpu || bool(frame->gpuImage);
    state.usedCpu = state.usedCpu || !frame->gpuImage;
    ++state.frameCount;
    ++metrics.processedFrames;
}

int finishPipeline(camera::VideoCapture& capture, camera::VideoWriter& writer, const PipelineState& state, const Command& command, const std::atomic<bool>& interrupted)
{
    capture.requestStop();
    if (interrupted)
    {
        std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
    }
    if (command.mode == Mode::Transcode && state.frameCount == 0)
    {
        std::cout << "[main] Кадры не получены; MP4 не сформирован\n";
        return interrupted ? 0 : 1;
    }
    writer.finish();
    const char* backend = state.usedGpu ? (state.usedCpu ? "GPU/CPU" : "CUDA/GPU") : "CPU";
    std::cout << "[main] Обработано: " << state.frameCount << " кадров, " << backend
              << "; результат сохранён: " << command.outputPath << '\n';
    return state.readFailed && !interrupted ? 1 : 0;
}

int reportCaptureFailure(const std::atomic<bool>& interrupted)
{
    if (interrupted)
    {
        std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
    }
    return interrupted ? 0 : 1;
}

int runDiagnostic(camera::VideoCapture& capture, Mode mode, const std::atomic<bool>& interrupted)
{
    if (mode == Mode::Inspect)
    {
        std::cout << capture.description() << '\n';
        return 0;
    }
    return showFirstFrame(capture, interrupted);
}

int pipeline(camera::VideoCapture& capture, camera::Processor& processor, camera::VideoWriter& writer, const Command& command, const std::atomic<bool>& interrupted, PipelineStatistics& metrics)
{
    PipelineState state;
    while (!interrupted && !state.finished)
    {
        processNextFrame(capture, processor, writer, state, command, metrics);
    }
    return finishPipeline(capture, writer, state, command, interrupted);
}
}

Pipeline::Pipeline(Command value, const std::atomic<bool>& stopFlag) : interrupted(stopFlag), command(std::move(value))
{
}

Pipeline::~Pipeline()
{
    stop();
}

void Pipeline::start()
{
    running.store(true);
    try
    {
        worker = std::thread([this]
        {
            const auto startTime = std::chrono::steady_clock::now();
            if (init())
            {
                run();
            }
            finishWorker(startTime);
            running.store(false);
        });
    }
    catch (...)
    {
        running.store(false);
        throw;
    }
}

void Pipeline::finishWorker(std::chrono::steady_clock::time_point startTime)
{
    if (capture)
    {
        metrics.droppedFrames = capture->droppedCount();
        capture->requestStop();
    }
    // Включаем в измерение drain, закрытие файлов и join потоков захвата/записи.
    capture.reset();
    writer.reset();
    metrics.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    const double fps = metrics.elapsedSeconds > 0 ? metrics.processedFrames / metrics.elapsedSeconds : 0;
    std::ostringstream message;
    message << std::fixed << std::setprecision(6)
            << "[Pipeline] Выход: " << command.outputPath
            << "; кадров: " << metrics.processedFrames << "; время: " << metrics.elapsedSeconds
            << " с; средний FPS: " << fps << "; пропущено очередью: " << metrics.droppedFrames << '\n';
    std::cout << message.str();
}

const PipelineStatistics& Pipeline::statistics() const noexcept
{
    return metrics;
}

bool Pipeline::isRunning() const noexcept
{
    return running.load();
}

bool Pipeline::init()
{
    if (initialized || stopped)
    {
        return false;
    }
    try
    {
        return initializePipeline();
    }
    catch (const std::exception& error)
    {
        reportError(error);
        return false;
    }
}

bool Pipeline::initializePipeline()
{
    capture = createCapture(command);
    if (!capture)
    {
        result = reportCaptureFailure(interrupted);
        return false;
    }
    std::cout << "[Pipeline] Настройка decoder для " << command.outputPath << ":\n" << capture->description() << '\n';
    const bool diagnostic = command.mode == Mode::Inspect || command.mode == Mode::DecodeFirst;
    if (!diagnostic)
    {
        writer = std::make_unique<camera::VideoWriter>();
    }
    if (writer && !writer->init(command.outputPath, writerOptions(command)))
    {
        return false;
    }
    initialized = true;
    return true;
}

void Pipeline::run()
{
    if (!initialized || stopped || started)
    {
        return;
    }
    started = true;
    try
    {
        const bool diagnostic = command.mode == Mode::Inspect || command.mode == Mode::DecodeFirst;
        result = diagnostic ? runDiagnostic(*capture, command.mode, interrupted) : pipeline(*capture, processor, *writer, command, interrupted, metrics);
    }
    catch (const std::exception& error)
    {
        reportError(error);
    }
}

void Pipeline::stop() noexcept
{
    if (stopped)
    {
        return;
    }
    if (worker.joinable())
    {
        worker.join();
    }
    stopped = true;
    if (capture)
    {
        capture->requestStop();
    }
    // После join владельцы доступны только управляющему потоку.
    capture.reset();
    writer.reset();
}

int Pipeline::exitCode() const noexcept
{
    return result;
}

void Pipeline::reportError(const std::exception& error) noexcept
{
    result = 1;
    std::cerr << "[main] Ошибка: " << error.what() << '\n';
}
}
