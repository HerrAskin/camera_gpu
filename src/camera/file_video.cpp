#include "camera/file_video.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace camera
{
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

AVPixelFormat selectCudaFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
    {
        if (*format == AV_PIX_FMT_CUDA)
        {
            return *format;
        }
    }
    // Не разрешаем незаметный переход на программное декодирование.
    return AV_PIX_FMT_NONE;
}

}

void FileVideo::closeInput(AVFormatContext* context)
{
    avformat_close_input(&context);
}

void FileVideo::releaseDevice(AVBufferRef* device)
{
    av_buffer_unref(&device);
}

void FileVideo::closeDecoder(AVCodecContext* decoder)
{
    avcodec_free_context(&decoder);
}

bool FileVideo::init(const std::string& path)
{
    if (context)
    {
        std::cerr << "[FileVideo::init] Ошибка инициализации: объект уже инициализирован\n";
        return false;
    }
    try
    {
        AVFormatContext* rawContext = nullptr;
        check(avformat_open_input(&rawContext, path.c_str(), nullptr, nullptr), "Открытие файла");
        // Владение передаётся сразу: последующие ошибки не оставят открытый файл.
        context.reset(rawContext);
        draining = false;
        ended = false;
        check(avformat_find_stream_info(context.get(), nullptr), "Чтение информации о потоках");
        streamIndex = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(streamIndex, "Поиск видеопотока");

        AVBufferRef* rawDevice = nullptr;
        const int result = av_hwdevice_ctx_create(&rawDevice, AV_HWDEVICE_TYPE_CUDA, "0", nullptr, 0);
        device.reset(rawDevice);
        check(result, "Создание CUDA-контекста");

        const AVStream* stream = context->streams[streamIndex];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec)
        {
            throw std::runtime_error("Decoder для codec видеопотока не найден");
        }
        bool supportsCuda = false;
        for (int index = 0; const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index); ++index)
        {
            if (config->device_type == AV_HWDEVICE_TYPE_CUDA && config->pix_fmt == AV_PIX_FMT_CUDA && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            {
                supportsCuda = true;
                break;
            }
        }
        if (!supportsCuda)
        {
            throw std::runtime_error("Найденный decoder не поддерживает CUDA device context");
        }

        decoder.reset(avcodec_alloc_context3(codec));
        if (!decoder)
        {
            throw std::bad_alloc();
        }
        check(avcodec_parameters_to_context(decoder.get(), stream->codecpar), "Передача параметров codec");
        decoder->pkt_timebase = stream->time_base;
        decoder->get_format = selectCudaFormat;
        // Decoder владеет отдельной ссылкой на то же устройство, а не новым CUDA-контекстом.
        decoder->hw_device_ctx = av_buffer_ref(device.get());
        if (!decoder->hw_device_ctx)
        {
            throw std::bad_alloc();
        }
        check(avcodec_open2(decoder.get(), codec, nullptr), "Открытие decoder");
        std::cout << "[FileVideo::init] инициализация успешна\n";
        return true;
    }
    catch (const std::exception& error)
    {
        // Сначала освобождается decoder с его ссылкой на устройство, затем сам device и вход.
        decoder.reset();
        device.reset();
        context.reset();
        streamIndex = -1;
        draining = false;
        ended = false;
        std::cerr << "[FileVideo::init] Ошибка инициализации: " << error.what() << '\n';
        return false;
    }
}

bool FileVideo::decodeFirstFrame()
{
    if (!decoder || !avcodec_is_open(decoder.get()) || !context || streamIndex < 0)
    {
        std::cerr << "[FileVideo::decodeFirstFrame] Сначала успешно вызови FileVideo::init()\n";
        return false;
    }

    try
    {
        auto frame = read();
        if (!frame)
        {
            std::cerr << "[FileVideo::decodeFirstFrame] Видеопоток не содержит декодируемых кадров\n";
            return false;
        }
        std::cout << "[FileVideo::decodeFirstFrame] Размер: " << frame->width() << 'x' << frame->height()
                  << ", формат: CUDA/GPU, timestamp_us: ";
        if (frame->timestamp)
        {
            std::cout << frame->timestamp->count();
        }
        else
        {
            std::cout << "не задан";
        }
        std::cout << '\n';
        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[FileVideo::decodeFirstFrame] " << error.what() << '\n';
        return false;
    }
}

std::optional<Frame> FileVideo::read()
{
    if (!decoder || !avcodec_is_open(decoder.get()) || !context || streamIndex < 0)
    {
        throw std::logic_error("[FileVideo::read] Сначала успешно вызови FileVideo::init()");
    }
    if (ended)
    {
        return std::nullopt;
    }
    const auto packetDeleter = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(packetDeleter)> packet(av_packet_alloc(), packetDeleter);
    if (!packet)
    {
        throw std::runtime_error("[FileVideo::read] Не удалось выделить packet");
    }
    const auto frameDeleter = [](AVFrame* frame) { av_frame_free(&frame); };
    std::shared_ptr<AVFrame> gpuFrame(av_frame_alloc(), frameDeleter);
    if (!gpuFrame)
    {
        throw std::runtime_error("[FileVideo::read] Не удалось выделить frame");
    }
    while (true)
    {
        const int receiveResult = avcodec_receive_frame(decoder.get(), gpuFrame.get());
        if (receiveResult == 0)
        {
            if (gpuFrame->format != AV_PIX_FMT_CUDA || !gpuFrame->hw_frames_ctx)
            {
                throw std::runtime_error("[FileVideo::read] Decoder вернул кадр без CUDA hw_frames_ctx");
            }
            Frame result;
            result.gpuImage = std::move(gpuFrame);
            // Duration кадра использует единицы потока; Writer сможет перевести его в time base encoder.
            const AVStream* stream = context->streams[streamIndex];
            result.gpuImage->time_base = stream->time_base;
            if (result.gpuImage->best_effort_timestamp != AV_NOPTS_VALUE)
            {
                result.timestamp = std::chrono::microseconds(av_rescale_q(result.gpuImage->best_effort_timestamp, stream->time_base, AVRational{1, 1000000}));
            }
            return result;
        }
        av_frame_unref(gpuFrame.get());
        if (receiveResult == AVERROR_EOF)
        {
            ended = true;
            return std::nullopt;
        }
        if (receiveResult != AVERROR(EAGAIN))
        {
            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(receiveResult, message, sizeof(message));
            throw std::runtime_error(std::string("[FileVideo::read] Получение кадра: ") + message);
        }
        if (draining)
        {
            throw std::runtime_error("[FileVideo::read] Decoder запросил packet после EOF flush");
        }
        int readResult = 0;
        while ((readResult = av_read_frame(context.get(), packet.get())) >= 0)
        {
            if (packet->stream_index == streamIndex)
            {
                break;
            }
            av_packet_unref(packet.get());
        }
        if (readResult == AVERROR_EOF)
        {
            const int sendResult = avcodec_send_packet(decoder.get(), nullptr);
            if (sendResult < 0 && sendResult != AVERROR_EOF)
            {
                char message[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(sendResult, message, sizeof(message));
                throw std::runtime_error(std::string("[FileVideo::read] EOF flush: ") + message);
            }
            draining = true;
            continue;
        }
        if (readResult < 0)
        {
            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(readResult, message, sizeof(message));
            throw std::runtime_error(std::string("[FileVideo::read] Чтение packet: ") + message);
        }
        const int sendResult = avcodec_send_packet(decoder.get(), packet.get());
        av_packet_unref(packet.get());
        if (sendResult < 0)
        {
            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(sendResult, message, sizeof(message));
            throw std::runtime_error(std::string("[FileVideo::read] Передача packet decoder: ") + message);
        }
    }
}

std::string FileVideo::description() const
{
    if (!decoder || !avcodec_is_open(decoder.get()))
    {
        throw std::logic_error("[FileVideo::description] Сначала успешно вызови FileVideo::init()");
    }
    const AVStream* stream = context->streams[streamIndex];
    const AVCodecParameters* codec = stream->codecpar;
    std::ostringstream output;
    output << "Видеопоток: " << streamIndex << '\n'
           << "Codec: " << avcodec_get_name(codec->codec_id) << '\n'
           << "Размер: " << codec->width << 'x' << codec->height << '\n'
           << "Time base: " << stream->time_base.num << '/'
           << stream->time_base.den << '\n'
           << "CUDA-контекст: создан, устройство 0" << '\n'
           << "Decoder: " << decoder->codec->name << ", открыт с CUDA";
    return output.str();
}

}
