#include "camera/frame_queue.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
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
    frame.image = cv::Mat(1, 1, CV_32SC1, cv::Scalar(id));
    return frame;
}

int frameId(const camera::Frame& frame)
{
    return frame.image.at<int>(0, 0);
}

void testCapacityAndLatestFrames()
{
    bool rejectedZero = false;
    try
    {
        camera::FrameQueue invalid(0);
    }
    catch (const std::invalid_argument&)
    {
        rejectedZero = true;
    }
    require(rejectedZero, "Нулевая вместимость должна отклоняться");

    camera::FrameQueue queue(1);
    require(queue.push(makeFrame(1)), "Очередь должна принимать кадр");
    require(queue.push(makeFrame(2)), "Переполненная очередь должна принимать новый кадр");
    require(queue.size() == 1, "Вместимость очереди не должна превышаться");
    require(queue.droppedCount() == 1, "Должен учитываться отброшенный кадр");
    const auto newest = queue.pop();
    require(newest && frameId(*newest) == 2, "При переполнении должен оставаться newest кадр");

    camera::FrameQueue multipleOverflow(3);
    for (int id = 0; id <= 4; ++id)
    {
        multipleOverflow.push(makeFrame(id));
    }
    require(multipleOverflow.size() == 3, "После нескольких переполнений размер должен быть равен capacity");
    require(multipleOverflow.droppedCount() == 2, "После нескольких переполнений должны учитываться обе потери");
    for (int expected = 2; expected <= 4; ++expected)
    {
        const auto frame = multipleOverflow.pop();
        require(frame && frameId(*frame) == expected, "Несколько переполнений должны сохранять три newest кадра по FIFO");
    }

    camera::FrameQueue fifo(3);
    fifo.push(makeFrame(10));
    fifo.push(makeFrame(11));
    fifo.push(makeFrame(12));
    for (int expected = 10; expected <= 12; ++expected)
    {
        const auto frame = fifo.pop();
        require(frame && frameId(*frame) == expected, "Кадры без переполнения должны идти FIFO");
    }
    require(fifo.droppedCount() == 0, "Без переполнения счетчик потерь должен быть нулевым");
}

void testRejectOverflow()
{
    camera::FrameQueue queue(2);
    require(queue.tryPush(makeFrame(1)) && queue.tryPush(makeFrame(2)), "Свободная очередь должна принимать кадры");
    require(!queue.tryPush(makeFrame(3)), "Полная очередь должна отклонять новый кадр");
    require(queue.droppedCount() == 0, "Отклонение не должно вытеснять принятые кадры");
    require(frameId(*queue.pop()) == 1, "Первый принятый кадр должен сохраниться");
    require(queue.tryPush(makeFrame(4)), "После извлечения снова доступно место");
    queue.close();
    require(!queue.tryPush(makeFrame(5)), "Закрытая очередь должна отклонять кадры");
    require(frameId(*queue.pop()) == 2 && frameId(*queue.pop()) == 4, "Закрытая очередь сохраняет FIFO");
}

void testTimeoutAndCloseDrain()
{
    camera::FrameQueue queue(2);
    const auto start = std::chrono::steady_clock::now();
    require(!queue.pop(std::chrono::milliseconds(30)), "Пустая очередь должна вернуть timeout");
    require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(20), "Ожидание должно учитывать timeout");
    bool rejectedNegative = false;
    try
    {
        queue.pop(std::chrono::milliseconds(-1));
    }
    catch (const std::invalid_argument&)
    {
        rejectedNegative = true;
    }
    require(rejectedNegative, "Отрицательный timeout должен отклоняться");

    queue.push(makeFrame(20));
    queue.close();
    queue.close();
    require(queue.isClosed(), "close должен установить состояние закрытия");
    require(!queue.push(makeFrame(21)), "Закрытая очередь не должна принимать кадры");
    const auto queued = queue.pop();
    require(queued && frameId(*queued) == 20, "close должен оставить кадры для чтения");
    require(!queue.pop(), "Пустая закрытая очередь должна вернуть nullopt");
}

void testCloseWakesWaitingConsumer()
{
    camera::FrameQueue queue(1);
    std::promise<void> starting;
    auto started = starting.get_future();
    auto result = std::async(std::launch::async, [&queue, &starting]
    {
        starting.set_value();
        return queue.pop(std::chrono::seconds(5));
    });
    started.wait();
    queue.close();
    const auto status = result.wait_for(std::chrono::seconds(1));
    if (status != std::future_status::ready)
    {
        queue.close();
    }
    require(status == std::future_status::ready, "close должен разбудить ожидающего потребителя");
    require(!result.get(), "Потребитель после close пустой очереди должен получить nullopt");
}

void testPushWakesWaitingConsumer()
{
    camera::FrameQueue queue(1);
    std::promise<void> starting;
    auto started = starting.get_future();
    auto result = std::async(std::launch::async, [&queue, &starting]
    {
        starting.set_value();
        return queue.pop(std::chrono::seconds(5));
    });
    started.wait();
    queue.push(makeFrame(42));
    const auto status = result.wait_for(std::chrono::seconds(1));
    if (status != std::future_status::ready)
    {
        queue.close();
    }
    require(status == std::future_status::ready, "push должен разбудить ожидающего потребителя");
    const auto frame = result.get();
    require(frame && frameId(*frame) == 42, "Потребитель должен получить добавленный кадр");
}

void testConcurrentProducerConsumer()
{
    constexpr int producedCount = 10000;
    camera::FrameQueue queue(8);
    std::vector<int> consumed;
    consumed.reserve(producedCount);
    std::atomic<bool> producerSucceeded{true};

    std::thread producer([&queue, &producerSucceeded]
    {
        for (int id = 0; id < producedCount; ++id)
        {
            if (!queue.push(makeFrame(id)))
            {
                producerSucceeded = false;
                break;
            }
        }
        queue.close();
    });

    std::thread consumer([&queue, &consumed]
    {
        while (auto frame = queue.pop(std::chrono::seconds(1)))
        {
            consumed.push_back(frameId(*frame));
        }
    });
    producer.join();
    consumer.join();

    require(producerSucceeded, "Производитель не должен столкнуться с закрытой очередью");
    require(!consumed.empty(), "Потребитель должен получить кадры");
    for (std::size_t index = 1; index < consumed.size(); ++index)
    {
        require(consumed[index] > consumed[index - 1], "Потребитель должен видеть монотонные ID");
    }
    require(consumed.back() == producedCount - 1, "После переполнений должен сохраниться последний кадр");
    require(consumed.size() + queue.droppedCount() == producedCount, "Принятые кадры должны делиться на прочитанные и отброшенные");
    require(queue.size() <= 8, "Параллельная очередь не должна превышать вместимость");
}
}

int main()
{
    try
    {
        testCapacityAndLatestFrames();
        testRejectOverflow();
        testTimeoutAndCloseDrain();
        testCloseWakesWaitingConsumer();
        testPushWakesWaitingConsumer();
        testConcurrentProducerConsumer();
        std::cout << "Frame queue checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
