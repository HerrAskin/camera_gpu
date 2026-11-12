#pragma once

#include <opencv2/core/mat.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

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
    // Длительность нужна программному encoder, у которого нет исходного GPU AVFrame.
    std::optional<std::chrono::microseconds> duration;
    // Номер подключения связывает кадр с отдельным выходным файлом после reconnect.
    std::uint64_t streamGeneration = 0;

    int width() const;
    int height() const;
};

}
