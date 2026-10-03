#include "camera/file_video.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class TcpListener
{
public:
    TcpListener()
    {
        descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0)
        {
            throw std::runtime_error("Не удалось создать TCP-сокет");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(descriptor, 1) != 0)
        {
            close(descriptor);
            descriptor = -1;
            throw std::runtime_error("Не удалось запустить loopback listener");
        }

        socklen_t addressLength = sizeof(address);
        if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0)
        {
            close(descriptor);
            descriptor = -1;
            throw std::runtime_error("Не удалось получить порт listener");
        }
        port = ntohs(address.sin_port);
    }

    ~TcpListener()
    {
        if (descriptor >= 0)
        {
            close(descriptor);
        }
    }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    std::string url() const
    {
        return "rtsp://127.0.0.1:" + std::to_string(port) + "/stream";
    }

private:
    int descriptor = -1;
    unsigned short port = 0;
};

class JoinThread
{
public:
    explicit JoinThread(std::thread threadValue) : thread(std::move(threadValue))
    {
    }

    ~JoinThread()
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    void join()
    {
        thread.join();
    }

    JoinThread(const JoinThread&) = delete;
    JoinThread& operator=(const JoinThread&) = delete;

private:
    std::thread thread;
};

void requireElapsed(Clock::time_point start, std::chrono::milliseconds minimum, const char* message)
{
    const auto elapsed = Clock::now() - start;
    require(elapsed >= minimum, message);
    require(elapsed < std::chrono::seconds(3), "Операция ввода-вывода не завершилась за 3 секунды");
}

void testTimeout()
{
    TcpListener listener;
    camera::FileVideo video;
    const auto start = Clock::now();
    require(!video.init(listener.url(), std::chrono::milliseconds(500)), "RTSP init должен завершиться по таймауту");
    requireElapsed(start, std::chrono::milliseconds(200), "RTSP init завершился раньше ожидаемого таймаута");
}

void testRequestStop()
{
    TcpListener listener;
    camera::FileVideo video;
    const auto start = Clock::now();
    // Поток хранит ссылку на video, поэтому JoinThread гарантирует join до уничтожения объекта даже при исключении.
    JoinThread stopper(std::thread([&video]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        video.requestStop();
    }));
    const bool initialized = video.init(listener.url(), std::chrono::seconds(10));
    stopper.join();
    require(!initialized, "Остановка должна прервать ожидающий RTSP init");
    requireElapsed(start, std::chrono::milliseconds(150), "requestStop не должен срабатывать до вызова из потока");
}

void testInvalidTimeout()
{
    camera::FileVideo video;
    require(!video.init("rtsp://127.0.0.1:1/stream", std::chrono::milliseconds(0)), "Нулевой timeout должен быть отклонён");
}
}

int main()
{
    try
    {
        testInvalidTimeout();
        testTimeout();
        testRequestStop();
        std::cout << "I/O cancellation checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
