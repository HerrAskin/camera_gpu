#pragma once

#include "camera/video.hpp"

struct AVFormatContext;
struct AVBufferRef;
struct AVCodecContext;

namespace camera
{
class FileVideo final : public Video
{
public:
    FileVideo() = default;
    bool init(const std::string& path);
    bool decodeFirstFrame();
    std::optional<Frame> read() override;
    std::string description() const override;

private:
    static void closeInput(AVFormatContext* context);
    static void releaseDevice(AVBufferRef* device);
    static void closeDecoder(AVCodecContext* decoder);

    std::unique_ptr<AVFormatContext, decltype(&closeInput)> context{nullptr, closeInput};
    std::unique_ptr<AVBufferRef, decltype(&releaseDevice)> device{nullptr, releaseDevice};
    std::unique_ptr<AVCodecContext, decltype(&closeDecoder)> decoder{nullptr, closeDecoder};
    int streamIndex = -1;
    // draining: EOF уже отправлен decoder, теперь читаются задержанные кадры.
    bool draining = false;
    // ended: decoder сообщил EOF, последующие read() сразу возвращают nullopt.
    bool ended = false;
};
}
