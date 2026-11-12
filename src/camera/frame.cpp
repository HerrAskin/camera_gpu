#include "camera/frame.hpp"

extern "C"
{
#include <libavutil/frame.h>
}

namespace camera
{
int Frame::width() const
{
    return gpuImage ? gpuImage->width : image.cols;
}

int Frame::height() const
{
    return gpuImage ? gpuImage->height : image.rows;
}

}
