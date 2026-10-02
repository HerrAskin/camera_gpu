#include "camera/writer.hpp"

#include <stdexcept>

namespace camera
{
Writer::Writer(const std::filesystem::path& path)
{
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(path);
    output << "timestamp_us,width,height\n";
}

void Writer::write(const Frame& frame)
{
    if (finished || frame.image.empty())
    {
        throw std::logic_error("Writer requires an image and open output");
    }
    output << frame.timestamp.count() << ',' << frame.image.cols << ','
           << frame.image.rows << '\n';
}

void Writer::finish()
{
    if (!finished)
    {
        output.close();
        finished = true;
    }
}
}
