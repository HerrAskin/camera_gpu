#pragma once

#include "arguments_parser.hpp"
#include "pipeline.hpp"

#include <atomic>
#include <csignal>
#include <memory>
#include <vector>

namespace demo
{
/** Управляет независимыми конвейерами; сигнал из main переводит в атомарную отмену. */
class Session
{
public:
    Session(Command command, const volatile std::sig_atomic_t& interrupted);
    ~Session();
    bool init();
    void start();
    void stop() noexcept;
    int exitCode() const noexcept;

private:
    void createPipelines();
    bool anyRunning() const;
    void collectResults();
    void reportStatistics(std::chrono::steady_clock::time_point startTime) const;

    Command command;
    const volatile std::sig_atomic_t& interrupted;
    std::atomic<bool> stopRequested{false};
    std::vector<std::unique_ptr<Pipeline>> pipelines;
    bool initialized = false;
    bool started = false;
    bool stopped = false;
    int result = 1;
};
}
