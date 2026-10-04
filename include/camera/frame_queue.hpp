#pragma once

#include "camera/video.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace camera
{
/** Ограниченная очередь кадров для передачи между производителем и обработчиком.
 * При переполнении сохраняет самые новые кадры: удаление Frame освобождает только
 * разделяемые ссылки на CPU/GPU-буферы, а не копирует пиксели. Владелец обязан
 * закрыть очередь и дождаться потоков, обращающихся к ней, до её уничтожения.
 */
class FrameQueue
{
public:
    /** Создаёт очередь; capacity должна быть положительной. */
    explicit FrameQueue(std::size_t capacity);

    /** Добавляет кадр, удаляя самый старый при переполнении; после close возвращает false. */
    bool push(Frame frame);

    /** Добавляет только при наличии места; false означает полную или закрытую очередь.
     * Уже принятые кадры сохраняются, droppedCount не меняется.
     */
    bool tryPush(Frame frame);

    /** Извлекает кадр; timeout=0 означает немедленный опрос. nullopt означает timeout
     * либо закрытую и опустевшую очередь; isClosed() позволяет различить эти случаи.
     */
    std::optional<Frame> pop(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

    /** Закрывает очередь, сохраняя кадры для извлечения; повторный вызов безопасен. */
    void close();

    /** Возвращает текущее число ожидающих кадров. */
    std::size_t size() const;

    /** Возвращает накопленное число кадров, отброшенных при переполнении. */
    std::size_t droppedCount() const;

    /** Возвращает состояние закрытия очереди. */
    bool isClosed() const;

private:
    const std::size_t capacity;
    std::deque<Frame> frames;
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool closed = false;
    std::size_t dropped = 0;
};
}
