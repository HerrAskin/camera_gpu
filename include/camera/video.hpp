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

/** Источник кадров. read() возвращает nullopt при штатном EOF и бросает исключение при ошибке. */
class Video
{
public:
    virtual ~Video() = default;
    /** Читает следующий кадр; nullopt означает EOF. */
    virtual std::optional<Frame> read() = 0;
    /** Возвращает описание уже настроенного источника. */
    virtual std::string description() const = 0;
};

/** Создаёт mock-источник с заданным числом кадров. */
std::unique_ptr<Video> createMockVideo(std::size_t frameCount = 5);
}
