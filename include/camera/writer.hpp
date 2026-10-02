#pragma once

#include "camera/video.hpp"

#include <filesystem>
#include <fstream>

namespace camera
{
// Пока сохраняет только метаданные в CSV; видеокодирование будет добавлено отдельно.
class Writer
{
public:
    explicit Writer(const std::filesystem::path& path);
    void write(const Frame& frame);
    void finish();

private:
    std::ofstream output;
    bool finished = false;
};
}
