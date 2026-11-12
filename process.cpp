#include "process.h"

#include <array>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

extern char** environ;

namespace {
using FileActions = posix_spawn_file_actions_t;

bool exited_within(pid_t child, std::chrono::milliseconds patience)
{
    using Clock = std::chrono::steady_clock;
    for (const auto deadline = Clock::now() + patience;;) {
        if (waitpid(child, nullptr, WNOHANG) != 0)
            return true;
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
}
} // namespace

ChildProcess::ChildProcess(std::span<const std::string> arguments, const std::string& role,
                           const std::string& error_log)
{
    std::array<int, 2> sockets{};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) != 0)
        throw std::runtime_error("Could not create a socket for the " + role);
    FileDescriptor ours{sockets[0]}, theirs{sockets[1]};
    FileActions actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, ours.get());
    posix_spawn_file_actions_addclose(&actions, theirs.get());
    if (!error_log.empty())
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, error_log.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    std::vector<std::string> owned(arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : owned)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    const int error = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        child = -1;
        throw std::runtime_error("Could not start the " + role + ": " + std::strerror(error));
    }
    fcntl(ours.get(), F_SETFL, fcntl(ours.get(), F_GETFL) | O_NONBLOCK);
    socket = std::move(ours);
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
    if (this != &other) {
        stop(0ms);
        socket = std::move(other.socket);
        child = std::exchange(other.child, -1);
    }
    return *this;
}

ChildProcess::~ChildProcess()
{
    stop(0ms);
}

void ChildProcess::stop(std::chrono::milliseconds patience)
{
    socket.reset();
    const auto leaving = std::exchange(child, -1);
    if (leaving <= 0 || exited_within(leaving, patience))
        return;
    kill(leaving, SIGTERM);
    if (exited_within(leaving, 200ms))
        return;
    kill(leaving, SIGKILL);
    waitpid(leaving, nullptr, 0);
}
