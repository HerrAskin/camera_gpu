#include "camera/video_writer.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
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

std::int64_t durationUs(const Frame& frame)
{
    if (frame.duration)
    {
        return frame.duration->count();
    }
    return frame.gpuImage ? av_rescale_q(frame.gpuImage->duration, frame.gpuImage->time_base, AVRational{1, 1000000}) : 0;
}

int swscaleMatrix(AVColorSpace colorSpace)
{
    if (colorSpace == AVCOL_SPC_BT470BG || colorSpace == AVCOL_SPC_SMPTE170M)
    {
        return SWS_CS_ITU601;
    }
    if (colorSpace == AVCOL_SPC_BT2020_NCL)
    {
        return SWS_CS_BT2020;
    }
    if (colorSpace == AVCOL_SPC_SMPTE240M)
    {
        return SWS_CS_SMPTE240M;
    }
    return SWS_CS_ITU709;
}
}

void VideoWriter::freeFormat(AVFormatContext* context)
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

void VideoWriter::freeCodec(AVCodecContext* context)
{
    avcodec_free_context(&context);
}

void VideoWriter::freePacket(AVPacket* value)
{
    av_packet_free(&value);
}

void VideoWriter::freeFrame(AVFrame* value)
{
    av_frame_free(&value);
}

void VideoWriter::freeScaler(SwsContext* value)
{
    sws_freeContext(value);
}

VideoWriter::~VideoWriter()
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

void VideoWriter::fail(const std::string& message, const char* operation)
{
    failed = true;
    throw std::runtime_error(std::string("[") + operation + "] " + message);
}

bool VideoWriter::init(const std::filesystem::path& path, Format requestedFormat, std::size_t queueDepth)
{
    Options requested;
    requested.format = requestedFormat;
    requested.queueDepth = queueDepth;
    return init(path, requested);
}

bool VideoWriter::init(const std::filesystem::path& path, const Options& requested)
{
    if (initialized)
    {
        std::cerr << "[VideoWriter::init] Ошибка: объект уже инициализирован\n";
        return false;
    }
    try
    {
        configureOutput(path, requested);
        initialized = true;
        startWorker(requested.queueDepth);
        try
        {
            std::cout << "[VideoWriter::init] инициализация успешна\n";
        }
        catch (...)
        {
            // Логирование не должно превращать уже запущенный worker в частично созданный объект.
        }
        return true;
    }
    catch (const std::exception& error)
    {
        cleanupFailedInit();
        std::cerr << "[VideoWriter::init] Ошибка инициализации: " << error.what() << '\n';
        return false;
    }
}

void VideoWriter::configureOutput(const std::filesystem::path& path, const Options& requested)
{
    basePath = path;
    options = requested;
    format = requested.format;
    if (format == Format::Csv)
    {
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(path);
        output << "timestamp_us,width,height\n";
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        paths.push_back(path);
        return;
    }
    openOutput(path);
}

void VideoWriter::openOutput(const std::filesystem::path& path)
{
    currentOutputPath = path;
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

void VideoWriter::startWorker(std::size_t queueDepth)
{
    if (queueDepth == 0)
    {
        return;
    }
    queue = std::make_unique<FrameQueue>(queueDepth);
    asynchronous = true;
    accepting = true;
    worker = std::thread(&VideoWriter::runWorker, this);
}

void VideoWriter::cleanupFailedInit()
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
    writtenGeneration.reset();
    paths.clear();
    streamGeneration.reset();
}

void VideoWriter::validateVideoFrame(const Frame& frame)
{
    if (!frame.timestamp || frame.width() <= 0 || frame.height() <= 0)
    {
        fail("Timestamp и положительные размеры обязательны для MP4");
    }
    const bool needsSourceDuration = !frame.duration || frame.duration->count() <= 0;
    if (frame.gpuImage && (frame.gpuImage->format != AV_PIX_FMT_CUDA || !frame.gpuImage->hw_frames_ctx ||
        frame.gpuImage->time_base.num <= 0 || frame.gpuImage->time_base.den <= 0 ||
        (needsSourceDuration && frame.gpuImage->duration <= 0)))
    {
        fail("CUDA кадр не содержит корректную поверхность и временные метаданные");
    }
    if (frame.gpuImage && !frame.image.empty())
    {
        fail("Для MP4 кадр должен содержать только одно представление изображения");
    }
    if (!frame.gpuImage && frame.image.type() != CV_8UC3)
    {
        fail("CPU кадр для MP4 должен иметь формат BGR CV_8UC3");
    }
    if (durationUs(frame) <= 0)
    {
        fail("Для MP4 обязательна положительная duration");
    }
    if (headerWritten && (frame.width() != encoderContext->width || frame.height() != encoderContext->height))
    {
        fail("Размер кадра изменился во время записи сегмента");
    }
    if (headerWritten && gpuEncoder && (!frame.gpuImage || !frame.gpuImage->hw_frames_ctx ||
        !encoderContext->hw_frames_ctx || frame.gpuImage->hw_frames_ctx->buffer != encoderContext->hw_frames_ctx->buffer))
    {
        fail("CUDA hw_frames_ctx изменился во время записи сегмента");
    }
}

void VideoWriter::openEncoder(const Frame& frame)
{
    if (options.preferGpu && frame.gpuImage)
    {
        try
        {
            openGpuEncoder(frame);
            return;
        }
        catch (const std::exception& error)
        {
            std::cerr << "[VideoWriter] NVENC недоступен, переход на CPU: " << error.what() << '\n';
        }
    }
    openCpuEncoder(frame);
    std::cerr << "[VideoWriter] Выбран программный H.264 encoder " << encoderContext->codec->name << "; выход: " << currentOutputPath << '\n';
}

void VideoWriter::openGpuEncoder(const Frame& frame)
{
    const AVFrame& source = *frame.gpuImage;
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec)
    {
        throw std::runtime_error("encoder h264_nvenc не найден");
    }
    auto candidate = std::unique_ptr<AVCodecContext, decltype(&freeCodec)>(avcodec_alloc_context3(codec), &freeCodec);
    if (!candidate)
    {
        throw std::runtime_error("не удалось выделить NVENC context");
    }
    configureEncoder(candidate.get(), frame, AV_PIX_FMT_CUDA);
    candidate->color_range = source.color_range;
    candidate->colorspace = source.colorspace;
    candidate->color_trc = source.color_trc;
    candidate->color_primaries = source.color_primaries;
    candidate->hw_frames_ctx = av_buffer_ref(source.hw_frames_ctx);
    if (!candidate->hw_frames_ctx)
    {
        throw std::runtime_error("не удалось передать CUDA frames context");
    }
    const int result = avcodec_open2(candidate.get(), codec, nullptr);
    if (result < 0)
    {
        throw std::runtime_error(errorText(result));
    }
    encoderContext = std::move(candidate);
    gpuEncoder = true;
    std::cout << "[VideoWriter] Выбран NVENC H.264 encoder; выход: " << currentOutputPath << '\n';
}

void VideoWriter::openCpuEncoder(const Frame& frame)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
    {
        throw std::runtime_error("software encoder libx264 недоступен");
    }
    auto candidate = std::unique_ptr<AVCodecContext, decltype(&freeCodec)>(avcodec_alloc_context3(codec), &freeCodec);
    if (!candidate)
    {
        fail("Не удалось выделить CPU encoder context");
    }
    configureEncoder(candidate.get(), frame, AV_PIX_FMT_YUV420P);
    candidate->color_range = frame.gpuImage && frame.gpuImage->color_range != AVCOL_RANGE_UNSPECIFIED
        ? frame.gpuImage->color_range : AVCOL_RANGE_MPEG;
    candidate->colorspace = frame.gpuImage && frame.gpuImage->colorspace != AVCOL_SPC_UNSPECIFIED
        ? frame.gpuImage->colorspace : AVCOL_SPC_BT709;
    candidate->color_trc = frame.gpuImage && frame.gpuImage->color_trc != AVCOL_TRC_UNSPECIFIED
        ? frame.gpuImage->color_trc : AVCOL_TRC_BT709;
    candidate->color_primaries = frame.gpuImage && frame.gpuImage->color_primaries != AVCOL_PRI_UNSPECIFIED
        ? frame.gpuImage->color_primaries : AVCOL_PRI_BT709;
    const int result = avcodec_open2(candidate.get(), codec, nullptr);
    if (result < 0)
    {
        fail("Открытие software H.264 encoder: " + errorText(result));
    }
    encoderContext = std::move(candidate);
    gpuEncoder = false;
}

void VideoWriter::configureEncoder(AVCodecContext* context, const Frame& frame, int pixelFormat)
{
    context->width = frame.width();
    context->height = frame.height();
    context->pix_fmt = static_cast<AVPixelFormat>(pixelFormat);
    context->time_base = encoderTimeBase;
    context->framerate = av_d2q(1000000.0 / durationUs(frame), 1000000);
    context->bit_rate = 2000000;
    context->max_b_frames = 0;
    context->sample_aspect_ratio = frame.gpuImage ? frame.gpuImage->sample_aspect_ratio : AVRational{1, 1};
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER)
    {
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
}

void VideoWriter::writeHeader()
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
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (paths.empty() || paths.back() != currentOutputPath)
        {
            paths.push_back(currentOutputPath);
        }
    }
}

void VideoWriter::drainPackets(bool flushing)
{
    AVStream* stream = formatContext->streams[0];
    const char* operation = flushing ? "VideoWriter::finish" : "VideoWriter::write";
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

void VideoWriter::writeCsv(const Frame& frame)
{
    if (frame.timestamp)
    {
        output << frame.timestamp->count();
    }
    output << ',' << frame.width() << ',' << frame.height() << '\n';
}

std::unique_ptr<AVFrame, decltype(&VideoWriter::freeFrame)> VideoWriter::prepareEncoderFrame(const Frame& frame)
{
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
    if (!gpuEncoder)
    {
        return prepareCpuFrame(frame);
    }
    std::unique_ptr<AVFrame, decltype(&freeFrame)> encoded(av_frame_clone(frame.gpuImage.get()), &freeFrame);
    if (!encoded)
    {
        fail("Не удалось клонировать CUDA frame");
    }
    // clone разделяет буферы пикселей по refcount; меняем только метаданные копии кадра.
    encoded->pts = outputPts;
    encoded->time_base = encoderContext->time_base;
    encoded->pict_type = AV_PICTURE_TYPE_NONE;
    const auto duration = durationUs(frame);
    encoded->duration = duration;
    if (encoded->duration <= 0)
    {
        fail("Duration кадра потерялась при преобразовании в encoder time_base");
    }
    return encoded;
}

std::unique_ptr<AVFrame, decltype(&VideoWriter::freeFrame)> VideoWriter::prepareCpuFrame(const Frame& frame)
{
    auto encoded = std::unique_ptr<AVFrame, decltype(&freeFrame)>(av_frame_alloc(), &freeFrame);
    if (!encoded)
    {
        fail("Не удалось выделить CPU encoder frame");
    }
    encoded->format = AV_PIX_FMT_YUV420P;
    encoded->width = frame.width();
    encoded->height = frame.height();
    const auto inputPts = av_rescale_q(frame.timestamp->count(), AVRational{1, 1000000}, encoderContext->time_base);
    if (lastOutputPts && *lastOutputPts == std::numeric_limits<std::int64_t>::max())
    {
        fail("Невозможно скорректировать timestamp после максимального PTS");
    }
    encoded->pts = lastOutputPts && inputPts <= *lastOutputPts ? *lastOutputPts + 1 : inputPts;
    encoded->duration = av_rescale_q(durationUs(frame), AVRational{1, 1000000}, encoderContext->time_base);
    encoded->time_base = encoderContext->time_base;
    encoded->color_range = encoderContext->color_range;
    encoded->colorspace = encoderContext->colorspace;
    encoded->color_trc = encoderContext->color_trc;
    encoded->color_primaries = encoderContext->color_primaries;
    encoded->sample_aspect_ratio = encoderContext->sample_aspect_ratio;
    const int result = av_frame_get_buffer(encoded.get(), 32);
    if (result < 0)
    {
        fail("Выделение YUV frame: " + errorText(result));
    }
    convertCpuFrame(frame, encoded.get());
    encoded->pict_type = AV_PICTURE_TYPE_NONE;
    return encoded;
}

void VideoWriter::convertCpuFrame(const Frame& frame, AVFrame* target)
{
    std::unique_ptr<AVFrame, decltype(&freeFrame)> transferred(nullptr, &freeFrame);
    const AVFrame* source = nullptr;
    if (frame.gpuImage)
    {
        transferred.reset(av_frame_alloc());
        if (!transferred)
        {
            fail("Не удалось выделить CPU transfer frame");
        }
        const int transfer = av_hwframe_transfer_data(transferred.get(), frame.gpuImage.get(), 0);
        if (transfer < 0)
        {
            fail("CUDA to CPU transfer: " + errorText(transfer));
        }
        const int props = av_frame_copy_props(transferred.get(), frame.gpuImage.get());
        if (props < 0)
        {
            fail("Копирование метаданных CUDA кадра: " + errorText(props));
        }
        source = transferred.get();
    }
    if (source)
    {
        scaler.reset(sws_getCachedContext(scaler.release(), frame.width(), frame.height(), static_cast<AVPixelFormat>(source->format),
            frame.width(), frame.height(), AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr));
    }
    else
    {
        scaler.reset(sws_getCachedContext(scaler.release(), frame.width(), frame.height(), AV_PIX_FMT_BGR24,
            frame.width(), frame.height(), AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr));
    }
    if (!scaler)
    {
        fail("Не удалось создать CPU scaler");
    }
    const int sourceRange = source && source->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    const int destinationRange = encoderContext->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    const auto sourceColorSpace = source ? source->colorspace : AVCOL_SPC_BT709;
    const int sourceMatrix = swscaleMatrix(sourceColorSpace);
    const int destinationMatrix = swscaleMatrix(encoderContext->colorspace);
    const int colorResult = sws_setColorspaceDetails(scaler.get(), sws_getCoefficients(sourceMatrix), sourceRange,
        sws_getCoefficients(destinationMatrix), destinationRange, 0, 1 << 16, 1 << 16);
    if (colorResult < 0)
    {
        fail("Не удалось задать цветовую матрицу CPU scaler");
    }
    const std::uint8_t* sourceData[4]{};
    int sourceLines[4]{};
    if (source)
    {
        for (int i = 0; i < 4; ++i)
        {
            sourceData[i] = source->data[i];
            sourceLines[i] = source->linesize[i];
        }
    }
    else
    {
        sourceData[0] = frame.image.data;
        sourceLines[0] = static_cast<int>(frame.image.step);
    }
    const int rows = sws_scale(scaler.get(), sourceData, sourceLines, 0, frame.height(), target->data, target->linesize);
    if (rows != frame.height())
    {
        fail("CPU colorspace conversion returned incomplete frame");
    }
}

void VideoWriter::writeVideo(const Frame& frame)
{
    if (streamGeneration && frame.streamGeneration != *streamGeneration)
    {
        closeSegment();
        beginSegment(frame);
    }
    else if (!streamGeneration && frame.streamGeneration > 0)
    {
        closeSegment();
        beginSegment(frame);
    }
    else if (!streamGeneration)
    {
        streamGeneration = frame.streamGeneration;
    }
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

void VideoWriter::writeInternal(const Frame& frame)
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

void VideoWriter::runWorker() noexcept
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
                        writtenGeneration = frame->streamGeneration;
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

void VideoWriter::closeWorker()
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

void VideoWriter::write(const Frame& frame)
{
    if (asynchronous)
    {
        enqueueFrame(frame);
        return;
    }
    writeDirectFrame(frame);
}

void VideoWriter::enqueueFrame(const Frame& frame)
{
    std::unique_lock<std::mutex> lock(lifecycleMutex);
    rethrowWorkerError();
    rethrowAdmissionError();
    if (!accepting)
    {
        throw std::logic_error("[VideoWriter::write] VideoWriter не готов к записи");
    }
    try
    {
        if (!queue->tryPush(Frame(frame)))
        {
            admissionError = std::make_exception_ptr(std::runtime_error(
                "[VideoWriter::write] Очередь VideoWriter переполнена; прием кадров закрыт"));
            accepting = false;
            queue->close();
            rethrowAdmissionError();
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
    waitForFirstFrame(lock, frame.streamGeneration);
}

void VideoWriter::waitForFirstFrame(std::unique_lock<std::mutex>& lock, std::uint64_t generation)
{
    if (format != Format::H264Mp4 || writtenGeneration == generation)
    {
        return;
    }
    // Каждый сегмент требует прогрева encoder; ожидание отпускает lifecycleMutex.
    lifecycleCondition.wait(lock, [this, generation]
    {
        return writtenGeneration == generation || workerException;
    });
}

void VideoWriter::writeDirectFrame(const Frame& frame)
{
    if (!initialized || failed || finished)
    {
        throw std::logic_error("[VideoWriter::write] VideoWriter не готов к записи");
    }
    writeInternal(frame);
}

void VideoWriter::finishVideo()
{
    if (!headerWritten)
    {
        fail("Нельзя создать MP4 без кадров", "VideoWriter::finish");
    }
    // Null frame завершает encoder; затем вычитываем все пакеты перед записью MP4 trailer.
    const int result = avcodec_send_frame(encoderContext.get(), nullptr);
    if (result < 0 && result != AVERROR_EOF)
    {
        fail("Завершение encoder flush: " + errorText(result), "VideoWriter::finish");
    }
    drainPackets(true);
    if (!pendingFrameDurations.empty())
    {
        fail("Encoder не выдал packet для каждого входного кадра", "VideoWriter::finish");
    }
    const int trailerResult = av_write_trailer(formatContext.get());
    if (trailerResult < 0)
    {
        fail("Запись MP4 trailer: " + errorText(trailerResult), "VideoWriter::finish");
    }
    if (formatContext->pb && !(formatContext->oformat->flags & AVFMT_NOFILE))
    {
        const int closeResult = avio_closep(&formatContext->pb);
        if (closeResult < 0)
        {
            fail("Закрытие MP4 output: " + errorText(closeResult), "VideoWriter::finish");
        }
    }
    encoderContext.reset();
    formatContext.reset();
    scaler.reset();
}

std::filesystem::path VideoWriter::segmentPath(std::uint64_t generation) const
{
    const auto stem = basePath.stem().string() + "_session" + std::to_string(generation + 1);
    const auto extension = basePath.extension().empty() ? std::string(".mp4") : basePath.extension().string();
    std::filesystem::path candidate = basePath.parent_path() / (stem + extension);
    std::uint64_t suffix = 2;
    while (std::filesystem::exists(candidate))
        candidate = basePath.parent_path() / (stem + "_" + std::to_string(suffix++) + extension);
    return candidate;
}

void VideoWriter::beginSegment(const Frame& frame)
{
    resetSegment();
    const auto path = segmentPath(frame.streamGeneration);
    openOutput(path);
    streamGeneration = frame.streamGeneration;
}

void VideoWriter::closeSegment()
{
    if (headerWritten)
    {
        finishVideo();
    }
    else
    {
        encoderContext.reset();
        formatContext.reset();
        scaler.reset();
    }
    resetSegment();
}

void VideoWriter::resetSegment()
{
    headerWritten = false;
    lastOutputPts.reset();
    pendingFrameDurations.clear();
    correctedTimestamps = 0;
}

std::vector<std::filesystem::path> VideoWriter::outputPaths() const
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return paths;
}

void VideoWriter::finish()
{
    if (asynchronous)
    {
        closeWorker();
    }
    rethrowWorkerError();
    if (!initialized || failed)
    {
        throw std::logic_error("[VideoWriter::finish] VideoWriter не готов к завершению");
    }
    if (finished)
    {
        rethrowAdmissionError();
        return;
    }
    try
    {
        finishOutput();
        reportFinished();
    }
    catch (...)
    {
        failed = true;
        throw;
    }
    rethrowAdmissionError();
}

void VideoWriter::finishOutput()
{
    if (format == Format::Csv)
    {
        output.close();
        return;
    }
    finishVideo();
}

void VideoWriter::reportFinished()
{
    finished = true;
    if (format == Format::H264Mp4 && correctedTimestamps > 0)
    {
        std::cout << "[VideoWriter::finish] Скорректировано timestamps: " << correctedTimestamps << '\n';
    }
    std::cout << "[VideoWriter::finish] запись завершена\n";
}

void VideoWriter::rethrowWorkerError() const
{
    if (workerException)
    {
        std::rethrow_exception(workerException);
    }
}

void VideoWriter::rethrowAdmissionError() const
{
    if (admissionError)
    {
        std::rethrow_exception(admissionError);
    }
}
}
