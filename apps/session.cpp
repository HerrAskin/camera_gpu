#include "session.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace demo
{
namespace
{
bool isRtsp(const std::string& source)
{
    return source.rfind("rtsp://", 0) == 0;
}

std::filesystem::path normalized(const std::filesystem::path& path)
{
    return std::filesystem::weakly_canonical(path);
}

std::set<std::filesystem::path> reservePaths(const std::vector<Source>& sources)
{
    std::set<std::filesystem::path> reserved;
    for (const auto& source : sources)
    {
        if (!isRtsp(source.inputPath))
        {
            reserved.insert(normalized(source.inputPath));
        }
    }
    for (const auto& source : sources)
    {
        if (source.outputPath.empty())
        {
            continue;
        }
        const auto path = normalized(source.outputPath);
        for (const auto& occupied : reserved)
        {
            std::error_code error;
            if (path == occupied || std::filesystem::equivalent(path, occupied, error))
            {
                throw std::runtime_error("Выходной путь совпадает с источником или другим target");
            }
        }
        reserved.insert(path);
    }
    return reserved;
}

std::filesystem::path selectOutput(const Source& source, const Command& command, std::size_t index, std::set<std::filesystem::path>& reserved)
{
    if (!source.outputPath.empty())
    {
        return source.outputPath;
    }
    const auto stem = isRtsp(source.inputPath) ? "source" + std::to_string(index + 1)
        : std::filesystem::path(source.inputPath).stem().string();
    const auto base = std::filesystem::path(command.outputDirectory) / (stem + "_out.mp4");
    auto path = base;
    unsigned suffix = 2;
    // Не перезаписываем существующую запись и не используем путь другого источника.
    while (std::filesystem::exists(path) || reserved.count(normalized(path)))
    {
        path = base.parent_path() / (stem + "_out_" + std::to_string(suffix++) + ".mp4");
    }
    reserved.insert(normalized(path));
    return path;
}
}

Session::Session(Command value, const volatile std::sig_atomic_t& flag) : command(std::move(value)), interrupted(flag)
{
}

Session::~Session()
{
    stop();
}

bool Session::init()
{
    if (initialized || stopped)
    {
        return false;
    }
    try
    {
        createPipelines();
        initialized = true;
        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Session::init] Ошибка: " << error.what() << '\n';
        return false;
    }
}

void Session::createPipelines()
{
    if (command.sources.empty())
    {
        pipelines.push_back(std::make_unique<Pipeline>(command, stopRequested));
        return;
    }
    const bool usesDefaultDirectory = !command.outputDirectoryExplicit &&
        std::any_of(command.sources.begin(), command.sources.end(), [](const Source& source) { return source.outputPath.empty(); });
    if (usesDefaultDirectory)
    {
        std::cout << "[Session::init] Каталог записи по умолчанию: " << command.outputDirectory << '\n';
    }
    auto reserved = reservePaths(command.sources);
    for (std::size_t index = 0; index < command.sources.size(); ++index)
    {
        auto options = command;
        options.sources.clear();
        options.inputPath = command.sources[index].inputPath;
        options.outputPath = selectOutput(command.sources[index], command, index, reserved).string();
        const auto parent = std::filesystem::path(options.outputPath).parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }
        std::cout << "[Session::init] Источник " << index + 1 << ", выход: " << options.outputPath << '\n';
        pipelines.push_back(std::make_unique<Pipeline>(std::move(options), stopRequested));
    }
}

void Session::start()
{
    if (!initialized || stopped || started)
    {
        return;
    }
    started = true;
    const auto startTime = std::chrono::steady_clock::now();
    try
    {
        for (auto& pipeline : pipelines)
        {
            pipeline->start();
        }
        while (!interrupted && anyRunning())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop();
        collectResults();
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Session::start] Ошибка: " << error.what() << '\n';
        stop();
        result = 1;
    }
    reportStatistics(startTime);
}

void Session::reportStatistics(std::chrono::steady_clock::time_point startTime) const
{
    std::size_t frames = 0;
    for (const auto& pipeline : pipelines)
    {
        frames += pipeline->statistics().processedFrames;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    const double fps = seconds > 0 ? frames / seconds : 0;
    std::ostringstream message;
    message << std::fixed << std::setprecision(6)
            << "[Session] Источников: " << pipelines.size() << "; кадров: " << frames
            << "; время: " << seconds << " с; суммарный FPS: " << fps << '\n';
    std::cout << message.str();
}

bool Session::anyRunning() const
{
    for (const auto& pipeline : pipelines)
    {
        if (pipeline->isRunning())
        {
            return true;
        }
    }
    return false;
}

void Session::collectResults()
{
    result = 0;
    for (const auto& pipeline : pipelines)
    {
        if (pipeline->exitCode() != 0)
        {
            result = 1;
        }
    }
}

void Session::stop() noexcept
{
    if (stopped)
    {
        return;
    }
    stopped = true;
    stopRequested.store(true);
    for (auto& pipeline : pipelines)
    {
        pipeline->stop();
    }
}

int Session::exitCode() const noexcept
{
    return result;
}
}
