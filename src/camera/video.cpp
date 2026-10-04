#include "camera/video.hpp"

#include <opencv2/core.hpp>

extern "C"
{
#include <libavutil/frame.h>
}

namespace camera
{
int Frame::width() const
{
    return gpuImage ? gpuImage->width : image.cols;
}

int Frame::height() const
{
    return gpuImage ? gpuImage->height : image.rows;
}

namespace
{
class MockVideo final : public Video
{
public:
    explicit MockVideo(std::size_t frameCount) : frameCount(frameCount) {}

    std::string description() const override
    {
        return "Mock-видео: 4x2, Gray8, 25 FPS, CPU";
    }

    std::optional<Frame> read() override
    {
        if (nextFrame == frameCount)
        {
            return std::nullopt;
        }
        Frame frame;
        frame.image = cv::Mat(2, 4, CV_8UC1, cv::Scalar(42));
        frame.timestamp = std::chrono::microseconds(nextFrame * 40000);
        ++nextFrame;
        return frame;
    }

    bool isFinished() const override
    {
        return nextFrame == frameCount;
    }

private:
    std::size_t frameCount;
    std::size_t nextFrame = 0;
};
}

std::unique_ptr<Video> createMockVideo(std::size_t frameCount)
{
    return std::make_unique<MockVideo>(frameCount);
}

}
