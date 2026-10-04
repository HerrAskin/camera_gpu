#include "camera/file_video.hpp"
#include "camera/processor.hpp"
#include "camera/video.hpp"
#include "camera/writer.hpp"

#include <csignal>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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
    std::size_t queueDepth = 8;
    std::size_t writeQueueDepth = 8;
    unsigned reconnectAttempts = 0;
};

void printUsage()
{
    std::cerr << "[main] Использование: camera_demo [output.csv]\n"
              << "[main]                camera_demo --inspect <video-file>\n"
              << "[main]                camera_demo --decode-first <video-file>\n"
              << "[main]                camera_demo --decode <video-file> <output.csv>\n"
              << "[main]                camera_demo --transcode <video-file> <output.mp4>\n"
              << "[main] Перед режимом можно задать --queue-depth <N> (по умолчанию 8)\n"
              << "[main] --write-queue-depth <N>: очередь записи (по умолчанию 8, 0 — синхронно)\n"
              << "[main] --reconnect <N>: дополнительные попытки RTSP-записи (по умолчанию 0)\n";
}

bool parseCommand(int argc, char* argv[], Command& command)
{
    while (argc > 1 && (std::string(argv[1]) == "--queue-depth" || std::string(argv[1]) == "--write-queue-depth" || std::string(argv[1]) == "--reconnect"))
    {
        if (argc < 3)
        {
            return false;
        }
        const bool queueOption = std::string(argv[1]) == "--queue-depth";
        const bool writeQueueOption = std::string(argv[1]) == "--write-queue-depth";
        const std::string value = argv[2];
        unsigned parsedValue = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsedValue);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (queueOption && parsedValue == 0))
        {
            return false;
        }
        if (queueOption)
        {
            command.queueDepth = parsedValue;
        }
        else if (writeQueueOption)
        {
            command.writeQueueDepth = parsedValue;
        }
        else
        {
            command.reconnectAttempts = parsedValue;
        }
        argc -= 2;
        argv += 2;
    }
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
    // RTSP URL не является локальным файлом: equivalent() для него даёт ошибку.
    if (command.inputPath.rfind("rtsp://", 0) == 0)
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
        std::optional<camera::Frame> frame;
        while (interrupted == 0)
        {
            frame = video.read();
            if (!frame && video.isFinished())
            {
                // Повторный опрос забирает последний кадр или ошибку, появившиеся между проверками.
                frame = video.read();
                break;
            }
            if (frame)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        video.requestStop();
        if (interrupted != 0)
        {
            std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
            return true;
        }
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

enum class SessionResult
{
    Success,
    SourceFailed, // Открытие/чтение источника можно повторить по политике main.
    Failed        // Ошибка вывода или другого этапа не запускает переподключение.
};

// Каждый вызов владеет своей парой Video/Writer: их GPU-контексты не смешиваются между подключениями.
SessionResult runSession(const Command& command)
{
    std::unique_ptr<camera::Video> video;
    const bool fileInput = command.mode != Mode::Mock;
    const bool gpuInput = command.mode == Mode::Decode || command.mode == Mode::Transcode;
    const bool transcode = command.mode == Mode::Transcode;
    const bool rtspInput = command.inputPath.rfind("rtsp://", 0) == 0;
    if (fileInput)
    {
        auto fileVideo = std::make_unique<camera::FileVideo>();
        const auto heldByWriter = gpuInput && command.writeQueueDepth > 0 ? command.writeQueueDepth + 2 : 0;
        if (!fileVideo->init(command.inputPath, std::chrono::seconds(10), command.queueDepth, heldByWriter))
        {
            return SessionResult::SourceFailed;
        }
        if (command.mode == Mode::Inspect)
        {
            std::cout << fileVideo->description() << '\n';
            return SessionResult::Success;
        }
        if (command.mode == Mode::DecodeFirst)
        {
            return printFirstFrame(*fileVideo) ? SessionResult::Success : SessionResult::Failed;
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
    if (!writer.init(command.outputPath, outputFormat, command.writeQueueDepth))
    {
        return SessionResult::Failed;
    }

    std::size_t frameCount = 0;
    bool readFailed = false;
    while (interrupted == 0)
    {
        decltype(video->read()) frame;
        bool sourceFinished = false;
        try
        {
            frame = video->read();
            if (!frame && video->isFinished())
            {
                sourceFinished = true;
                frame = video->read();
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "[main] Ошибка чтения: " << error.what() << '\n';
            readFailed = true;
            break;
        }
        if (!frame)
        {
            if (sourceFinished)
            {
                break;
            }
            // Пустая очередь не означает EOF; пауза предотвращает активное ожидание.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (transcode && rtspInput && frameCount == 0 && !frame->timestamp)
        {
            std::cout << "[main] Пропущен начальный кадр RTSP без timestamp\n";
            continue;
        }
        processor.process(*frame);
        try
        {
            writer.write(*frame);
        }
        catch (...)
        {
            video->requestStop();
            // При перегрузке дописываем уже принятые кадры; исходная ошибка всё равно остановит main.
            try
            {
                writer.finish();
            }
            catch (const std::exception& error)
            {
                std::cerr << "[main] Завершение записи после ошибки: " << error.what() << '\n';
            }
            throw;
        }
        ++frameCount;
    }
    video->requestStop();
    if (transcode && frameCount == 0 && readFailed)
    {
        std::cout << "[main] Кадры не получены; MP4 не сформирован\n";
        return SessionResult::SourceFailed;
    }
    if (interrupted != 0)
    {
        std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
        if (transcode && frameCount == 0)
        {
            std::cout << "[main] Кадры не получены; MP4 не сформирован\n";
            return SessionResult::Success;
        }
    }
    writer.finish();
    std::cout << "[main] Обработано: " << frameCount << " кадров, "
              << (gpuInput ? "CUDA/GPU" : "CPU")
              << (transcode ? "; H264 MP4 сохранён: " : "; метаданные сохранены: ") << command.outputPath << '\n';
    return readFailed ? SessionResult::SourceFailed : SessionResult::Success;
}

bool waitBeforeReconnect(unsigned attempt)
{
    const unsigned seconds = attempt == 1 ? 1 : attempt == 2 ? 2 : attempt == 3 ? 4 : 5;
    std::cout << "[main] Переподключение: попытка " << attempt << ", пауза " << seconds << " с\n" << std::flush;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (interrupted == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return interrupted == 0;
}

std::string nextSessionPath(const std::string& original, std::size_t& sessionNumber)
{
    const std::filesystem::path path(original);
    std::filesystem::path candidate;
    do
    {
        ++sessionNumber;
        candidate = path.parent_path() / (path.stem().string() + "_session" + std::to_string(sessionNumber) + path.extension().string());
    }
    while (std::filesystem::exists(candidate));
    return candidate.string();
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
        const bool rtspRecording = command.mode == Mode::Transcode && command.inputPath.rfind("rtsp://", 0) == 0;
        if (command.reconnectAttempts != 0 && !rtspRecording)
        {
            std::cerr << "[main] --reconnect доступен только для --transcode RTSP\n";
            return 1;
        }
        if (!validateInputOutputPaths(command))
        {
            return 1;
        }
        const std::string originalOutput = command.outputPath;
        std::size_t sessionNumber = 1;
        unsigned retries = 0;
        while (true)
        {
            const auto result = runSession(command);
            if (interrupted != 0)
            {
                if (result != SessionResult::Success)
                {
                    std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
                }
                return 0;
            }
            if (result == SessionResult::Success)
            {
                return 0;
            }
            if (result == SessionResult::Failed || !rtspRecording || retries == command.reconnectAttempts)
            {
                if (rtspRecording && command.reconnectAttempts != 0 && result == SessionResult::SourceFailed)
                {
                    std::cerr << "[main] Попытки переподключения исчерпаны\n";
                }
                return 1;
            }
            // runSession уже завершил Writer и уничтожил Video (с join); можно создавать новую сессию.
            ++retries;
            if (!waitBeforeReconnect(retries))
            {
                std::cout << "[main] Пользовательское прерывание (Ctrl+C)\n";
                return 0;
            }
            command.outputPath = nextSessionPath(originalOutput, sessionNumber);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[main] Ошибка: " << error.what() << '\n';
        return 1;
    }
}
