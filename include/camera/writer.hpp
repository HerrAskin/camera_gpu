#pragma once

#include "camera/video.hpp"

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace camera
{
/** Записывает CSV либо CUDA-кадры в H.264 MP4 через NVENC.
 * Методы вызываются последовательно; MP4 фиксирует размер и hw_frames_ctx по первому кадру.
 * После ошибки write() или finish() запись продолжать нельзя.
 */
class Writer
{
public:
    enum class Format
    {
        Csv,
        H264Mp4
    };

    Writer() = default;
    /** Освобождает ресурсы; для корректного MP4 перед уничтожением нужен явный finish(). */
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;

    /** Открывает выход; false и сообщение в логе означают ошибку или повторную инициализацию. */
    bool init(const std::filesystem::path& path, Format format = Format::Csv);
    /** Добавляет кадр; для MP4 нужны CUDA, timestamp, duration, размеры и исходный time_base. */
    void write(const Frame& frame);
    /** Завершает запись и выпускает trailer; повторный вызов после успеха безопасен, ошибки бросаются. */
    void finish();

private:
    static void freeFormat(AVFormatContext* context);
    static void freeCodec(AVCodecContext* context);
    static void freePacket(AVPacket* packet);
    static void freeFrame(AVFrame* frame);
    void validateVideoFrame(const Frame& frame);
    void openEncoder(const Frame& frame);
    void writeHeader();
    void writeCsv(const Frame& frame);
    void writeVideo(const Frame& frame);
    std::unique_ptr<AVFrame, decltype(&freeFrame)> prepareEncoderFrame(const Frame& frame);
    void finishVideo();
    void drainPackets(bool flushing);
    void fail(const std::string& message, const char* operation = "Writer::write");

    std::ofstream output;
    // RAII-владельцы muxer, encoder и временного packet; deleter-ы реализуют FFmpeg cleanup.
    std::unique_ptr<AVFormatContext, decltype(&freeFormat)> formatContext{nullptr, &freeFormat};
    std::unique_ptr<AVCodecContext, decltype(&freeCodec)> encoderContext{nullptr, &freeCodec};
    std::unique_ptr<AVPacket, decltype(&freePacket)> packet{nullptr, &freePacket};
    Format format = Format::Csv;
    // Различают успешное открытие, готовность MP4 header, закрытие и терминальную ошибку.
    bool initialized = false;
    bool headerWritten = false;
    bool finished = false;
    bool failed = false;
    std::optional<std::int64_t> lastOutputPts;
    std::uint64_t correctedTimestamps = 0;
    // NVENC может вернуть packet duration по своему nominal FPS; сохраняем исходную duration по PTS.
    std::map<std::int64_t, std::int64_t> pendingFrameDurations;
};
}
