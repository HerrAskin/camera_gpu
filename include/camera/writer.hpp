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
struct AVBufferRef;

namespace camera
{
class Writer
{
public:
    enum class Format
    {
        Csv,
        H264Mp4
    };

    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;

    bool init(const std::filesystem::path& path, Format format = Format::Csv);
    void write(const Frame& frame);
    void finish();

private:
    static void freeFormat(AVFormatContext* context);
    static void freeCodec(AVCodecContext* context);
    static void freePacket(AVPacket* packet);
    void initializeVideo(const Frame& frame);
    void drainPackets(bool flushing);
    void fail(const std::string& message, const char* operation = "Writer::write");

    std::ofstream output;
    // RAII-владельцы muxer, encoder и временного packet; deleter-ы реализуют FFmpeg cleanup.
    std::unique_ptr<AVFormatContext, decltype(&freeFormat)> formatContext{nullptr, &freeFormat};
    std::unique_ptr<AVCodecContext, decltype(&freeCodec)> codecContext{nullptr, &freeCodec};
    std::unique_ptr<AVPacket, decltype(&freePacket)> packet{nullptr, &freePacket};
    Format format = Format::Csv;
    // Различают успешное открытие, готовность MP4 header, закрытие и терминальную ошибку.
    bool initialized = false;
    bool headerWritten = false;
    bool finished = false;
    bool failed = false;
    std::optional<std::int64_t> lastPts;
    // NVENC может вернуть packet duration по своему nominal FPS; сохраняем исходную duration по PTS.
    std::map<std::int64_t, std::int64_t> pendingDurations;
};
}
