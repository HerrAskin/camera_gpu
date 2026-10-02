#include "camera/video.hpp"

#include <opencv2/core.hpp>

namespace camera
{
namespace
{
class MockVideo final : public Video
{
public:
    explicit MockVideo(std::size_t frameCount) : frameCount(frameCount) {}

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
