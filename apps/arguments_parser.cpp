#include "arguments_parser.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <utility>

namespace demo
{
namespace
{
void printUsage()
{
    std::cout << "[main] camera_demo [output.csv]\n"
              << "[main] camera_demo [options] --inspect <file-or-rtsp>\n"
              << "[main] camera_demo [options] --decode-first <file-or-rtsp>\n"
              << "[main] camera_demo [options] --decode <file-or-rtsp> <output.csv>\n"
              << "[main] camera_demo [options] --transcode <file-or-rtsp> [output.mp4]\n"
              << "[main] camera_demo [options] --source <file-or-rtsp> [--output <output.mp4>] ... [--output-dir <directory>]\n"
              << "[main] options: --cpu --queue-depth N --write-queue-depth N --reconnect N --keep-all-frames --help\n";
}

bool parseValue(const std::string& option, const std::string& value, Command& command)
{
    unsigned number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    {
        return false;
    }
    if (option == "--queue-depth")
    {
        if (number == 0)
        {
            return false;
        }
        command.queueDepth = number;
    }
    else if (option == "--write-queue-depth")
    {
        command.writeQueueDepth = number;
    }
    else if (option == "--reconnect")
    {
        command.reconnectAttempts = number;
    }
    else
    {
        return false;
    }
    return true;
}

bool parseOptions(int& argc, char**& argv, Command& command)
{
    while (argc > 1)
    {
        const std::string option = argv[1];
        if (option == "--keep-all-frames")
        {
            command.keepAllFrames = true;
            --argc;
            ++argv;
        }
        else if (option == "--cpu")
        {
            command.forceCpu = true;
            --argc;
            ++argv;
        }
        else if (option == "--queue-depth" || option == "--write-queue-depth" || option == "--reconnect")
        {
            if (argc < 3 || !parseValue(option, argv[2], command))
            {
                return false;
            }
            argc -= 2;
            argv += 2;
        }
        else break;
    }
    return true;
}

bool parseMode(int argc, char* argv[], Command& command)
{
    if (argc == 1)
    {
        return true;
    }
    const std::string option = argv[1];
    if (option == "--inspect" && argc == 3)
    {
        command.mode = Mode::Inspect;
    }
    else if (option == "--decode-first" && argc == 3)
    {
        command.mode = Mode::DecodeFirst;
    }
    else if (option == "--decode" && argc == 4)
    {
        command.mode = Mode::Decode;
    }
    else if (option == "--transcode" && (argc == 3 || argc == 4))
    {
        command.mode = Mode::Transcode;
    }
    else if (argc == 2 && option.rfind("--", 0) != 0)
    {
        command.outputPath = option;
        return true;
    }
    else
    {
        return false;
    }
    command.inputPath = argv[2];
    if (argc == 4)
    {
        command.outputPath = argv[3];
    }
    return true;
}

bool validateReconnect(const Command& command)
{
    if (!command.reconnectAttempts)
    {
        return true;
    }
    if (command.mode == Mode::Transcode && command.inputPath.rfind("rtsp://", 0) == 0)
    {
        return true;
    }
    for (const auto& source : command.sources)
    {
        if (source.inputPath.rfind("rtsp://", 0) == 0)
        {
            return true;
        }
    }
    std::cerr << "[main] --reconnect доступен только для --transcode RTSP\n";
    return false;
}

bool isMultiSourceArguments(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--source" || arg == "--output" || arg == "--output-dir")
        {
            return true;
        }
    }
    return false;
}

bool readPathArgument(int& index, int argc, char* argv[], std::string& value)
{
    if (++index >= argc || argv[index][0] == '\0' || std::string(argv[index]).rfind("--", 0) == 0)
    {
        return false;
    }
    value = argv[index];
    return true;
}

bool parseMultiTranscode(int& index, int argc, char* argv[], Command& command)
{
    std::string inputPath;
    if (!readPathArgument(index, argc, argv, inputPath))
    {
        return false;
    }
    std::string outputPath;
    if (index + 1 < argc && std::string(argv[index + 1]).rfind("--", 0) != 0)
    {
        outputPath = argv[++index];
    }
    command.sources.push_back({std::move(inputPath), std::move(outputPath)});
    return true;
}

bool parseMultiOption(int& index, int argc, char* argv[], Command& command, std::optional<std::size_t>& activeSource)
{
    const std::string option = argv[index];
    if (option == "--source")
    {
        std::string inputPath;
        if (!readPathArgument(index, argc, argv, inputPath))
        {
            return false;
        }
        command.sources.push_back({std::move(inputPath), {}});
        activeSource = command.sources.size() - 1;
        return true;
    }
    if (option == "--transcode")
    {
        if (!parseMultiTranscode(index, argc, argv, command))
        {
            return false;
        }
        activeSource = command.sources.size() - 1;
        return true;
    }
    if (option == "--output")
    {
        std::string outputPath;
        if (!activeSource || !command.sources[*activeSource].outputPath.empty() || !readPathArgument(index, argc, argv, outputPath))
        {
            return false;
        }
        command.sources[*activeSource].outputPath = std::move(outputPath);
        return true;
    }
    if (option == "--output-dir")
    {
        if (!readPathArgument(index, argc, argv, command.outputDirectory))
        {
            return false;
        }
        command.outputDirectoryExplicit = true;
        return true;
    }
    if (option == "--keep-all-frames")
    {
        command.keepAllFrames = true;
        return true;
    }
    if (option == "--cpu")
    {
        command.forceCpu = true;
        return true;
    }
    if (option == "--queue-depth" || option == "--write-queue-depth" || option == "--reconnect")
    {
        return index + 1 < argc && parseValue(option, argv[++index], command);
    }
    return false;
}

bool parseMultiSource(int argc, char* argv[], Command& command)
{
    command.mode = Mode::Transcode;
    std::optional<std::size_t> activeSource;
    for (int i = 1; i < argc; ++i)
    {
        if (!parseMultiOption(i, argc, argv, command, activeSource))
        {
            return false;
        }
    }
    if (command.sources.empty())
    {
        return false;
    }
    command.inputPath = command.sources.front().inputPath;
    command.outputPath = command.sources.front().outputPath;
    return true;
}
}

std::optional<Command> ArgumentsParser::parse(int argc, char* argv[]) const
{
    Command command;
    if (argc == 1)
    {
        command.help = true;
        printUsage();
        return command;
    }
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--help")
        {
            command.help = true;
            printUsage();
            return command;
        }
    }
    const bool multiSource = isMultiSourceArguments(argc, argv);
    const bool parsed = multiSource ? parseMultiSource(argc, argv, command) : parseOptions(argc, argv, command) && parseMode(argc, argv, command);
    if (!parsed || !validateReconnect(command))
    {
        printUsage();
        return std::nullopt;
    }
    if (!multiSource && command.mode == Mode::Transcode)
    {
        if (argc == 3)
        {
            command.outputPath.clear();
        }
        command.sources.push_back({command.inputPath, command.outputPath});
    }
    if (!multiSource && !validatePaths(command))
    {
        return std::nullopt;
    }
    return command;
}

bool validatePaths(const Command& command)
{
    if (command.mode != Mode::Decode && command.mode != Mode::Transcode)
    {
        return true;
    }
    if (command.inputPath.rfind("rtsp://", 0) == 0 || !std::filesystem::exists(command.outputPath))
    {
        return true;
    }
    std::error_code error;
    const bool same = std::filesystem::equivalent(command.inputPath, command.outputPath, error);
    if (!error && !same)
    {
        return true;
    }
    std::cerr << "[main] " << (error ? error.message() : "Входной и выходной файлы совпадают") << '\n';
    return false;
}
}
