#include "camera/mock_capture.hpp"
#include <opencv2/core.hpp>

namespace camera
{
namespace
{
class MockCapture final : public VideoCapture
{
public:
    explicit MockCapture(std::size_t frameCount) : frameCount(frameCount)
    {
    }

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
        frame.duration = std::chrono::microseconds(40000);
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

std::unique_ptr<VideoCapture> createMockCapture(std::size_t frameCount)
{
    return std::make_unique<MockCapture>(frameCount);
}

}
