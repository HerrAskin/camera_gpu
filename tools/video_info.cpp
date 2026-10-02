extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
void check(int result, const char* operation)
{
    if (result < 0)
    {
        char message[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Использование: video_info <video-file>\n";
        return 1;
    }
    try
    {
        AVFormatContext* rawContext = nullptr;
        check(avformat_open_input(&rawContext, argv[1], nullptr, nullptr), "Открытие файла");

        // Контекст закрывается и при успешном завершении, и при исключении ниже.
        auto closeInput = [](AVFormatContext* context)
        {
            avformat_close_input(&context);
        };
        std::unique_ptr<AVFormatContext, decltype(closeInput)> context(rawContext, closeInput);

        check(avformat_find_stream_info(context.get(), nullptr), "Чтение информации о потоках");
        const int streamIndex = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO,
                                                    -1, -1, nullptr, 0);
        check(streamIndex, "Поиск видеопотока");
        const AVStream* stream = context->streams[streamIndex];
        const AVCodecParameters* codec = stream->codecpar;

        std::cout << "Видеопоток: " << streamIndex << '\n'
                  << "Codec: " << avcodec_get_name(codec->codec_id) << '\n'
                  << "Размер: " << codec->width << 'x' << codec->height << '\n'
                  << "Time base: " << stream->time_base.num << '/'
                  << stream->time_base.den << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Ошибка: " << error.what() << '\n';
        return 1;
    }
}
