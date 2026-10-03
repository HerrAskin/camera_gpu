#include "camera/file_video.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
}

#include <iostream>
#include <cstdint>
#include <limits>
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

void checkIo(int result, const char* operation, const std::atomic<bool>& stopRequested, std::chrono::steady_clock::time_point deadline)
{
    if (result < 0)
    {
        if (stopRequested.load(std::memory_order_relaxed))
        {
            throw std::runtime_error(std::string(operation) + ": запрошена остановка");
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw std::runtime_error(std::string(operation) + ": истёк срок ожидания I/O");
        }
        if (result == AVERROR_EXIT)
        {
            throw std::runtime_error(std::string(operation) + ": I/O прервано");
        }
        check(result, operation);
    }
}

}

int FileVideo::interruptIo(void* opaque) noexcept
{
    const auto* video = static_cast<const FileVideo*>(opaque);
    // Callback вызывается в потоке FFmpeg; relaxed достаточно для независимого флага отмены.
    return video->stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= video->deadline;
}

void FileVideo::beginIo() noexcept
{
    // Один дедлайн ограничивает весь init/read, включая разбор пакетов и выдачу кадров.
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::steady_clock::time_point::max() - now;
    if (ioTimeout >= std::chrono::duration_cast<std::chrono::milliseconds>(remaining))
    {
        deadline = std::chrono::steady_clock::time_point::max();
        return;
    }
    deadline = now + ioTimeout;
}

void FileVideo::requestStop() noexcept
{
    stopRequested.store(true, std::memory_order_relaxed);
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

void FileVideo::openInput(const std::string& path)
{
    formatContext.reset(avformat_alloc_context());
    if (!formatContext)
    {
        throw std::bad_alloc();
    }
    formatContext->interrupt_callback.callback = interruptIo;
    formatContext->interrupt_callback.opaque = this;
    AVDictionary* options = nullptr;
    if (path.rfind("rtsp://", 0) == 0)
    {
        // RTP по TCP проходит через SSH-туннель вместе с RTSP-соединением.
        check(av_dict_set(&options, "rtsp_transport", "tcp", 0), "Настройка RTSP transport");
        const auto milliseconds = ioTimeout.count();
        const auto maxMicroseconds = std::numeric_limits<std::int64_t>::max();
        const auto timeout = milliseconds > maxMicroseconds / 1000 ? maxMicroseconds : milliseconds * 1000;
        const std::string timeoutValue = std::to_string(timeout);
        const int timeoutResult = av_dict_set(&options, "timeout", timeoutValue.c_str(), 0);
        if (timeoutResult < 0)
        {
            av_dict_free(&options);
            check(timeoutResult, "Настройка RTSP timeout");
        }
    }
    beginIo();
    // open_input может заменить или освободить переданный context даже при ошибке;
    // после вызова сохраняем возвращённый указатель в RAII-владельце.
    AVFormatContext* rawContext = formatContext.release();
    const int openResult = avformat_open_input(&rawContext, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    formatContext.reset(rawContext);
    checkIo(openResult, "Открытие источника", stopRequested, deadline);
    beginIo();
    checkIo(avformat_find_stream_info(formatContext.get(), nullptr), "Чтение информации о потоках", stopRequested, deadline);
    streamIndex = av_find_best_stream(formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(streamIndex, "Поиск видеопотока");
}

void FileVideo::createCudaDevice()
{
    AVBufferRef* rawDevice = nullptr;
    const int result = av_hwdevice_ctx_create(&rawDevice, AV_HWDEVICE_TYPE_CUDA, "0", nullptr, 0);
    cudaDevice.reset(rawDevice);
    check(result, "Создание CUDA-контекста");
}

void FileVideo::openDecoder()
{
    const AVStream* stream = formatContext->streams[streamIndex];
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

    decoderContext.reset(avcodec_alloc_context3(codec));
    if (!decoderContext)
    {
        throw std::bad_alloc();
    }
    check(avcodec_parameters_to_context(decoderContext.get(), stream->codecpar), "Передача параметров codec");
    decoderContext->pkt_timebase = stream->time_base;
    decoderContext->get_format = selectCudaFormat;
    // Decoder удерживает отдельную ссылку на то же устройство.
    decoderContext->hw_device_ctx = av_buffer_ref(cudaDevice.get());
    if (!decoderContext->hw_device_ctx)
    {
        throw std::bad_alloc();
    }
    check(avcodec_open2(decoderContext.get(), codec, nullptr), "Открытие decoder");
}

bool FileVideo::init(const std::string& path, std::chrono::milliseconds timeout)
{
    if (formatContext)
    {
        std::cerr << "[FileVideo::init] Ошибка инициализации: объект уже инициализирован\n";
        return false;
    }
    if (timeout <= std::chrono::milliseconds::zero())
    {
        std::cerr << "[FileVideo::init] Ошибка инициализации: ioTimeout должен быть больше нуля\n";
        return false;
    }
    ioTimeout = timeout;
    stopRequested.store(false, std::memory_order_relaxed);
    try
    {
        openInput(path);
        createCudaDevice();
        openDecoder();
        draining = false;
        ended = false;
        std::cout << "[FileVideo::init] инициализация успешна\n";
        return true;
    }
    catch (const std::exception& error)
    {
        // Decoder держит ссылку на device, поэтому освобождаем ресурсы в обратном порядке.
        decoderContext.reset();
        cudaDevice.reset();
        formatContext.reset();
        streamIndex = -1;
        draining = false;
        ended = false;
        std::cerr << "[FileVideo::init] Ошибка инициализации: " << error.what() << '\n';
        return false;
    }
}

void FileVideo::feedDecoder(AVPacket* packet)
{
    int readResult = 0;
    while (true)
    {
        if (stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
        {
            checkIo(AVERROR_EXIT, "[FileVideo::read] Чтение packet", stopRequested, deadline);
        }
        readResult = av_read_frame(formatContext.get(), packet);
        if (readResult < 0)
        {
            break;
        }
        if (packet->stream_index == streamIndex)
        {
            break;
        }
        // av_read_frame возвращает владение ссылками на данные packet вызывающему коду.
        av_packet_unref(packet);
    }
    if (readResult == AVERROR_EOF)
    {
        if (stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
        {
            checkIo(AVERROR_EXIT, "[FileVideo::read] Чтение packet", stopRequested, deadline);
        }
        const int sendResult = avcodec_send_packet(decoderContext.get(), nullptr);
        if (sendResult < 0 && sendResult != AVERROR_EOF)
        {
            check(sendResult, "[FileVideo::read] EOF flush");
        }
        draining = true;
        return;
    }
    checkIo(readResult, "[FileVideo::read] Чтение packet", stopRequested, deadline);

    const int sendResult = avcodec_send_packet(decoderContext.get(), packet);
    // Decoder удерживает нужные ссылки сам; освобождаем packet до проверки результата.
    av_packet_unref(packet);
    check(sendResult, "[FileVideo::read] Передача packet decoder");
}

Frame FileVideo::makeFrame(std::shared_ptr<AVFrame> frame) const
{
    if (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx)
    {
        throw std::runtime_error("[FileVideo::read] Decoder вернул кадр без CUDA hw_frames_ctx");
    }
    Frame result;
    // shared_ptr сохраняет hw_frames_ctx и CUDA-поверхность после следующего вызова read().
    result.gpuImage = std::move(frame);
    // Duration использует time base потока, чтобы Writer мог пересчитать её для encoder.
    const AVStream* stream = formatContext->streams[streamIndex];
    result.gpuImage->time_base = stream->time_base;
    if (result.gpuImage->best_effort_timestamp != AV_NOPTS_VALUE)
    {
        result.timestamp = std::chrono::microseconds(av_rescale_q(result.gpuImage->best_effort_timestamp, stream->time_base, AVRational{1, 1000000}));
    }
    return result;
}

std::optional<Frame> FileVideo::read()
{
    if (!decoderContext || !avcodec_is_open(decoderContext.get()) || !formatContext || streamIndex < 0)
    {
        throw std::logic_error("[FileVideo::read] Сначала успешно вызови FileVideo::init()");
    }
    if (stopRequested.load(std::memory_order_relaxed))
    {
        throw std::runtime_error("[FileVideo::read] Запрошена остановка");
    }
    if (ended)
    {
        return std::nullopt;
    }
    beginIo();
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
        if (stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
        {
            checkIo(AVERROR_EXIT, "[FileVideo::read] Получение кадра", stopRequested, deadline);
        }
        // Сначала забираем готовые кадры из decoder; новый packet нужен только при EAGAIN.
        const int receiveResult = avcodec_receive_frame(decoderContext.get(), gpuFrame.get());
        if (receiveResult == 0)
        {
            if (stopRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
            {
                checkIo(AVERROR_EXIT, "[FileVideo::read] Получение кадра", stopRequested, deadline);
            }
            return makeFrame(std::move(gpuFrame));
        }
        av_frame_unref(gpuFrame.get());
        if (receiveResult == AVERROR_EOF)
        {
            // nullopt возвращается только после выдачи decoder всех задержанных кадров.
            ended = true;
            return std::nullopt;
        }
        if (receiveResult != AVERROR(EAGAIN))
        {
            check(receiveResult, "[FileVideo::read] Получение кадра");
        }
        if (draining)
        {
            throw std::runtime_error("[FileVideo::read] Decoder запросил packet после EOF flush");
        }
        feedDecoder(packet.get());
    }
}

std::string FileVideo::description() const
{
    if (!decoderContext || !avcodec_is_open(decoderContext.get()))
    {
        throw std::logic_error("[FileVideo::description] Сначала успешно вызови FileVideo::init()");
    }
    const AVStream* stream = formatContext->streams[streamIndex];
    const AVCodecParameters* codec = stream->codecpar;
    std::ostringstream output;
    output << "Видеопоток: " << streamIndex << '\n'
           << "Codec: " << avcodec_get_name(codec->codec_id) << '\n'
           << "Размер: " << codec->width << 'x' << codec->height << '\n'
           << "Time base: " << stream->time_base.num << '/'
           << stream->time_base.den << '\n'
           << "CUDA-контекст: создан, устройство 0" << '\n'
           << "Decoder: " << decoderContext->codec->name << ", открыт с CUDA";
    return output.str();
}
}
