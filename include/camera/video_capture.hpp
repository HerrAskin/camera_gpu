#pragma once

#include "camera/frame.hpp"
#include "camera/frame_queue.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C"
{
#include <libavutil/rational.h>
#include <libavutil/pixfmt.h>
}

struct AVFormatContext;
struct AVBufferRef;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct AVCodec;
struct SwsContext;

namespace camera
{
class VideoCapture
{
public:
    struct Options
    {
        std::chrono::milliseconds ioTimeout{std::chrono::seconds(10)};
        std::size_t queueDepth = 8;
        std::size_t additionalHeldFrames = 0;
        unsigned reconnectAttempts = 0;
        bool preferGpu = true;
        // Файловый benchmark/архив: ждать свободного места вместо удаления старых кадров.
        bool keepAllFrames = false;
    };

    VideoCapture() = default;
    virtual ~VideoCapture();
    /** Запускает чтение; при RTSP-сбое начальное подключение может продолжиться в worker. */
    bool init(const std::string& source, const Options& options);
    bool init(const std::string& source, std::chrono::milliseconds ioTimeout = std::chrono::seconds(10), std::size_t queueDepth = 8, std::size_t additionalHeldFrames = 0);
    /** Запрашивает остановку I/O и отменяет ожидание перед reconnect. */
    virtual void requestStop() noexcept;
    /** Извлекает готовый кадр без ожидания; isFinished различает пустую очередь и конец. */
    virtual std::optional<Frame> read();
    virtual bool isFinished() const;
    std::size_t droppedCount() const;
    /** Возвращает кэшированное описание последнего успешно настроенного потока. */
    virtual std::string description() const;

private:
    static int interruptIo(void* opaque) noexcept;
    static void closeInput(AVFormatContext* context);
    static void releaseDevice(AVBufferRef* device);
    static void closeDecoder(AVCodecContext* decoder);
    static void freeScaler(SwsContext* scaler);
    static AVPixelFormat selectFormat(AVCodecContext* context, const AVPixelFormat* formats);
    void beginIo() noexcept;
    void openInput();
    void openDecoder();
    bool hasCudaDecoder(const AVCodec* codec) const noexcept;
    void configureDecoder(const AVCodec* codec, bool useGpu);
    bool validateOptions(const Options& value) const noexcept;
    bool startReader();
    void prepareAttempt(unsigned generation);
    void resetAttempt() noexcept;
    void readerLoop() noexcept;
    void readWithRetry();
    bool readConnectedFrames();
    bool enqueueFrame(Frame frame);
    bool reconnectAllowed(unsigned retries) const noexcept;
    bool reconnect(unsigned generation) noexcept;
    bool waitReconnect(unsigned seconds) noexcept;
    void feedDecoder(AVPacket* packet);
    std::optional<Frame> decodeFrame();
    Frame makeFrame(std::shared_ptr<AVFrame> frame);
    Frame makeCpuFrame(std::shared_ptr<AVFrame> frame);
    void storeError(std::exception_ptr error) noexcept;

    std::unique_ptr<AVFormatContext, decltype(&closeInput)> formatContext{nullptr, closeInput};
    std::unique_ptr<AVBufferRef, decltype(&releaseDevice)> cudaDevice{nullptr, releaseDevice};
    std::unique_ptr<AVCodecContext, decltype(&closeDecoder)> decoderContext{nullptr, closeDecoder};
    std::unique_ptr<SwsContext, decltype(&freeScaler)> scaler{nullptr, freeScaler};
    std::atomic<bool> stopRequested{false};
    std::unique_ptr<FrameQueue> frameQueue;
    std::thread readerThread;
    mutable std::mutex readerErrorMutex;
    std::exception_ptr readerError;
    std::string source;
    std::string cachedDescription;
    Options options{};
    bool initialized = false;
    bool liveInput = false;
    bool gpuActive = false;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    int streamIndex = -1;
    unsigned generation = 0;
    bool draining = false;
    bool ended = false;
    AVRational frameRate{0, 1};
};
}
