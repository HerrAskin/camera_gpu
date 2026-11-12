#include "camera/video_capture.hpp"
#include "camera/processor.hpp"
#include "camera/mock_capture.hpp"
#include "camera/video_writer.hpp"

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void requireThrows(Function&& function, const char* message)
{
    try
    {
        function();
    }
    catch (const Exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}

void testVideoCaptureLifecycle()
{
    camera::VideoCapture file;
    camera::VideoCapture::Options options;
    options.preferGpu = false;
    requireThrows<std::logic_error>([&] { file.read(); }, "Read before init must throw");
    require(!file.init("", options), "Invalid input must return false");
    options.queueDepth = 0;
    require(!file.init("", options), "Zero queue depth must be rejected");
    options.queueDepth = 8;
    options.additionalHeldFrames = std::numeric_limits<std::size_t>::max();
    require(!file.init("", options), "Frame reservation overflow must be rejected");
    require(!file.init("", std::chrono::seconds(10), 0), "Legacy overload must reject zero queue depth");
    require(!file.init("", std::chrono::seconds(10), 8, std::numeric_limits<std::size_t>::max()), "Legacy overload must reject reservation overflow");
    require(!file.init(""), "Failed init must release partial state");
    requireThrows<std::logic_error>([&] { file.read(); }, "Read after failed init must throw");
    requireThrows<std::logic_error>([&] { file.description(); }, "Failed init must not leave a ready object");
}

void testMockVideoAndProcessor()
{
    auto video = camera::createMockCapture(3);
    camera::Processor processor;
    camera::Frame retained;
    for (int index = 0; index < 3; ++index)
    {
        auto frame = video->read();
        require(frame && frame->timestamp && frame->timestamp->count() == index * 40000, "Timestamp mismatch");
        require(frame->height() == 2 && frame->width() == 4, "Image size mismatch");
        require(frame->image.at<unsigned char>(0, 0) == 42, "Image data mismatch");
        processor.process(*frame);
        if (index == 0)
        {
            retained = *frame;
        }
    }
    require(!video->read() && !video->read(), "EOF must remain stable");
    video.reset();
    require(retained.image.at<unsigned char>(0, 0) == 42, "Frame lifetime mismatch");
    require(retained.width() == 4 && retained.height() == 2, "Copied CPU frame size mismatch");
    auto empty = camera::createMockCapture(0);
    require(!empty->read(), "Empty video must return EOF");
}

std::filesystem::path testOutputDirectory()
{
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "tmp" / "writer_tests";
    std::filesystem::create_directories(path);
    return path;
}

void testCsvWriter(const std::filesystem::path& testOutput)
{
    const auto csvPath = testOutput / "metadata.csv";
    camera::VideoWriter csvWriter;
    require(!csvWriter.init(testOutput / "missing" / "metadata.csv"), "CSV init must report open failure");
    require(csvWriter.init(csvPath), "CSV writer must be retryable after failed init");
    camera::Frame noTimestamp;
    noTimestamp.image = cv::Mat(2, 4, CV_8UC1, cv::Scalar(0));
    csvWriter.write(noTimestamp);
    csvWriter.finish();
    csvWriter.finish();
    requireThrows<std::logic_error>([&] { csvWriter.write(noTimestamp); }, "Write after finish must fail");

    std::ifstream csvInput(csvPath);
    std::string csvContents((std::istreambuf_iterator<char>(csvInput)), std::istreambuf_iterator<char>());
    require(csvContents == "timestamp_us,width,height\n,4,2\n", "CSV without timestamp must retain empty field");
}

void testMp4RejectsCpuFrames(const std::filesystem::path& testOutput)
{
    camera::VideoWriter mp4Writer;
    require(mp4Writer.init(testOutput / "cpu_input.mp4", camera::VideoWriter::Options{camera::VideoWriter::Format::H264Mp4, 0, false}), "MP4 output init must succeed without opening encoder");
    camera::Frame noTimestamp;
    noTimestamp.image = cv::Mat(2, 4, CV_8UC1, cv::Scalar(0));
    requireThrows<std::runtime_error>([&] { mp4Writer.write(noTimestamp); }, "MP4 writer must reject untimed CPU frames");
    requireThrows<std::logic_error>([&] { mp4Writer.finish(); }, "Failed MP4 writer must not finish a corrupt file");
}

void testGpuVideoCapture(const std::string& path, int expectedCount)
{
    require(expectedCount > 0, "GPU integration frame count must be positive");
    auto gpuVideo = std::make_unique<camera::VideoCapture>();
    camera::VideoCapture::Options options;
    options.ioTimeout = std::chrono::seconds(10);
    options.queueDepth = static_cast<std::size_t>(expectedCount + 2);
    require(gpuVideo->init(path, options), "GPU integration input must initialize");
    camera::Frame gpuRetained;
    camera::Frame gpuRetainedCopy;
    int count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true)
    {
        auto frame = gpuVideo->read();
        if (!frame)
        {
            if (gpuVideo->isFinished())
            {
                break;
            }
            require(std::chrono::steady_clock::now() < deadline, "GPU input did not finish within 10 seconds");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        require(frame->gpuImage && frame->image.empty(), "VideoCapture must return GPU-only frame");
        require(frame->width() == 640 && frame->height() == 360, "GPU frame size mismatch");
        require(frame->timestamp && frame->timestamp->count() == count * 100000, "GPU timestamp mismatch");
        if (count == 0)
        {
            gpuRetained = *frame;
            gpuRetainedCopy = gpuRetained;
        }
        ++count;
    }
    require(count == expectedCount, "GPU frame count mismatch");
    require(!gpuVideo->read() && !gpuVideo->read(), "GPU EOF must remain stable");
    gpuRetained.gpuImage.reset();
    require(gpuRetainedCopy.gpuImage && gpuRetainedCopy.width() == 640 && gpuRetainedCopy.height() == 360, "GPU surface copy must outlive original Frame copy");
    gpuVideo.reset();
    require(gpuRetainedCopy.width() == 640 && gpuRetainedCopy.height() == 360, "GPU surface copy must outlive VideoCapture");
}

void testGpuVideoCaptureStopAndDrop(const std::string& path, int expectedCount)
{
    require(expectedCount > 0, "GPU drop-test frame count must be positive");
    const auto stopStart = std::chrono::steady_clock::now();
    {
        camera::VideoCapture stoppedVideo;
        require(stoppedVideo.init(path), "GPU stop input must initialize");
        stoppedVideo.requestStop();
        // Деструктор обязан закрыть очередь и присоединить worker, даже если кадры ещё не прочитаны.
    }
    require(std::chrono::steady_clock::now() - stopStart < std::chrono::seconds(3), "GPU stop and destruction must finish within 3 seconds");

    camera::VideoCapture droppingVideo;
    camera::VideoCapture::Options options;
    options.ioTimeout = std::chrono::seconds(10);
    options.queueDepth = 1;
    require(droppingVideo.init(path, options), "GPU drop input must initialize");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (droppingVideo.droppedCount() < static_cast<std::size_t>(expectedCount - 1))
    {
        require(std::chrono::steady_clock::now() < deadline, "GPU worker did not fill the bounded queue within 3 seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::optional<camera::Frame> lastFrame;
    while (!lastFrame)
    {
        lastFrame = droppingVideo.read();
        if (!lastFrame)
        {
            require(std::chrono::steady_clock::now() < deadline, "GPU worker did not produce the retained frame within 3 seconds");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const bool retainedLatestFrame = lastFrame && lastFrame->timestamp && lastFrame->timestamp->count() == (expectedCount - 1) * 100000;
    require(retainedLatestFrame, "Drop-oldest queue must retain the newest GPU frame");
}
}

int main(int argc, char* argv[])
{
    try
    {
        testVideoCaptureLifecycle();
        testMockVideoAndProcessor();
        const auto testOutput = testOutputDirectory();
        testCsvWriter(testOutput);
        testMp4RejectsCpuFrames(testOutput);
        if (argc >= 2)
        {
            const int expectedCount = argc >= 3 ? std::stoi(argv[2]) : 10;
            testGpuVideoCapture(argv[1], expectedCount);
            testGpuVideoCaptureStopAndDrop(argv[1], expectedCount);
        }
        std::cout << "Video checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
