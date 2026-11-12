#pragma once

#include "camera/frame.hpp"
#include "camera/frame_queue.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

namespace camera
{
/** Записывает CSV или сегментированные H.264 MP4-файлы.
 * При ненулевой queueDepth кадры передаются ограниченной FIFO-очередью worker-потоку.
 * Переполнение закрывает приём, сохраняет уже принятые кадры и сообщает ошибку через write/finish.
 * Для CPU MP4 принимает BGR CV_8UC3; для CUDA сначала предпочитает NVENC и при сбое открытия
 * encoder переключается на software H.264. Изменение streamGeneration начинает новый MP4-файл.
 * finish() дожидается worker, записывает trailer и освобождает выход; его нужно вызвать явно.
 */
class VideoWriter
{
public:
    enum class Format
    {
        Csv,
        H264Mp4
    };
    /** Настройки формата, bounded queue и предпочтения NVENC/software encoder. */
    struct Options
    {
        Format format = Format::Csv;
        std::size_t queueDepth = 0;
        // false гарантирует software encoder даже для CUDA input.
        bool preferGpu = true;
    };

    VideoWriter() = default;
    /** Останавливает worker и освобождает ресурсы; корректный MP4 требует явного finish(). */
    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;
    VideoWriter(VideoWriter&&) = delete;
    VideoWriter& operator=(VideoWriter&&) = delete;

    /** Открывает выход; возвращает false, если его нельзя создать или объект уже запущен. */
    bool init(const std::filesystem::path& path, Format format = Format::Csv, std::size_t queueDepth = 0);
    /** Открывает выход с выбором GPU/software encoder и параметров очереди. */
    bool init(const std::filesystem::path& path, const Options& options);
    /** Принимает кадр; ошибка worker или переполнение очереди становятся терминальными. */
    void write(const Frame& frame);
    /** Дренирует очередь, закрывает последний сегмент и повторно сообщает сохранённые ошибки. */
    void finish();
    /** Возвращает созданные файлы; безопасно вызывать одновременно с записью. */
    std::vector<std::filesystem::path> outputPaths() const;

private:
    static void freeFormat(AVFormatContext* context);
    static void freeCodec(AVCodecContext* context);
    static void freePacket(AVPacket* packet);
    static void freeFrame(AVFrame* frame);
    static void freeScaler(SwsContext* scaler);
    void fail(const std::string& message, const char* operation = "VideoWriter::write");
    void openOutput(const std::filesystem::path& path);
    void configureOutput(const std::filesystem::path& path, const Options& requested);
    void startWorker(std::size_t queueDepth);
    void cleanupFailedInit();
    void enqueueFrame(const Frame& frame);
    void writeDirectFrame(const Frame& frame);
    void waitForFirstFrame(std::unique_lock<std::mutex>& lock, std::uint64_t generation);
    void rethrowWorkerError() const;
    void rethrowAdmissionError() const;
    void reportFinished();
    void finishOutput();
    void configureEncoder(AVCodecContext* context, const Frame& frame, int pixelFormat);
    void openEncoder(const Frame& frame);
    void openGpuEncoder(const Frame& frame);
    void openCpuEncoder(const Frame& frame);
    void writeHeader();
    void writeCsv(const Frame& frame);
    void writeVideo(const Frame& frame);
    void validateVideoFrame(const Frame& frame);
    std::unique_ptr<AVFrame, decltype(&freeFrame)> prepareEncoderFrame(const Frame& frame);
    std::unique_ptr<AVFrame, decltype(&freeFrame)> prepareCpuFrame(const Frame& frame);
    void convertCpuFrame(const Frame& frame, AVFrame* target);
    void finishVideo();
    void closeSegment();
    void resetSegment();
    void drainPackets(bool flushing);
    void writeInternal(const Frame& frame);
    void runWorker() noexcept;
    void closeWorker();
    void beginSegment(const Frame& frame);
    std::filesystem::path segmentPath(std::uint64_t generation) const;

    std::filesystem::path basePath;
    std::filesystem::path currentOutputPath;
    std::vector<std::filesystem::path> paths;
    std::ofstream output;
    std::unique_ptr<AVFormatContext, decltype(&freeFormat)> formatContext{nullptr, &freeFormat};
    std::unique_ptr<AVCodecContext, decltype(&freeCodec)> encoderContext{nullptr, &freeCodec};
    std::unique_ptr<AVPacket, decltype(&freePacket)> packet{nullptr, &freePacket};
    std::unique_ptr<SwsContext, decltype(&freeScaler)> scaler{nullptr, &freeScaler};
    Options options;
    Format format = Format::Csv;
    std::optional<std::uint64_t> streamGeneration;
    bool gpuEncoder = false;
    bool initialized = false;
    bool headerWritten = false;
    bool finished = false;
    bool failed = false;
    std::optional<std::int64_t> lastOutputPts;
    std::uint64_t correctedTimestamps = 0;
    std::map<std::int64_t, std::int64_t> pendingFrameDurations;
    bool asynchronous = false;
    bool accepting = false;
    std::unique_ptr<FrameQueue> queue;
    std::thread worker;
    mutable std::mutex lifecycleMutex;
    std::condition_variable lifecycleCondition;
    std::exception_ptr workerException;
    std::exception_ptr admissionError;
    std::optional<std::uint64_t> writtenGeneration;
};
}
