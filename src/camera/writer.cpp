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
#include <limits>
#include <stdexcept>
#include <utility>

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

void Writer::freeFrame(AVFrame* value)
{
    av_frame_free(&value);
}

Writer::~Writer()
{
    try
    {
        closeWorker();
    }
    catch (...)
    {
        // Деструктор не выпускает исключения; штатный join возможен только из потока-владельца.
    }
}

void Writer::fail(const std::string& message, const char* operation)
{
    failed = true;
    throw std::runtime_error(std::string("[") + operation + "] " + message);
}

bool Writer::init(const std::filesystem::path& path, Format requestedFormat, std::size_t queueDepth)
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
        if (queueDepth > 0)
        {
            queue = std::make_unique<FrameQueue>(queueDepth);
            asynchronous = true;
            accepting = true;
            worker = std::thread(&Writer::runWorker, this);
        }
        try
        {
            std::cout << "[Writer::init] инициализация успешна\n";
        }
        catch (...)
        {
            // Логирование не должно превращать уже запущенный worker в частично созданный объект.
        }
        return true;
    }
    catch (const std::exception& error)
    {
        output.exceptions(std::ios::goodbit);
        output.close();
        output.clear();
        formatContext.reset();
        encoderContext.reset();
        if (queue)
        {
            queue->close();
            queue.reset();
        }
        asynchronous = false;
        accepting = false;
        initialized = false;
        headerWritten = false;
        finished = false;
        failed = false;
        workerException = nullptr;
        admissionError = nullptr;
        firstVideoFrameWritten = false;
        std::cerr << "[Writer::init] Ошибка инициализации: " << error.what() << '\n';
        return false;
    }
}

void Writer::validateVideoFrame(const Frame& frame)
{
    if (!frame.gpuImage || !frame.image.empty() || frame.gpuImage->format != AV_PIX_FMT_CUDA || !frame.gpuImage->hw_frames_ctx)
    {
        fail("H264 MP4 требует CUDA frame с hw_frames_ctx");
    }
    if (!frame.timestamp || frame.width() <= 0 || frame.height() <= 0 || frame.gpuImage->time_base.num <= 0 ||
        frame.gpuImage->time_base.den <= 0 || frame.gpuImage->duration <= 0)
    {
        fail("Timestamp, положительные размеры, исходные time_base и duration обязательны для MP4");
    }
    if (headerWritten && (frame.width() != encoderContext->width || frame.height() != encoderContext->height ||
        frame.gpuImage->hw_frames_ctx->buffer != encoderContext->hw_frames_ctx->buffer))
    {
        fail("Размер или CUDA hw_frames_ctx изменились во время записи");
    }
}

void Writer::openEncoder(const Frame& frame)
{
    // Encoder настраивается по первому кадру: размеры и CUDA frame pool задают постоянную конфигурацию.
    const AVFrame& source = *frame.gpuImage;
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
    encoderContext.reset(avcodec_alloc_context3(encoder));
    if (!encoderContext)
    {
        fail("Не удалось выделить encoder context");
    }
    encoderContext->width = frame.width();
    encoderContext->height = frame.height();
    encoderContext->pix_fmt = AV_PIX_FMT_CUDA;
    encoderContext->time_base = encoderTimeBase;
    // FPS нужен encoder/VUI; кадры сохраняют собственные PTS и duration.
    encoderContext->framerate = nominalFrameRate;
    encoderContext->bit_rate = 2000000;
    encoderContext->max_b_frames = 0;
    encoderContext->color_range = source.color_range;
    encoderContext->colorspace = source.colorspace;
    encoderContext->color_trc = source.color_trc;
    encoderContext->color_primaries = source.color_primaries;
    encoderContext->sample_aspect_ratio = source.sample_aspect_ratio;
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER)
    {
        encoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    encoderContext->hw_frames_ctx = av_buffer_ref(source.hw_frames_ctx);
    if (!encoderContext->hw_frames_ctx)
    {
        fail("Не удалось передать CUDA hw_frames_ctx encoder");
    }
    const int result = avcodec_open2(encoderContext.get(), encoder, nullptr);
    if (result < 0)
    {
        fail("Открытие h264_nvenc: " + errorText(result));
    }
}

void Writer::writeHeader()
{
    AVStream* stream = avformat_new_stream(formatContext.get(), nullptr);
    if (!stream)
    {
        fail("Не удалось создать MP4 video stream");
    }
    stream->time_base = encoderContext->time_base;
    int result = avcodec_parameters_from_context(stream->codecpar, encoderContext.get());
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
        const int result = avcodec_receive_packet(encoderContext.get(), packet.get());
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
        const auto duration = pendingFrameDurations.find(packet->pts);
        if (duration == pendingFrameDurations.end())
        {
            fail("Encoded packet не соответствует ожидаемому PTS", operation);
        }
        const int64_t packetDuration = duration->second;
        pendingFrameDurations.erase(duration);
        // Muxer вправе изменить stream time_base после заголовка, поэтому временные поля
        // packet и сохранённую duration переводим в фактический time_base потока.
        av_packet_rescale_ts(packet.get(), encoderContext->time_base, stream->time_base);
        packet->duration = av_rescale_q(packetDuration, encoderContext->time_base, stream->time_base);
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

void Writer::writeCsv(const Frame& frame)
{
    if (frame.timestamp)
    {
        output << frame.timestamp->count();
    }
    output << ',' << frame.width() << ',' << frame.height() << '\n';
}

std::unique_ptr<AVFrame, decltype(&Writer::freeFrame)> Writer::prepareEncoderFrame(const Frame& frame)
{
    const AVFrame& source = *frame.gpuImage;
    const std::int64_t inputPts = av_rescale_q(frame.timestamp->count(), AVRational{1, 1000000}, encoderContext->time_base);
    std::int64_t outputPts = inputPts;
    if (lastOutputPts && outputPts <= *lastOutputPts)
    {
        if (*lastOutputPts == std::numeric_limits<std::int64_t>::max())
        {
            fail("Невозможно скорректировать timestamp после максимального PTS");
        }
        // Клонируем кадр, чтобы сделать PTS строго возрастающим и оставить входной Frame неизменным.
        outputPts = *lastOutputPts + 1;
    }
    std::unique_ptr<AVFrame, decltype(&freeFrame)> encoded(av_frame_clone(&source), &freeFrame);
    if (!encoded)
    {
        fail("Не удалось клонировать CUDA frame");
    }
    // clone разделяет буферы пикселей по refcount; меняем только метаданные копии кадра.
    encoded->pts = outputPts;
    encoded->time_base = encoderContext->time_base;
    encoded->pict_type = AV_PICTURE_TYPE_NONE;
    encoded->duration = av_rescale_q(source.duration, source.time_base, encoderContext->time_base);
    if (encoded->duration <= 0)
    {
        fail("Duration кадра потерялась при преобразовании в encoder time_base");
    }
    return encoded;
}

void Writer::writeVideo(const Frame& frame)
{
    validateVideoFrame(frame);
    if (!headerWritten)
    {
        openEncoder(frame);
        writeHeader();
    }
    auto encoded = prepareEncoderFrame(frame);
    if (!pendingFrameDurations.emplace(encoded->pts, encoded->duration).second)
    {
        fail("Уникальный PTS для packet duration обязателен");
    }
    const int result = avcodec_send_frame(encoderContext.get(), encoded.get());
    if (result < 0)
    {
        pendingFrameDurations.erase(encoded->pts);
        fail("Передача CUDA frame encoder: " + errorText(result));
    }
    drainPackets(false);
    lastOutputPts = encoded->pts;
    if (encoded->pts != av_rescale_q(frame.timestamp->count(), AVRational{1, 1000000}, encoderContext->time_base))
    {
        ++correctedTimestamps;
    }
}

void Writer::writeInternal(const Frame& frame)
{
    if ((!frame.gpuImage && frame.image.empty()) || frame.width() <= 0 || frame.height() <= 0)
    {
        fail("Для записи требуется непустое изображение");
    }
    try
    {
        if (format == Format::Csv)
        {
            writeCsv(frame);
        }
        else
        {
            writeVideo(frame);
        }
    }
    catch (...)
    {
        failed = true;
        throw;
    }
}

void Writer::runWorker() noexcept
{
    try
    {
        while (true)
        {
            auto frame = queue->pop(std::chrono::milliseconds(100));
            if (frame)
            {
                writeInternal(*frame);
                if (format == Format::H264Mp4)
                {
                    {
                        std::lock_guard<std::mutex> lock(lifecycleMutex);
                        firstVideoFrameWritten = true;
                    }
                    lifecycleCondition.notify_all();
                }
                continue;
            }
            // Последний push и close могли произойти после timeout pop: сначала забираем остаток.
            if (queue->isClosed() && queue->size() == 0)
            {
                return;
            }
        }
    }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            workerException = std::current_exception();
            accepting = false;
            queue->close();
        }
        lifecycleCondition.notify_all();
    }
}

void Writer::closeWorker()
{
    if (queue)
    {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            accepting = false;
            queue->close();
        }
    }
    if (worker.joinable())
    {
        worker.join();
    }
}

void Writer::write(const Frame& frame)
{
    if (asynchronous)
    {
        std::unique_lock<std::mutex> lock(lifecycleMutex);
        if (workerException)
        {
            std::rethrow_exception(workerException);
        }
        if (admissionError)
        {
            std::rethrow_exception(admissionError);
        }
        if (!accepting)
        {
            throw std::logic_error("[Writer::write] Writer не готов к записи");
        }
        try
        {
            if (!queue->tryPush(Frame(frame)))
            {
                admissionError = std::make_exception_ptr(std::runtime_error("[Writer::write] Очередь Writer переполнена; прием кадров закрыт"));
                accepting = false;
                queue->close();
                std::rethrow_exception(admissionError);
            }
        }
        catch (...)
        {
            if (!admissionError)
            {
                admissionError = std::current_exception();
                accepting = false;
                queue->close();
            }
            throw;
        }
        if (format == Format::H264Mp4 && !firstVideoFrameWritten)
        {
            // Первый кадр открывает NVENC/muxer; дождаться именно его, чтобы startup не забил bounded FIFO.
            lifecycleCondition.wait(lock, [this]
            {
                return firstVideoFrameWritten || workerException;
            });
        }
        return;
    }
    if (!initialized || failed || finished)
    {
        throw std::logic_error("[Writer::write] Writer не готов к записи");
    }
    writeInternal(frame);
}

void Writer::finishVideo()
{
    if (!headerWritten)
    {
        fail("Нельзя создать MP4 без кадров", "Writer::finish");
    }
    // Null frame завершает encoder; затем вычитываем все пакеты перед записью MP4 trailer.
    const int result = avcodec_send_frame(encoderContext.get(), nullptr);
    if (result < 0 && result != AVERROR_EOF)
    {
        fail("Завершение encoder flush: " + errorText(result), "Writer::finish");
    }
    drainPackets(true);
    if (!pendingFrameDurations.empty())
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
    encoderContext.reset();
    formatContext.reset();
}

void Writer::finish()
{
    if (asynchronous)
    {
        closeWorker();
        if (workerException)
        {
            std::rethrow_exception(workerException);
        }
    }
    if (!initialized || failed)
    {
        throw std::logic_error("[Writer::finish] Writer не готов к завершению");
    }
    if (finished)
    {
        if (asynchronous && admissionError)
        {
            std::rethrow_exception(admissionError);
        }
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
            finishVideo();
        }
        finished = true;
        if (format == Format::H264Mp4 && correctedTimestamps > 0)
        {
            std::cout << "[Writer::finish] Скорректировано timestamps: " << correctedTimestamps << '\n';
        }
        std::cout << "[Writer::finish] запись завершена\n";
    }
    catch (...)
    {
        failed = true;
        throw;
    }
    if (asynchronous && admissionError)
    {
        std::rethrow_exception(admissionError);
    }
}
}
