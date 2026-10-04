#pragma once

#include "camera/video.hpp"
#include "camera/frame_queue.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>

struct AVFormatContext;
struct AVBufferRef;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace camera
{
/** Декодирует видеопоток через CUDA в worker-потоке и передаёт кадры через bounded очередь.
 * init/read/description/isFinished принадлежат потоку-потребителю; requestStop можно
 * вызывать параллельно. Объект должен жить до завершения всех этих вызовов.
 */
class FileVideo final : public Video
{
public:
    FileVideo() = default;
    ~FileVideo() override;
    /** Настраивает источник; после неудачи можно повторить, успешная повторная инициализация запрещена.
     * При ошибке возвращает false и пишет сообщение в лог.
     */
    // additionalHeldFrames резервирует GPU-буферы для кадров, удерживаемых последующими этапами.
    bool init(const std::string& path, std::chrono::milliseconds ioTimeout = std::chrono::seconds(10), std::size_t queueDepth = 8, std::size_t additionalHeldFrames = 0);
    /** Запрашивает кооперативную остановку из другого потока; объект должен жить до выхода init/read.
     * Не предназначен для вызова из обработчика сигнала.
     */
    void requestStop() noexcept override;
    /** Немедленно извлекает готовый кадр; пустая очередь даёт nullopt, ошибка worker-потока
     * повторно бросается при каждом опросе после извлечения уже накопленных кадров.
     */
    std::optional<Frame> read() override;
    /** Источник завершён, когда worker закрыл очередь и она опустела. */
    bool isFinished() const override;
    /** Число кадров, отброшенных при переполнении очереди. Доступно после init. */
    std::size_t droppedCount() const;
    /** Описание доступно после успешного init; иначе бросает исключение. */
    std::string description() const override;

private:
    static int interruptIo(void* opaque) noexcept;
    void beginIo() noexcept;
    static void closeInput(AVFormatContext* context);
    static void releaseDevice(AVBufferRef* device);
    static void closeDecoder(AVCodecContext* decoder);
    void openInput(const std::string& path);
    void createCudaDevice();
    void openDecoder(std::size_t queueDepth);
    void feedDecoder(AVPacket* packet);
    Frame makeFrame(std::shared_ptr<AVFrame> frame) const;
    std::optional<Frame> decodeFrame();
    void readerLoop() noexcept;
    void joinReader() noexcept;

    std::unique_ptr<AVFormatContext, decltype(&closeInput)> formatContext{nullptr, closeInput};
    std::unique_ptr<AVBufferRef, decltype(&releaseDevice)> cudaDevice{nullptr, releaseDevice};
    std::unique_ptr<AVCodecContext, decltype(&closeDecoder)> decoderContext{nullptr, closeDecoder};
    std::atomic<bool> stopRequested{false};
    std::unique_ptr<FrameQueue> frameQueue;
    std::thread readerThread;
    mutable std::mutex readerErrorMutex;
    std::exception_ptr readerError;
    bool initialized = false;
    // RTSP EOF означает потерю live-соединения; вызывающий код может перезапустить источник.
    bool liveInput = false;
    // До запуска worker дедлайн задаёт init(); затем только worker в decodeFrame().
    // Callback вызывается FFmpeg в том же потоке, атомарным нужен только stopRequested.
    std::chrono::milliseconds ioTimeout{std::chrono::seconds(10)};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    int streamIndex = -1;
    // draining: EOF уже отправлен decoder, теперь читаются задержанные кадры.
    bool draining = false;
    // ended: decoder сообщил EOF, последующие decodeFrame() сразу возвращают nullopt.
    bool ended = false;
};
}
