#include "camera/processor.hpp"
#include "camera/writer.hpp"

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc > 2)
    {
        std::cerr << "Использование: camera_demo [output.csv]\n";
        return 1;
    }
    try
    {
        auto video = camera::createMockVideo();
        camera::Processor processor;
        camera::Writer writer(argc == 2 ? argv[1] : "mock_frames.csv");

        while (auto frame = video->read())
        {
            processor.process(*frame);
            writer.write(*frame);
        }
        writer.finish();
        std::cout << "Mock-видео сохранено: 5 кадров, CPU\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Ошибка: " << error.what() << '\n';
        return 1;
    }
}
