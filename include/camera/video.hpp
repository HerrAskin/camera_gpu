#pragma once

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

namespace camera
{
struct Frame
{
    cv::Mat image;
    std::chrono::microseconds timestamp{0};
};

class Video
{
public:
    virtual ~Video() = default;
    virtual std::optional<Frame> read() = 0;
};

std::unique_ptr<Video> createMockVideo(std::size_t frameCount = 5);
}
