#include "camera/file_video.hpp"
#include "camera/processor.hpp"
#include "camera/video.hpp"
#include "camera/writer.hpp"

#include <iostream>
#include <filesystem>
#include <string>

int main(int argc, char* argv[])
{
    const bool inspect = argc > 1 && std::string(argv[1]) == "--inspect";
    const bool decodeFirst = argc > 1 && std::string(argv[1]) == "--decode-first";
    const bool decode = argc > 1 && std::string(argv[1]) == "--decode";
    const bool transcode = argc > 1 && std::string(argv[1]) == "--transcode";
    if ((inspect && argc != 3) || (decodeFirst && argc != 3) || ((decode || transcode) && argc != 4) || (!inspect && !decodeFirst && !decode && !transcode && argc > 2))
    {
        std::cerr << "[main] Использование: camera_demo [output.csv]\n"
                  << "[main]                camera_demo --inspect <video-file>\n";
        std::cerr << "[main]                camera_demo --decode-first <video-file>\n";
        std::cerr << "[main]                camera_demo --decode <video-file> <output.csv>\n";
        std::cerr << "[main]                camera_demo --transcode <video-file> <output.mp4>\n";
        return 1;
    }
    try
    {
        std::unique_ptr<camera::Video> video;
        std::string outputPath;
        bool gpuInput = false;
        if (inspect || decodeFirst || decode || transcode)
        {
            if ((decode || transcode) && std::filesystem::exists(argv[3]))
            {
                std::error_code equivalenceError;
                const bool sameFile = std::filesystem::equivalent(argv[2], argv[3], equivalenceError);
                if (equivalenceError)
                {
                    std::cerr << "[main] Не удалось проверить, совпадают ли входной и выходной файлы: " << equivalenceError.message() << '\n';
                    return 1;
                }
                if (sameFile)
                {
                    std::cerr << "[main] Входной и выходной файлы совпадают\n";
                    return 1;
                }
            }
            auto fileVideo = std::make_unique<camera::FileVideo>();
            if (!fileVideo->init(argv[2]))
            {
                return 1;
            }
            if (inspect)
            {
                std::cout << fileVideo->description() << '\n';
                return 0;
            }
            if (decodeFirst)
            {
                return fileVideo->decodeFirstFrame() ? 0 : 1;
            }
            video = std::move(fileVideo);
            outputPath = argv[3];
            gpuInput = true;
        }
        else
        {
            video = camera::createMockVideo();
            outputPath = argc == 2 ? argv[1] : "mock_frames.csv";
        }
        camera::Processor processor;
        camera::Writer writer;
        const auto outputFormat = transcode ? camera::Writer::Format::H264Mp4 : camera::Writer::Format::Csv;
        if (!writer.init(outputPath, outputFormat))
        {
            return 1;
        }

        std::size_t frameCount = 0;
        while (auto frame = video->read())
        {
            processor.process(*frame);
            writer.write(*frame);
            ++frameCount;
        }
        writer.finish();
        std::cout << "[main] Обработано: " << frameCount << " кадров, "
                  << (gpuInput ? "CUDA/GPU" : "CPU")
                  << (transcode ? "; H264 MP4 сохранён: " : "; метаданные сохранены: ") << outputPath << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[main] Ошибка: " << error.what() << '\n';
        return 1;
    }
}
