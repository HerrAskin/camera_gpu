#pragma once

#include "camera/video.hpp"

#include <atomic>
#include <chrono>

struct AVFormatContext;
struct AVBufferRef;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace camera
{
/** Читает видеопоток через CUDA decoder; методы init/read/description вызываются одним потоком. */
class FileVideo final : public Video
{
public:
    FileVideo() = default;
    /** Настраивает источник; после неудачи можно повторить, успешная повторная инициализация запрещена.
     * При ошибке возвращает false и пишет сообщение в лог.
     */
    bool init(const std::string& path, std::chrono::milliseconds ioTimeout = std::chrono::seconds(10));
    /** Запрашивает кооперативную остановку из другого потока; объект должен жить до выхода init/read.
     * Не предназначен для вызова из обработчика сигнала.
     */
    void requestStop() noexcept;
    /** Возвращает GPU-кадр или nullopt на EOF; ошибки, таймаут и остановка приводят к исключению.
     * ioTimeout задаёт кооперативный срок ожидания одного вызова, а не принудительную отмену GPU-операций.
     */
    std::optional<Frame> read() override;
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
    void openDecoder();
    void feedDecoder(AVPacket* packet);
    Frame makeFrame(std::shared_ptr<AVFrame> frame) const;

    std::unique_ptr<AVFormatContext, decltype(&closeInput)> formatContext{nullptr, closeInput};
    std::unique_ptr<AVBufferRef, decltype(&releaseDevice)> cudaDevice{nullptr, releaseDevice};
    std::unique_ptr<AVCodecContext, decltype(&closeDecoder)> decoderContext{nullptr, closeDecoder};
    std::atomic<bool> stopRequested{false};
    // Дедлайн меняет только поток, выполняющий init()/read(); callback вызывается FFmpeg внутри этого потока.
    std::chrono::milliseconds ioTimeout{std::chrono::seconds(10)};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    int streamIndex = -1;
    // draining: EOF уже отправлен decoder, теперь читаются задержанные кадры.
    bool draining = false;
    // ended: decoder сообщил EOF, последующие read() сразу возвращают nullopt.
    bool ended = false;
};
}
