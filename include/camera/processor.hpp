#pragma once

#include "camera/frame.hpp"

namespace camera
{
/** Mock-обработчик: process оставляет кадр без изменений. */
class Processor
{
public:
    /** Обрабатывает кадр; текущая mock-реализация ничего не меняет. */
    void process(Frame& /*frame*/)
    {
        // Полезная обработка будет добавлена на следующем этапе.
    }
};
}
