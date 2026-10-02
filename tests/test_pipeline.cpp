#include "camera/processor.hpp"

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

int main()
{
    try
    {
        auto video = camera::createMockVideo(3);
        camera::Processor processor;
        camera::Frame retained;
        for (int index = 0; index < 3; ++index)
        {
            auto frame = video->read();
            require(frame && frame->timestamp.count() == index * 40000, "Timestamp mismatch");
            require(frame->image.rows == 2 && frame->image.cols == 4, "Image size mismatch");
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
        auto empty = camera::createMockVideo(0);
        require(!empty->read(), "Empty video must return EOF");
        std::cout << "Video checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
