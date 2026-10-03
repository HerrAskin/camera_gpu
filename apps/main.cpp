#include "camera/file_video.hpp"
#include "camera/processor.hpp"
#include "camera/video.hpp"
#include "camera/writer.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace
{
volatile std::sig_atomic_t interrupted = 0;

// В обработчике сигнала безопасно только отметить прерывание; вывод и очистку выполняет main.
void handleInterrupt(int signal)
{
    if (signal == SIGINT)
    {
        interrupted = 1;
    }
}

enum class Mode
{
    Mock,
    Inspect,
    DecodeFirst,
    Decode,
    Transcode
};

struct Command
{
    Mode mode = Mode::Mock;
    std::string inputPath;
    std::string outputPath = "mock_frames.csv";
};

void printUsage()
{
    std::cerr << "[main] Использование: camera_demo [output.csv]\n"
              << "[main]                camera_demo --inspect <video-file>\n"
              << "[main]                camera_demo --decode-first <video-file>\n"
              << "[main]                camera_demo --decode <video-file> <output.csv>\n"
              << "[main]                camera_demo --transcode <video-file> <output.mp4>\n";
}

bool parseCommand(int argc, char* argv[], Command& command)
{
    if (argc == 1)
    {
        return true;
    }
    const std::string option = argv[1];
    if (option == "--inspect" && argc == 3)
    {
        command.mode = Mode::Inspect;
    }
    else if (option == "--decode-first" && argc == 3)
    {
        command.mode = Mode::DecodeFirst;
    }
    else if (option == "--decode" && argc == 4)
    {
        command.mode = Mode::Decode;
    }
    else if (option == "--transcode" && argc == 4)
    {
        command.mode = Mode::Transcode;
    }
    else if (argc == 2)
    {
        command.outputPath = option;
        return true;
    }
    else
    {
        return false;
    }

    command.inputPath = argv[2];
    if (command.mode == Mode::Decode || command.mode == Mode::Transcode)
    {
        command.outputPath = argv[3];
    }
    return true;
}

bool validateInputOutputPaths(const Command& command)
{
    if (command.mode != Mode::Decode && command.mode != Mode::Transcode)
    {
        return true;
    }
    if (!std::filesystem::exists(command.outputPath))
    {
        return true;
    }
    std::error_code equivalenceError;
    const bool sameFile = std::filesystem::equivalent(command.inputPath, command.outputPath, equivalenceError);
    if (equivalenceError)
    {
        std::cerr << "[main] Не удалось проверить, совпадают ли входной и выходной файлы: " << equivalenceError.message() << '\n';
        return false;
    }
    if (sameFile)
    {
        std::cerr << "[main] Входной и выходной файлы совпадают\n";
        return false;
    }
    return true;
}

bool printFirstFrame(camera::Video& video)
{
    try
    {
        auto frame = video.read();
        if (!frame)
        {
            std::cerr << "[main] Видеопоток не содержит декодируемых кадров\n";
            return false;
        }
        std::cout << "[main] Размер: " << frame->width() << 'x' << frame->height()
                  << ", формат: CUDA/GPU, timestamp_us: ";
        if (frame->timestamp)
        {
            std::cout << frame->timestamp->count();
        }
        else
        {
            std::cout << "не задан";
        }
        std::cout << '\n';
        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[main] " << error.what() << '\n';
        return false;
    }
}
}

int main(int argc, char* argv[])
{
    if (std::signal(SIGINT, handleInterrupt) == SIG_ERR)
    {
        std::cerr << "[main] Не удалось зарегистрировать обработчик SIGINT\n";
        return 1;
    }

    Command command;
    if (!parseCommand(argc, argv, command))
    {
        printUsage();
        return 1;
    }
    try
    {
        if (!validateInputOutputPaths(command))
        {
            return 1;
        }
        std::unique_ptr<camera::Video> video;
        const bool fileInput = command.mode != Mode::Mock;
        const bool gpuInput = command.mode == Mode::Decode || command.mode == Mode::Transcode;
        const bool transcode = command.mode == Mode::Transcode;
        const bool rtspInput = command.inputPath.rfind("rtsp://", 0) == 0;
        if (fileInput)
        {
            auto fileVideo = std::make_unique<camera::FileVideo>();
            if (!fileVideo->init(command.inputPath))
            {
                return 1;
            }
            if (command.mode == Mode::Inspect)
            {
                std::cout << fileVideo->description() << '\n';
                return 0;
            }
            if (command.mode == Mode::DecodeFirst)
            {
                return printFirstFrame(*fileVideo) ? 0 : 1;
            }
            video = std::move(fileVideo);
        }
        else
        {
            video = camera::createMockVideo();
        }

        camera::Processor processor;
        camera::Writer writer;
        const auto outputFormat = transcode ? camera::Writer::Format::H264Mp4 : camera::Writer::Format::Csv;
        if (!writer.init(command.outputPath, outputFormat))
        {
            return 1;
        }

        std::size_t frameCount = 0;
        bool readFailed = false;
        while (interrupted == 0)
        {
            decltype(video->read()) frame;
            try
            {
                frame = video->read();
            }
            catch (const std::exception& error)
            {
                std::cerr << "[main] Ошибка чтения: " << error.what() << '\n';
                readFailed = true;
                break;
            }
            if (!frame)
            {
                break;
            }
            if (transcode && rtspInput && frameCount == 0 && !frame->timestamp)
            {
                std::cout << "[main] Пропущен начальный кадр RTSP без timestamp\n";
                continue;
            }
            processor.process(*frame);
            writer.write(*frame);
            ++frameCount;
        }
        if (transcode && frameCount == 0 && readFailed)
        {
            std::cout << "[main] Кадры не получены; MP4 не сформирован\n";
            return 1;
        }
        if (interrupted != 0)
        {
            std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
            if (transcode && frameCount == 0)
            {
                std::cout << "[main] Кадры не получены; MP4 не сформирован\n";
                return 0;
            }
        }
        writer.finish();
        std::cout << "[main] Обработано: " << frameCount << " кадров, "
                  << (gpuInput ? "CUDA/GPU" : "CPU")
                  << (transcode ? "; H264 MP4 сохранён: " : "; метаданные сохранены: ") << command.outputPath << '\n';
        return readFailed ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[main] Ошибка: " << error.what() << '\n';
        return 1;
    }
}
