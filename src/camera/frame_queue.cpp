#include "camera/frame_queue.hpp"

#include <stdexcept>
#include <utility>

namespace camera
{
FrameQueue::FrameQueue(std::size_t capacityValue) : capacity(capacityValue)
{
    if (capacity == 0)
    {
        throw std::invalid_argument("Вместимость очереди кадров должна быть положительной");
    }
}

bool FrameQueue::push(Frame frame)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed)
        {
            return false;
        }

        // Сначала вставляем новый кадр: если выделение памяти завершится ошибкой,
        // очередь останется прежней и ранее накопленные кадры не будут потеряны.
        frames.push_back(std::move(frame));
        if (frames.size() > capacity)
        {
            frames.pop_front();
            ++dropped;
        }
    }
    condition.notify_one();
    return true;
}

bool FrameQueue::tryPush(Frame frame)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed || frames.size() == capacity)
        {
            return false;
        }
        frames.push_back(std::move(frame));
    }
    condition.notify_one();
    return true;
}

std::optional<Frame> FrameQueue::pop(std::chrono::milliseconds timeout)
{
    if (timeout < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("Таймаут извлечения не может быть отрицательным");
    }

    std::unique_lock<std::mutex> lock(mutex);
    if (timeout > std::chrono::milliseconds::zero())
    {
        condition.wait_for(lock, timeout, [this]
        {
            return closed || !frames.empty();
        });
    }
    if (frames.empty())
    {
        return std::nullopt;
    }

    std::optional<Frame> result(std::move(frames.front()));
    frames.pop_front();
    return result;
}

void FrameQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
    }
    condition.notify_all();
}

std::size_t FrameQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return frames.size();
}

std::size_t FrameQueue::droppedCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return dropped;
}

bool FrameQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return closed;
}
}
