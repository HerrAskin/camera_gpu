#pragma once
#include "camera/video_capture.hpp"

namespace camera
{
/** Создаёт CPU-источник кадров для проверок без камеры. */
std::unique_ptr<VideoCapture> createMockCapture(std::size_t frameCount = 5);
}
