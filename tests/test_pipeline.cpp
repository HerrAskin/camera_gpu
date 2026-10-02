#include "camera/file_video.hpp"
#include "camera/processor.hpp"
#include "camera/writer.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
}

int main(int argc, char* argv[])
{
    try
    {
        camera::FileVideo file;
        bool rejectedRead = false;
        try
        {
            file.read();
        }
        catch (const std::logic_error&)
        {
            rejectedRead = true;
        }
        require(rejectedRead, "Read before init must throw");
        require(!file.decodeFirstFrame(), "Decode before init must return false");
        require(!file.init(""), "Invalid input must return false");
        require(!file.decodeFirstFrame(), "Decode after failed init must return false");
        require(!file.init(""), "Failed init must release partial state");
        bool rejectedReadAfterFailedInit = false;
        try
        {
            file.read();
        }
        catch (const std::logic_error&)
        {
            rejectedReadAfterFailedInit = true;
        }
        require(rejectedReadAfterFailedInit, "Read after failed init must throw");
        bool rejectedDescription = false;
        try
        {
            file.description();
        }
        catch (const std::logic_error&)
        {
            rejectedDescription = true;
        }
        require(rejectedDescription, "Failed init must not leave a ready object");
        auto video = camera::createMockVideo(3);
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
        auto empty = camera::createMockVideo(0);
        require(!empty->read(), "Empty video must return EOF");
        const std::filesystem::path testOutput = std::filesystem::path(__FILE__).parent_path().parent_path() / "tmp" / "writer_tests";
        std::filesystem::create_directories(testOutput);
        const auto csvPath = testOutput / "metadata.csv";
        camera::Writer csvWriter;
        require(!csvWriter.init(testOutput / "missing" / "metadata.csv"), "CSV init must report open failure");
        require(csvWriter.init(csvPath), "CSV writer must be retryable after failed init");
        camera::Frame noTimestamp;
        noTimestamp.image = cv::Mat(2, 4, CV_8UC1, cv::Scalar(0));
        csvWriter.write(noTimestamp);
        csvWriter.finish();
        csvWriter.finish();
        bool rejectedWriteAfterFinish = false;
        try
        {
            csvWriter.write(noTimestamp);
        }
        catch (const std::logic_error&)
        {
            rejectedWriteAfterFinish = true;
        }
        require(rejectedWriteAfterFinish, "Write after finish must fail");
        std::ifstream csvInput(csvPath);
        std::string csvContents((std::istreambuf_iterator<char>(csvInput)), std::istreambuf_iterator<char>());
        require(csvContents == "timestamp_us,width,height\n,4,2\n", "CSV without timestamp must retain empty field");

        camera::Writer mp4Writer;
        require(mp4Writer.init(testOutput / "cpu_input.mp4", camera::Writer::Format::H264Mp4), "MP4 output init must succeed without opening encoder");
        bool rejectedCpuMp4 = false;
        try
        {
            mp4Writer.write(noTimestamp);
        }
        catch (const std::runtime_error&)
        {
            rejectedCpuMp4 = true;
        }
        require(rejectedCpuMp4, "MP4 writer must reject CPU frames");
        bool rejectedFinishAfterFailure = false;
        try
        {
            mp4Writer.finish();
        }
        catch (const std::logic_error&)
        {
            rejectedFinishAfterFailure = true;
        }
        require(rejectedFinishAfterFailure, "Failed MP4 writer must not finish a corrupt file");
        if (argc >= 2)
        {
            auto gpuVideo = std::make_unique<camera::FileVideo>();
            require(gpuVideo->init(argv[1]), "GPU integration input must initialize");
            const int expectedCount = argc >= 3 ? std::stoi(argv[2]) : 10;
            camera::Frame gpuRetained;
            camera::Frame gpuRetainedCopy;
            int count = 0;
            while (auto frame = gpuVideo->read())
            {
                require(frame->gpuImage && frame->image.empty(), "FileVideo must return GPU-only frame");
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
            require(gpuRetainedCopy.width() == 640 && gpuRetainedCopy.height() == 360, "GPU surface copy must outlive FileVideo");
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
