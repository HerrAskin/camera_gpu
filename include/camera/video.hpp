#pragma once

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

struct AVFrame;

namespace camera
{
struct Frame
{
    cv::Mat image;
    // Ровно одно представление заполнено; shared_ptr владеет FFmpeg-ссылкой и переживает Video.
    std::shared_ptr<AVFrame> gpuImage;
    std::optional<std::chrono::microseconds> timestamp;

    int width() const;
    int height() const;
};

class Video
{
public:
    virtual ~Video() = default;
    virtual std::optional<Frame> read() = 0;
    virtual std::string description() const = 0;
};

std::unique_ptr<Video> createMockVideo(std::size_t frameCount = 5);
}
