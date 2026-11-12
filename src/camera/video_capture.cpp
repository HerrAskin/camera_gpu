#include "camera/video_capture.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

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

void checkIo(int result, const char* operation, const std::atomic<bool>& stopped, std::chrono::steady_clock::time_point deadline)
{
    if (result >= 0)
    {
        return;
    }
    if (stopped.load(std::memory_order_relaxed))
    {
        throw std::runtime_error(std::string(operation) + ": запрошена остановка");
    }
    if (std::chrono::steady_clock::now() >= deadline || result == AVERROR_EXIT)
    {
        throw std::runtime_error(std::string(operation) + ": I/O прервано или истёк срок ожидания");
    }
    check(result, operation);
}

void checkInterrupted(const char* operation, const std::atomic<bool>& stopped, std::chrono::steady_clock::time_point deadline)
{
    if (stopped.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
    {
        checkIo(AVERROR_EXIT, operation, stopped, deadline);
    }
}
}

AVPixelFormat VideoCapture::selectFormat(AVCodecContext* context, const AVPixelFormat* formats)
{
    auto* capture = static_cast<VideoCapture*>(context->opaque);
    for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format)
    {
        if (*format == AV_PIX_FMT_CUDA && context->hw_device_ctx)
        {
            return *format;
        }
    }
    if (context->hw_device_ctx)
    {
        for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format)
        {
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
            if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
            {
                av_buffer_unref(&context->hw_device_ctx);
                if (capture)
                {
                    capture->cudaDevice.reset();
                    capture->gpuActive = false;
                }
                std::cerr << "[VideoCapture] CUDA decoder не инициализировался, переключаюсь на CPU" << std::endl;
                return *format;
            }
        }
        return AV_PIX_FMT_NONE;
    }
    for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format)
    {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
        {
            return *format;
        }
    }
    return formats[0];
}

int VideoCapture::interruptIo(void* opaque) noexcept
{
    auto* capture = static_cast<VideoCapture*>(opaque);
    return capture->stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= capture->deadline;
}

void VideoCapture::closeInput(AVFormatContext* context)
{
    avformat_close_input(&context);
}
void VideoCapture::releaseDevice(AVBufferRef* device)
{
    av_buffer_unref(&device);
}
void VideoCapture::closeDecoder(AVCodecContext* decoder)
{
    avcodec_free_context(&decoder);
}
void VideoCapture::freeScaler(SwsContext* context)
{
    sws_freeContext(context);
}

VideoCapture::~VideoCapture()
{
    requestStop();
    if (readerThread.joinable())
    {
        readerThread.join();
    }
}

void VideoCapture::requestStop() noexcept
{
    stopRequested.store(true, std::memory_order_relaxed);
}

void VideoCapture::beginIo() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    const auto left = std::chrono::steady_clock::time_point::max() - now;
    deadline = options.ioTimeout >= std::chrono::duration_cast<std::chrono::milliseconds>(left)
        ? std::chrono::steady_clock::time_point::max() : now + options.ioTimeout;
}

void VideoCapture::openInput()
{
    formatContext.reset(avformat_alloc_context());
    if (!formatContext)
    {
        throw std::bad_alloc();
    }
    formatContext->interrupt_callback = {interruptIo, this};
    AVDictionary* dictionary = nullptr;
    if (liveInput)
    {
        check(av_dict_set(&dictionary, "rtsp_transport", "tcp", 0), "Настройка RTSP transport");
        const auto max = std::numeric_limits<std::int64_t>::max();
        const auto millis = options.ioTimeout.count();
        const auto usec = millis > max / 1000 ? max : millis * 1000;
        const auto value = std::to_string(usec);
        check(av_dict_set(&dictionary, "timeout", value.c_str(), 0), "Настройка RTSP timeout");
    }
    beginIo();
    AVFormatContext* raw = formatContext.release();
    const int result = avformat_open_input(&raw, source.c_str(), nullptr, &dictionary);
    av_dict_free(&dictionary);
    formatContext.reset(raw);
    checkIo(result, "Открытие источника", stopRequested, deadline);
    beginIo();
    checkIo(avformat_find_stream_info(formatContext.get(), nullptr), "Чтение информации о потоках", stopRequested, deadline);
    streamIndex = av_find_best_stream(formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(streamIndex, "Поиск видеопотока");
    const AVStream* stream = formatContext->streams[streamIndex];
    frameRate = av_guess_frame_rate(formatContext.get(), const_cast<AVStream*>(stream), nullptr);
}

void VideoCapture::openDecoder()
{
    const auto* stream = formatContext->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
    {
        throw std::runtime_error("Decoder для видеопотока не найден");
    }
    if (options.preferGpu && hasCudaDecoder(codec))
    {
        try
        {
            AVBufferRef* device = nullptr;
            check(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, "0", nullptr, 0), "Создание CUDA-контекста");
            cudaDevice.reset(device);
            gpuActive = true;
            configureDecoder(codec, true);
            return;
        }
        catch (const std::exception& error)
        {
            decoderContext.reset();
            cudaDevice.reset();
            gpuActive = false;
            std::cerr << "[VideoCapture] CUDA недоступна, используется CPU decoder: " << error.what() << '\n';
        }
    }
    configureDecoder(codec, false);
}

bool VideoCapture::hasCudaDecoder(const AVCodec* codec) const noexcept
{
    for (int index = 0; const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index); ++index)
    {
        if (config->device_type == AV_HWDEVICE_TYPE_CUDA && config->pix_fmt == AV_PIX_FMT_CUDA &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
        {
            return true;
        }
    }
    return false;
}

void VideoCapture::configureDecoder(const AVCodec* codec, bool useGpu)
{
    const AVStream* stream = formatContext->streams[streamIndex];
    decoderContext.reset(avcodec_alloc_context3(codec));
    if (!decoderContext)
    {
        throw std::bad_alloc();
    }
    check(avcodec_parameters_to_context(decoderContext.get(), stream->codecpar), "Передача параметров codec");
    decoderContext->pkt_timebase = stream->time_base;
    decoderContext->opaque = this;
    decoderContext->get_format = selectFormat;
    decoderContext->extra_hw_frames = static_cast<int>(options.queueDepth + options.additionalHeldFrames + 2);
    if (useGpu)
    {
        decoderContext->hw_device_ctx = av_buffer_ref(cudaDevice.get());
        if (!decoderContext->hw_device_ctx)
        {
            throw std::bad_alloc();
        }
    }
    check(avcodec_open2(decoderContext.get(), codec, nullptr), "Открытие decoder");
}

void VideoCapture::prepareAttempt(unsigned attemptGeneration)
{
    generation = attemptGeneration;
    resetAttempt();
    openInput();
    openDecoder();
    draining = false;
    ended = false;
    std::ostringstream text;
    const auto* stream = formatContext->streams[streamIndex];
    const auto* codec = stream->codecpar;
    text << "Видеопоток: " << streamIndex << '\n' << "Codec: " << avcodec_get_name(codec->codec_id) << '\n'
         << "Размер: " << codec->width << 'x' << codec->height << '\n'
         << "Time base: " << stream->time_base.num << '/' << stream->time_base.den << '\n'
         << (gpuActive ? "CUDA decoder" : "CPU decoder") << ": " << decoderContext->codec->name;
    {
        std::lock_guard<std::mutex> lock(readerErrorMutex);
        cachedDescription = text.str();
    }
}

void VideoCapture::resetAttempt() noexcept
{
    scaler.reset();
    decoderContext.reset();
    cudaDevice.reset();
    formatContext.reset();
    streamIndex = -1;
    gpuActive = false;
    draining = false;
    ended = false;
}

bool VideoCapture::init(const std::string& sourceValue, std::chrono::milliseconds timeout, std::size_t depth, std::size_t held)
{
    Options value;
    value.ioTimeout = timeout;
    value.queueDepth = depth;
    value.additionalHeldFrames = held;
    return init(sourceValue, value);
}

bool VideoCapture::init(const std::string& sourceValue, const Options& value)
{
    if (!validateOptions(value))
    {
        std::cerr << "[VideoCapture::init] Некорректные параметры или объект уже инициализирован\n";
        return false;
    }
    source = sourceValue;
    options = value;
    liveInput = source.rfind("rtsp://", 0) == 0;
    stopRequested.store(false, std::memory_order_relaxed);
    frameQueue = std::make_unique<FrameQueue>(value.queueDepth);
    {
        std::lock_guard<std::mutex> lock(readerErrorMutex);
        readerError = nullptr;
    }
    try
    {
        prepareAttempt(0);
    }
    catch (const std::exception& error)
    {
        resetAttempt();
        if (!liveInput || value.reconnectAttempts == 0)
        {
            frameQueue.reset();
            std::cerr << "[VideoCapture::init] Ошибка инициализации: " << error.what() << '\n';
            return false;
        }
        std::cerr << "[VideoCapture::init] Первое подключение не удалось; запущу переподключение: " << error.what() << '\n';
    }
    initialized = true;
    if (!startReader())
    {
        initialized = false;
        resetAttempt();
        frameQueue.reset();
        return false;
    }
    return true;
}

bool VideoCapture::validateOptions(const Options& value) const noexcept
{
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max() - 2);
    return !initialized && !readerThread.joinable() && value.ioTimeout > std::chrono::milliseconds::zero() &&
        value.queueDepth > 0 && value.queueDepth <= maximum && value.additionalHeldFrames <= maximum - value.queueDepth;
}

bool VideoCapture::startReader()
{
    try
    {
        readerThread = std::thread(&VideoCapture::readerLoop, this);
        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[VideoCapture::init] Не удалось запустить worker: " << error.what() << '\n';
        return false;
    }
}

void VideoCapture::feedDecoder(AVPacket* packet)
{
    int result = 0;
    while (true)
    {
        if (stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
        {
            checkIo(AVERROR_EXIT, "Чтение packet", stopRequested, deadline);
        }
        result = av_read_frame(formatContext.get(), packet);
        if (result < 0 || packet->stream_index == streamIndex)
        {
            break;
        }
        av_packet_unref(packet);
    }
    if (result == AVERROR_EOF)
    {
        check(avcodec_send_packet(decoderContext.get(), nullptr), "Flush decoder");
        draining = true;
        return;
    }
    checkIo(result, "Чтение packet", stopRequested, deadline);
    const int sent = avcodec_send_packet(decoderContext.get(), packet);
    av_packet_unref(packet);
    check(sent, "Передача packet decoder");
}

Frame VideoCapture::makeCpuFrame(std::shared_ptr<AVFrame> decoded)
{
    Frame result;
    result.image.create(decoded->height, decoded->width, CV_8UC3);
    auto* context = sws_getCachedContext(scaler.release(), decoded->width, decoded->height, static_cast<AVPixelFormat>(decoded->format),
        decoded->width, decoded->height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!context)
    {
        throw std::runtime_error("Не удалось создать swscale context");
    }
    scaler.reset(context);
    std::uint8_t* destination[] = {result.image.data, nullptr, nullptr, nullptr};
    int strides[] = {static_cast<int>(result.image.step), 0, 0, 0};
    if (sws_scale(context, decoded->data, decoded->linesize, 0, decoded->height, destination, strides) <= 0)
    {
        throw std::runtime_error("Преобразование кадра в BGR завершилось ошибкой");
    }
    return result;
}

Frame VideoCapture::makeFrame(std::shared_ptr<AVFrame> decoded)
{
    Frame result;
    const AVStream* stream = formatContext->streams[streamIndex];
    const bool isGpuFrame = decoded->format == AV_PIX_FMT_CUDA && decoded->hw_frames_ctx;
    gpuActive = isGpuFrame;
    if (isGpuFrame)
    {
        decoded->time_base = stream->time_base;
        result.gpuImage = std::move(decoded);
    }
    else
    {
        result = makeCpuFrame(decoded);
    }
    const AVFrame* stamp = result.gpuImage ? result.gpuImage.get() : decoded.get();
    if (stamp->best_effort_timestamp != AV_NOPTS_VALUE)
    {
        result.timestamp = std::chrono::microseconds(av_rescale_q(stamp->best_effort_timestamp, stream->time_base, AVRational{1, 1000000}));
    }
    if (stamp->duration > 0)
    {
        result.duration = std::chrono::microseconds(av_rescale_q(stamp->duration, stream->time_base, AVRational{1, 1000000}));
    }
    else if (frameRate.num > 0 && frameRate.den > 0)
    {
        result.duration = std::chrono::microseconds(av_rescale_q(1, av_inv_q(frameRate), AVRational{1, 1000000}));
    }
    result.streamGeneration = generation;
    return result;
}

std::optional<Frame> VideoCapture::decodeFrame()
{
    if (ended)
    {
        return std::nullopt;
    }
    beginIo();
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    std::shared_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
    if (!packet || !frame)
    {
        throw std::bad_alloc();
    }
    while (true)
    {
        checkInterrupted("Получение кадра", stopRequested, deadline);
        const int received = avcodec_receive_frame(decoderContext.get(), frame.get());
        checkInterrupted("Получение кадра", stopRequested, deadline);
        if (received == 0)
        {
            return makeFrame(std::move(frame));
        }
        av_frame_unref(frame.get());
        if (received == AVERROR_EOF)
        {
            ended = true;
            return std::nullopt;
        }
        if (received != AVERROR(EAGAIN))
        {
            check(received, "Получение кадра");
        }
        if (draining)
        {
            throw std::runtime_error("Decoder запросил packet после EOF flush");
        }
        feedDecoder(packet.get());
    }
}

bool VideoCapture::waitReconnect(unsigned seconds) noexcept
{
    for (unsigned elapsed = 0; elapsed < seconds * 10 && !stopRequested.load(std::memory_order_relaxed); ++elapsed)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !stopRequested.load(std::memory_order_relaxed);
}

bool VideoCapture::reconnect(unsigned attempt) noexcept
{
    static constexpr unsigned delays[] = {1, 2, 4, 5};
    const unsigned delay = delays[std::min<std::size_t>(attempt - 1, 3)];
    std::cout << "[VideoCapture] Переподключение: попытка " << attempt << "..." << std::endl;
    if (!waitReconnect(delay))
    {
        return false;
    }
    try
    {
        prepareAttempt(attempt);
        return true;
    }
    catch (...)
    {
        resetAttempt();
        return false;
    }
}

void VideoCapture::storeError(std::exception_ptr error) noexcept
{
    std::lock_guard<std::mutex> lock(readerErrorMutex);
    readerError = std::move(error);
}

void VideoCapture::readerLoop() noexcept
{
    try
    {
        readWithRetry();
    }
    catch (...)
    {
        if (!stopRequested.load(std::memory_order_relaxed))
        {
            storeError(std::current_exception());
        }
    }
    frameQueue->close();
}

void VideoCapture::readWithRetry()
{
    unsigned retries = 0;
    bool connected = decoderContext != nullptr;
    while (!stopRequested.load(std::memory_order_relaxed))
    {
        if (!connected)
        {
            if (!reconnectAllowed(retries))
            {
                throw std::runtime_error("Попытки переподключения исчерпаны");
            }
            if (!reconnect(++retries))
            {
                if (stopRequested.load(std::memory_order_relaxed))
                {
                    return;
                }
                continue;
            }
            connected = true;
        }
        try
        {
            if (readConnectedFrames())
            {
                return;
            }
        }
        catch (...)
        {
            if (!liveInput || stopRequested.load(std::memory_order_relaxed))
            {
                throw;
            }
            if (!reconnectAllowed(retries))
            {
                throw std::runtime_error("Попытки переподключения исчерпаны");
            }
        }
        resetAttempt();
        connected = false;
    }
}

bool VideoCapture::readConnectedFrames()
{
    while (!stopRequested.load(std::memory_order_relaxed))
    {
        auto frame = decodeFrame();
        if (!frame)
        {
            return !liveInput;
        }
        if (!enqueueFrame(std::move(*frame)))
        {
            return true;
        }
    }
    return true;
}

bool VideoCapture::enqueueFrame(Frame frame)
{
    if (!options.keepAllFrames)
    {
        return frameQueue->push(std::move(frame));
    }
    // Ожидание разбито на короткие интервалы, чтобы requestStop отменял полный буфер.
    while (!stopRequested.load(std::memory_order_relaxed) && !frameQueue->isClosed())
    {
        if (frameQueue->waitPush(frame, std::chrono::milliseconds(100)))
        {
            return true;
        }
    }
    return false;
}

bool VideoCapture::reconnectAllowed(unsigned retries) const noexcept
{
    return liveInput && retries < options.reconnectAttempts;
}

std::optional<Frame> VideoCapture::read()
{
    if (!initialized || !frameQueue)
    {
        throw std::logic_error("Сначала успешно вызови VideoCapture::init()");
    }
    auto frame = frameQueue->pop();
    if (frame)
    {
        return frame;
    }
    std::lock_guard<std::mutex> lock(readerErrorMutex);
    if (readerError)
    {
        std::rethrow_exception(readerError);
    }
    return std::nullopt;
}

bool VideoCapture::isFinished() const
{
    if (!initialized || !frameQueue)
    {
        throw std::logic_error("Сначала успешно вызови VideoCapture::init()");
    }
    return frameQueue->isClosed() && frameQueue->size() == 0;
}

std::size_t VideoCapture::droppedCount() const
{
    if (!initialized || !frameQueue)
    {
        throw std::logic_error("Сначала успешно вызови VideoCapture::init()");
    }
    return frameQueue->droppedCount();
}

std::string VideoCapture::description() const
{
    std::lock_guard<std::mutex> lock(readerErrorMutex);
    if (!initialized || cachedDescription.empty())
    {
        throw std::logic_error("Сначала успешно вызови VideoCapture::init()");
    }
    return cachedDescription;
}
}
