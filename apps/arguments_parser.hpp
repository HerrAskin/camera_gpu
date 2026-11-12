#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace demo
{
enum class Mode
{
    Mock,
    Inspect,
    DecodeFirst,
    Decode,
    Transcode
};

// Только параметры CLI; библиотека не зависит от способов их получения.
struct Source
{
    std::string inputPath;
    std::string outputPath;
};

struct Command
{
    Mode mode = Mode::Mock;
    std::string inputPath;
    std::string outputPath = "mock_frames.csv";
    std::size_t queueDepth = 8;
    std::size_t writeQueueDepth = 8;
    unsigned reconnectAttempts = 0;
    bool forceCpu = false;
    bool keepAllFrames = false;
    bool help = false;
    std::vector<Source> sources;
    std::string outputDirectory = "tmp/recordings";
    bool outputDirectoryExplicit = false;
};

class ArgumentsParser
{
public:
    std::optional<Command> parse(int argc, char* argv[]) const;
};
bool validatePaths(const Command& command);
}
