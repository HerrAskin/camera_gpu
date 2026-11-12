#include "camera/video_writer.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
const auto outputDirectory = std::filesystem::path(__FILE__).parent_path().parent_path() / "tmp" / "writer_queue_tests";

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

camera::Frame makeFrame(int id)
{
    camera::Frame frame;
    frame.image = cv::Mat(3, 4, CV_8UC1, cv::Scalar(id));
    frame.timestamp = std::chrono::microseconds(id);
    return frame;
}

std::vector<std::string> readLines(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(input.good(), "Не удалось открыть итоговый CSV");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        lines.push_back(line);
    }
    require(input.eof(), "Ошибка чтения итогового CSV");
    return lines;
}

camera::Frame makeVideoFrame()
{
    camera::Frame frame;
    frame.image = cv::Mat(128, 128, CV_8UC3, cv::Scalar(20, 80, 160));
    frame.timestamp = std::chrono::microseconds(0);
    frame.duration = std::chrono::microseconds(40000);
    return frame;
}

class CaptureOutput
{
public:
    CaptureOutput() : previous(std::cout.rdbuf(output.rdbuf()))
    {
    }
    ~CaptureOutput()
    {
        std::cout.rdbuf(previous);
    }
    std::ostringstream output;

private:
    std::streambuf* previous;
};

template<typename Exception = std::exception, typename Operation>
std::string exceptionMessage(Operation operation)
{
    try
    {
        operation();
    }
    catch (const Exception& error)
    {
        return error.what();
    }
    return {};
}

void testMp4FailedInitRecovery()
{
    const auto directory = outputDirectory / "init_directory";
    std::filesystem::create_directories(directory);
    const auto missingParent = outputDirectory / ("missing_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    require(!std::filesystem::exists(missingParent), "Родитель MP4 должен отсутствовать");
    for (const std::size_t depth : {0, 2})
    {
        camera::VideoWriter writer;
        const camera::VideoWriter::Options options{camera::VideoWriter::Format::H264Mp4, depth, false};
        require(!writer.init(directory, options), "MP4 init на каталог должен вернуть false");
        require(!writer.init(missingParent / "output.mp4", options), "MP4 init без родителя должен вернуть false");
        require(writer.outputPaths().empty(), "Failed init не должен публиковать output paths");
        require(!exceptionMessage([&] { writer.write(makeVideoFrame()); }).empty(), "Write после failed init должен отклоняться");
        require(!exceptionMessage([&] { writer.finish(); }).empty(), "Finish после failed init должен отклоняться");
        if (!avcodec_find_encoder_by_name("libx264"))
        {
            std::cout << "Skipping CPU MP4 recovery checks: FFmpeg libx264 encoder is unavailable\n";
            continue;
        }
        const auto recovered = outputDirectory / ("recovered_" + std::to_string(depth) + ".mp4");
        require(writer.init(recovered, options), "MP4 init должен восстановиться после ошибок открытия");
        writer.write(makeVideoFrame());
        writer.finish();
        require(writer.outputPaths() == std::vector<std::filesystem::path>{recovered}, "Recovery должен публиковать только успешный output");
        require(std::filesystem::file_size(recovered) > 0, "Recovery MP4 должен содержать output");
    }
}

void testFullDeviceErrors()
{
#ifdef __linux__
    require(std::filesystem::is_character_file("/dev/full"), "/dev/full должен быть символьным устройством");
    for (const auto format : {camera::VideoWriter::Format::Csv, camera::VideoWriter::Format::H264Mp4})
    {
        if (format == camera::VideoWriter::Format::H264Mp4 && !avcodec_find_encoder_by_name("libx264"))
        {
            std::cout << "Skipping CPU MP4 I/O checks: FFmpeg libx264 encoder is unavailable\n";
            continue;
        }
        for (const std::size_t depth : {0, 2})
        {
            CaptureOutput captured;
            camera::VideoWriter writer;
            require(writer.init("/dev/full", camera::VideoWriter::Options{format, depth, false}), "/dev/full должен открываться для записи");
            const auto frame = makeVideoFrame();
            const auto write = [&] { writer.write(frame); };
            const auto finish = [&] { writer.finish(); };
            const auto writeError = format == camera::VideoWriter::Format::Csv
                ? exceptionMessage<std::ios_base::failure>(write) : exceptionMessage(write);
            const auto finishError = format == camera::VideoWriter::Format::Csv && writeError.empty()
                ? exceptionMessage<std::ios_base::failure>(finish) : exceptionMessage(finish);
            require(!finishError.empty(), "Finish обязан сообщить об ошибке /dev/full");
            const auto& firstError = writeError.empty() ? finishError : writeError;
            if (format == camera::VideoWriter::Format::H264Mp4)
            {
                char noSpace[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(AVERROR(ENOSPC), noSpace, sizeof(noSpace));
                require(firstError.find(noSpace) != std::string::npos, "MP4 должен сообщить ENOSPC, а не validation/queue error");
            }
            require(!exceptionMessage([&] { writer.write(frame); }).empty(), "Повторный write после I/O error должен отклоняться");
            require(!exceptionMessage([&] { writer.finish(); }).empty(), "Повторный finish после I/O error должен отклоняться");
            require(captured.output.str().find("запись завершена") == std::string::npos, "I/O error не должен логировать успешное завершение");
        }
    }
#else
    std::cout << "Skipping /dev/full checks: Linux is required\n";
#endif
}

void testAsyncFifoAndLifecycle()
{
    const auto path = outputDirectory / "fifo.csv";
    camera::VideoWriter writer;
    require(writer.init(path, camera::VideoWriter::Options{camera::VideoWriter::Format::Csv, 16, false}), "Async CSV init должен пройти");
    for (int id = 0; id < 10; ++id)
    {
        auto frame = makeFrame(id);
        writer.write(frame);
        // Освобождение исходного заголовка проверяет, что очередь удерживает собственную ссылку cv::Mat.
        frame.image.release();
    }
    writer.finish();
    writer.finish();

    const auto lines = readLines(path);
    require(lines.size() == 11, "finish должен записать все принятые кадры и заголовок");
    require(lines.front() == "timestamp_us,width,height", "CSV должен содержать заголовок");
    for (int id = 0; id < 10; ++id)
    {
        require(lines[static_cast<std::size_t>(id + 1)] == std::to_string(id) + ",4,3", "Async CSV должен сохранять FIFO порядок и метаданные");
    }

    bool writeRejected = false;
    try
    {
        writer.write(makeFrame(10));
    }
    catch (const std::logic_error&)
    {
        writeRejected = true;
    }
    require(writeRejected, "Запись после finish должна отклоняться");
    require(!writer.init(path, camera::VideoWriter::Options{camera::VideoWriter::Format::Csv, 1, false}), "Повторный init должен отклоняться");
}

void testDefaultSynchronousMode()
{
    const auto path = outputDirectory / "sync.csv";
    camera::VideoWriter writer;
    require(writer.init(path), "Writer с queueDepth по умолчанию должен инициализироваться");
    writer.write(makeFrame(42));
    writer.finish();
    const auto lines = readLines(path);
    require(lines.size() == 2 && lines[1] == "42,4,3", "Режим queueDepth=0 должен записать кадр");
}

void testDestructorDrainsAndJoins()
{
    const auto path = outputDirectory / "destructor.csv";
    {
        camera::VideoWriter writer;
        require(writer.init(path, camera::VideoWriter::Options{camera::VideoWriter::Format::Csv, 32, false}), "Async CSV init для destructor должен пройти");
        for (int id = 0; id < 20; ++id)
        {
            writer.write(makeFrame(id));
        }
        // Выход из scope должен дождаться worker и закрыть CSV без явного finish.
    }
    const auto lines = readLines(path);
    require(lines.size() == 21, "Destructor должен дождаться записи и закрытия CSV");
    for (int id = 0; id < 20; ++id)
    {
        require(lines[static_cast<std::size_t>(id + 1)] == std::to_string(id) + ",4,3", "Destructor должен сохранить принятые кадры по FIFO");
    }
}

void testAsyncMp4WorkerError()
{
    const auto path = outputDirectory / "cpu_frame.mp4";
    camera::VideoWriter writer;
    require(writer.init(path, camera::VideoWriter::Options{camera::VideoWriter::Format::H264Mp4, 2, false}), "MP4 output должен открыться");
    writer.write(makeFrame(1));

    bool finishRejected = false;
    try
    {
        writer.finish();
    }
    catch (const std::runtime_error&)
    {
        finishRejected = true;
    }
    require(finishRejected, "Worker validation error должен распространяться из finish");

    bool writeRejected = false;
    try
    {
        writer.write(makeFrame(2));
    }
    catch (const std::runtime_error&)
    {
        writeRejected = true;
    }
    require(writeRejected, "Worker error должен быть доступен при следующем write");
}

void testOverflowPreservesAcceptedFrames()
{
    const auto path = outputDirectory / "overflow.csv";
    camera::VideoWriter writer;
    require(writer.init(path, camera::VideoWriter::Options{camera::VideoWriter::Format::Csv, 1, false}), "CSV init с малой очередью должен пройти");

    std::vector<int> accepted;
    bool overflowed = false;
    for (int id = 0; id < 100000; ++id)
    {
        try
        {
            writer.write(makeFrame(id));
            accepted.push_back(id);
        }
        catch (const std::runtime_error&)
        {
            overflowed = true;
            break;
        }
    }
    require(overflowed, "Переполнение очереди должно отклонить кадр");

    bool subsequentWriteRejected = false;
    try
    {
        writer.write(makeFrame(100001));
    }
    catch (const std::runtime_error&)
    {
        subsequentWriteRejected = true;
    }
    require(subsequentWriteRejected, "Переполнение должно стать терминальной ошибкой приема");

    bool finishRejected = false;
    try
    {
        writer.finish();
    }
    catch (const std::runtime_error&)
    {
        finishRejected = true;
    }
    require(finishRejected, "finish должен сообщить об ошибке переполнения после drain");

    const auto lines = readLines(path);
    require(lines.size() == accepted.size() + 1, "После переполнения CSV должен содержать ровно принятые кадры");
    require(lines.front() == "timestamp_us,width,height", "CSV после переполнения должен содержать заголовок");
    for (std::size_t index = 0; index < accepted.size(); ++index)
    {
        const int id = accepted[index];
        require(lines[index + 1] == std::to_string(id) + ",4,3", "После переполнения принятые кадры должны остаться в FIFO порядке");
    }
}

void testCpuMp4AndReconnectSegments()
{
    if (!avcodec_find_encoder_by_name("libx264"))
    {
        std::cout << "Skipping CPU MP4 checks: FFmpeg libx264 encoder is unavailable\n";
        return;
    }

    const auto uniqueDirectory = outputDirectory / ("generation_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(uniqueDirectory);
    const auto basePath = uniqueDirectory / "generation.mp4";
    camera::VideoWriter writer;
    require(writer.init(basePath, camera::VideoWriter::Options{camera::VideoWriter::Format::H264Mp4, 8, false}),
        "CPU MP4 output должен открыться");
    for (std::uint64_t generation = 0; generation < 2; ++generation)
    {
        for (int index = 0; index < 3; ++index)
        {
            camera::Frame frame;
            frame.image = cv::Mat(128, 128, CV_8UC3, cv::Scalar(20 + index, 80, 160));
            frame.timestamp = std::chrono::microseconds(index * 40000);
            frame.duration = std::chrono::microseconds(40000);
            frame.streamGeneration = generation;
            writer.write(frame);
        }
    }
    writer.finish();

    const auto paths = writer.outputPaths();
    require(paths.size() == 2, "Reconnect должен создать два MP4 сегмента");
    require(paths[0] == basePath, "Первый сегмент должен использовать исходный путь");
    require(paths[1].filename() == "generation_session2.mp4", "Второй сегмент должен получить суффикс session2");
    for (const auto& path : paths)
    {
        require(std::filesystem::file_size(path) > 0, "CPU MP4 segment должен содержать muxed output");
    }
}
}

int main()
{
    try
    {
        std::filesystem::create_directories(outputDirectory);
        testMp4FailedInitRecovery();
        testFullDeviceErrors();
        testAsyncFifoAndLifecycle();
        testDefaultSynchronousMode();
        testDestructorDrainsAndJoins();
        testAsyncMp4WorkerError();
        testOverflowPreservesAcceptedFrames();
        testCpuMp4AndReconnectSegments();
        std::cout << "Writer queue checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
