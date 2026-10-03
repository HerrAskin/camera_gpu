#pragma once

#include "camera/video.hpp"

namespace camera
{
/** Mock-обработчик: process оставляет кадр без изменений. */
class Processor
{
public:
    /** Обрабатывает кадр; текущая mock-реализация ничего не меняет. */
    void process(Frame& frame);
};
}
