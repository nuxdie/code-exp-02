#pragma once

#include "wakeup.h"

#include <chrono>
#include <span>
#include <string>
#include <sys/types.h>
#include <utility>

// A child that talks over one socket, which is both its standard input and its output.
class ChildProcess {
  public:
    ChildProcess() = default;
    // Throws std::runtime_error naming `role` when the program cannot be started.
    ChildProcess(std::span<const std::string> arguments, const std::string& role,
                 const std::string& error_log = {});
    ChildProcess(ChildProcess&& other) noexcept
        : socket(std::move(other.socket)), child(std::exchange(other.child, -1))
    {
    }
    ChildProcess& operator=(ChildProcess&&) noexcept;
    ~ChildProcess();
    [[nodiscard]] int stream() const { return socket.get(); }
    // Closes the socket and waits for the child to leave; past `patience` it is told to.
    void stop(std::chrono::milliseconds patience);

  private:
    FileDescriptor socket;
    pid_t child = -1;
};
