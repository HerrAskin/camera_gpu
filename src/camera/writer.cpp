#include "camera/writer.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
}

#include <iostream>
#include <stdexcept>

namespace camera
{
namespace
{
constexpr AVRational encoderTimeBase{1, 1000000};

std::string errorText(int code)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}
}

void Writer::freeFormat(AVFormatContext* context)
{
    if (context)
    {
        if (context->pb && !(context->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
    }
}

void Writer::freeCodec(AVCodecContext* context)
{
    avcodec_free_context(&context);
}

void Writer::freePacket(AVPacket* value)
{
    av_packet_free(&value);
}

Writer::~Writer() = default;

void Writer::fail(const std::string& message, const char* operation)
{
    failed = true;
    throw std::runtime_error(std::string("[") + operation + "] " + message);
}

bool Writer::init(const std::filesystem::path& path, Format requestedFormat)
{
    if (initialized)
    {
        std::cerr << "[Writer::init] Ошибка: объект уже инициализирован\n";
        return false;
    }
    try
    {
        format = requestedFormat;
        if (format == Format::Csv)
        {
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.open(path);
            output << "timestamp_us,width,height\n";
        }
        else
        {
            AVFormatContext* rawContext = nullptr;
            const int result = avformat_alloc_output_context2(&rawContext, nullptr, "mp4", path.string().c_str());
            formatContext.reset(rawContext);
            if (result < 0 || !formatContext)
            {
                throw std::runtime_error("Создание MP4 muxer: " + errorText(result < 0 ? result : AVERROR_UNKNOWN));
            }
            if (!(formatContext->oformat->flags & AVFMT_NOFILE))
            {
                const int openResult = avio_open(&formatContext->pb, path.string().c_str(), AVIO_FLAG_WRITE);
                if (openResult < 0)
                {
                    throw std::runtime_error("Открытие MP4: " + errorText(openResult));
                }
            }
        }
        initialized = true;
        std::cout << "[Writer::init] инициализация успешна\n";
        return true;
    }
    catch (const std::exception& error)
    {
        output.exceptions(std::ios::goodbit);
        output.close();
        output.clear();
        formatContext.reset();
        codecContext.reset();
        std::cerr << "[Writer::init] Ошибка инициализации: " << error.what() << '\n';
        return false;
    }
}

void Writer::initializeVideo(const Frame& frame)
{
    if (!frame.gpuImage || !frame.image.empty() || frame.gpuImage->format != AV_PIX_FMT_CUDA || !frame.gpuImage->hw_frames_ctx)
    {
        fail("H264 MP4 требует CUDA frame с hw_frames_ctx");
    }
    if (frame.width() <= 0 || frame.height() <= 0)
    {
        fail("Размер кадра должен быть положительным");
    }
    const AVFrame& source = *frame.gpuImage;
    if (!frame.timestamp || source.time_base.num <= 0 || source.time_base.den <= 0 || source.duration <= 0)
    {
        fail("Timestamp, исходные time_base и положительная duration обязательны для MP4");
    }
    const double durationSeconds = av_q2d(source.time_base) * static_cast<double>(source.duration);
    if (durationSeconds <= 0.0)
    {
        fail("Не удалось определить nominal frame rate из duration первого кадра");
    }
    const AVRational nominalFrameRate = av_d2q(1.0 / durationSeconds, 1000000);
    if (nominalFrameRate.num <= 0 || nominalFrameRate.den <= 0)
    {
        fail("Некорректный nominal frame rate первого кадра");
    }
    const AVCodec* encoder = avcodec_find_encoder_by_name("h264_nvenc");
    if (!encoder)
    {
        fail("FFmpeg encoder h264_nvenc не найден; CPU fallback отключён");
    }
    codecContext.reset(avcodec_alloc_context3(encoder));
    if (!codecContext)
    {
        fail("Не удалось выделить encoder context");
    }
    codecContext->width = frame.width();
    codecContext->height = frame.height();
    codecContext->pix_fmt = AV_PIX_FMT_CUDA;
    codecContext->time_base = encoderTimeBase;
    // Nominal rate нужна encoder/VUI; фактические PTS и длительности задаются для каждого кадра отдельно.
    codecContext->framerate = nominalFrameRate;
    codecContext->bit_rate = 2000000;
    codecContext->max_b_frames = 0;
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER)
    {
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    codecContext->hw_frames_ctx = av_buffer_ref(frame.gpuImage->hw_frames_ctx);
    if (!codecContext->hw_frames_ctx)
    {
        fail("Не удалось передать CUDA hw_frames_ctx encoder");
    }
    int result = avcodec_open2(codecContext.get(), encoder, nullptr);
    if (result < 0)
    {
        fail("Открытие h264_nvenc: " + errorText(result));
    }
    AVStream* stream = avformat_new_stream(formatContext.get(), nullptr);
    if (!stream)
    {
        fail("Не удалось создать MP4 video stream");
    }
    stream->time_base = codecContext->time_base;
    result = avcodec_parameters_from_context(stream->codecpar, codecContext.get());
    if (result < 0)
    {
        fail("Копирование параметров encoder: " + errorText(result));
    }
    packet.reset(av_packet_alloc());
    if (!packet)
    {
        fail("Не удалось выделить packet");
    }
    result = avformat_write_header(formatContext.get(), nullptr);
    if (result < 0)
    {
        fail("Запись MP4 header: " + errorText(result));
    }
    headerWritten = true;
}

void Writer::drainPackets(bool flushing)
{
    AVStream* stream = formatContext->streams[0];
    const char* operation = flushing ? "Writer::finish" : "Writer::write";
    while (true)
    {
        const int result = avcodec_receive_packet(codecContext.get(), packet.get());
        if (result == AVERROR(EAGAIN))
        {
            if (flushing)
            {
                fail("Encoder запросил новые кадры после flush", operation);
            }
            return;
        }
        if (result == AVERROR_EOF)
        {
            return;
        }
        if (result < 0)
        {
            fail("Получение encoded packet: " + errorText(result), operation);
        }
        const auto duration = pendingDurations.find(packet->pts);
        if (duration == pendingDurations.end())
        {
            fail("Encoded packet не соответствует ожидаемому PTS", operation);
        }
        const int64_t packetDuration = duration->second;
        pendingDurations.erase(duration);
        av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
        packet->duration = av_rescale_q(packetDuration, codecContext->time_base, stream->time_base);
        if (packet->duration <= 0)
        {
            fail("Duration кадра потерялась при преобразовании в stream time_base", operation);
        }
        packet->stream_index = stream->index;
        const int muxResult = av_interleaved_write_frame(formatContext.get(), packet.get());
        av_packet_unref(packet.get());
        if (muxResult < 0)
        {
            fail("Запись MP4 packet: " + errorText(muxResult), operation);
        }
    }
}

void Writer::write(const Frame& frame)
{
    if (!initialized || failed || finished)
    {
        throw std::logic_error("[Writer::write] Writer не готов к записи");
    }
    if ((!frame.gpuImage && frame.image.empty()) || frame.width() <= 0 || frame.height() <= 0)
    {
        fail("Для записи требуется непустое изображение");
    }
    try
    {
        if (format == Format::Csv)
        {
            if (frame.timestamp)
            {
                output << frame.timestamp->count();
            }
            output << ',' << frame.width() << ',' << frame.height() << '\n';
            return;
        }
        if (!frame.gpuImage || !frame.image.empty() || frame.gpuImage->format != AV_PIX_FMT_CUDA || !frame.gpuImage->hw_frames_ctx)
        {
            fail("H264 MP4 требует CUDA frame с hw_frames_ctx");
        }
        if (!frame.timestamp)
        {
            fail("У CUDA кадра для MP4 отсутствует timestamp");
        }
        if (!headerWritten)
        {
            initializeVideo(frame);
        }
        if (frame.width() != codecContext->width || frame.height() != codecContext->height || !frame.gpuImage || !frame.gpuImage->hw_frames_ctx || frame.gpuImage->format != AV_PIX_FMT_CUDA)
        {
            fail("Размер или CUDA representation кадра изменились во время записи");
        }
        if (frame.gpuImage->hw_frames_ctx->buffer != codecContext->hw_frames_ctx->buffer)
        {
            fail("CUDA hw_frames_ctx изменился во время записи");
        }
        const AVFrame& source = *frame.gpuImage;
        if (source.time_base.num <= 0 || source.time_base.den <= 0 || source.duration <= 0)
        {
            fail("Исходные time_base и положительная duration обязательны для MP4");
        }
        const int64_t pts = frame.timestamp->count();
        if (lastPts && pts <= *lastPts)
        {
            fail("Timestamp кадра должен строго возрастать");
        }
        std::unique_ptr<AVFrame, void (*)(AVFrame*)> encoded(av_frame_clone(&source), [](AVFrame* value) { av_frame_free(&value); });
        if (!encoded)
        {
            fail("Не удалось клонировать CUDA frame");
        }
        encoded->pts = av_rescale_q(pts, AVRational{1, 1000000}, codecContext->time_base);
        encoded->time_base = codecContext->time_base;
        encoded->pict_type = AV_PICTURE_TYPE_NONE;
        encoded->duration = av_rescale_q(source.duration, source.time_base, codecContext->time_base);
        if (encoded->duration <= 0)
        {
            fail("Duration кадра потерялась при преобразовании в encoder time_base");
        }
        const auto insertedDuration = pendingDurations.emplace(encoded->pts, encoded->duration);
        if (!insertedDuration.second)
        {
            fail("Уникальный PTS для packet duration обязателен");
        }
        const int result = avcodec_send_frame(codecContext.get(), encoded.get());
        if (result < 0)
        {
            pendingDurations.erase(insertedDuration.first);
            fail("Передача CUDA frame encoder: " + errorText(result));
        }
        drainPackets(false);
        lastPts = encoded->pts;
    }
    catch (...)
    {
        failed = true;
        throw;
    }
}

void Writer::finish()
{
    if (!initialized || failed)
    {
        throw std::logic_error("[Writer::finish] Writer не готов к завершению");
    }
    if (finished)
    {
        return;
    }
    try
    {
        if (format == Format::Csv)
        {
            output.close();
        }
        else
        {
            if (!headerWritten)
            {
                fail("Нельзя создать MP4 без кадров", "Writer::finish");
            }
            const int result = avcodec_send_frame(codecContext.get(), nullptr);
            if (result < 0 && result != AVERROR_EOF)
            {
                fail("Завершение encoder flush: " + errorText(result), "Writer::finish");
            }
            drainPackets(true);
            if (!pendingDurations.empty())
            {
                fail("Encoder не выдал packet для каждого входного кадра", "Writer::finish");
            }
            const int trailerResult = av_write_trailer(formatContext.get());
            if (trailerResult < 0)
            {
                fail("Запись MP4 trailer: " + errorText(trailerResult), "Writer::finish");
            }
            if (formatContext->pb && !(formatContext->oformat->flags & AVFMT_NOFILE))
            {
                const int closeResult = avio_closep(&formatContext->pb);
                if (closeResult < 0)
                {
                    fail("Закрытие MP4 output: " + errorText(closeResult), "Writer::finish");
                }
            }
            codecContext.reset();
            formatContext.reset();
        }
        finished = true;
        std::cout << "[Writer::finish] запись завершена\n";
    }
    catch (...)
    {
        failed = true;
        throw;
    }
}
}
