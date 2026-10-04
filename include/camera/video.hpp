#pragma once

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

struct AVFrame;

namespace camera
{
/** Один кадр с CPU- или CUDA-представлением изображения.
 * Заполнено ровно одно из полей image и gpuImage. cv::Mat копирует заголовок и разделяет
 * владение пикселями; gpuImage разделяет владение ссылкой FFmpeg. timestamp — смещение
 * в микросекундах, а не UTC-время.
 */
struct Frame
{
    cv::Mat image;
    // Ровно одно представление заполнено; shared_ptr владеет FFmpeg-ссылкой и переживает Video.
    std::shared_ptr<AVFrame> gpuImage;
    std::optional<std::chrono::microseconds> timestamp;

    int width() const;
    int height() const;
};

/** Источник кадров. read() извлекает уже готовый кадр без ожидания; nullopt означает, что
 * сейчас очередь пуста. isFinished() различает временную пустоту и конец источника.
 */
class Video
{
public:
    virtual ~Video() = default;
    /** Извлекает готовый кадр без ожидания; nullopt означает пустую очередь. */
    virtual std::optional<Frame> read() = 0;
    /** Сообщает, что источник закончил работу и готовых кадров больше нет. */
    virtual bool isFinished() const = 0;
    /** Запрашивает остановку; источник без worker-потока может ничего не делать. */
    virtual void requestStop() noexcept {}
    /** Возвращает описание уже настроенного источника. */
    virtual std::string description() const = 0;
};

/** Создаёт mock-источник с заданным числом кадров. */
std::unique_ptr<Video> createMockVideo(std::size_t frameCount = 5);
}
