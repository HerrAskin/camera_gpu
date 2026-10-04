#include "camera/writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void testAsyncFifoAndLifecycle()
{
    const auto path = outputDirectory / "fifo.csv";
    camera::Writer writer;
    require(writer.init(path, camera::Writer::Format::Csv, 16), "Async CSV init должен пройти");
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
    require(!writer.init(path, camera::Writer::Format::Csv, 1), "Повторный init должен отклоняться");
}

void testDefaultSynchronousMode()
{
    const auto path = outputDirectory / "sync.csv";
    camera::Writer writer;
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
        camera::Writer writer;
        require(writer.init(path, camera::Writer::Format::Csv, 32), "Async CSV init для destructor должен пройти");
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
    camera::Writer writer;
    require(writer.init(path, camera::Writer::Format::H264Mp4, 2), "MP4 output должен открыться");
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
    camera::Writer writer;
    require(writer.init(path, camera::Writer::Format::Csv, 1), "CSV init с малой очередью должен пройти");

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
}

int main()
{
    try
    {
        std::filesystem::create_directories(outputDirectory);
        testAsyncFifoAndLifecycle();
        testDefaultSynchronousMode();
        testDestructorDrainsAndJoins();
        testAsyncMp4WorkerError();
        testOverflowPreservesAcceptedFrames();
        std::cout << "Writer queue checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
