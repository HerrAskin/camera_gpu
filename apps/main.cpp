#include "arguments_parser.hpp"
#include "session.hpp"

#include <csignal>
#include <iostream>

namespace
{
volatile std::sig_atomic_t interrupted = 0;

void handleInterrupt(int signal)
{
    // Обработчик не вызывает методы Session и не выполняет ввод-вывод.
    if (signal == SIGINT)
    {
        interrupted = 1;
    }
}
}

int main(int argc, char* argv[])
{
    if (std::signal(SIGINT, handleInterrupt) == SIG_ERR)
    {
        std::cerr << "[main] Не удалось зарегистрировать SIGINT\n";
        return 1;
    }
    auto args = demo::ArgumentsParser();
    auto command = args.parse(argc, argv);
    if (!command)
    {
        return 1;
    }
    if (command->help)
    {
        return 0;
    }
    demo::Session session(*command, interrupted);
    if (session.init())
    {
        session.start();
    }
    session.stop();
    return session.exitCode();
}
