#include "live_build.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

struct LiveBuild::Impl {
    std::string directory;
    int events = -1;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<uint64_t> pending;
    std::vector<BuildResult> results;
    std::atomic<bool> stopping{false};
    std::thread worker;

    explicit Impl(std::string directory) : directory(std::move(directory))
    {
        events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (events < 0)
            throw std::runtime_error("Could not create live-build event descriptor");
        worker = std::thread([this] { run(); });
    }
    void publish(BuildResult result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(result));
        }
        const uint64_t value = 1;
        (void)!write(events, &value, sizeof(value));
    }
    pid_t spawn(int output)
    {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
        std::vector<std::string> args{"flock",
                                      directory + "/.viewer-build.lock",
                                      "cmake",
                                      "--build",
                                      directory,
                                      "--target",
                                      "white_window",
                                      "--parallel",
                                      "2"};
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        pid_t child = -1;
        const int error =
            posix_spawnp(&child, "flock", &actions, &attributes, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        return error ? -1 : child;
    }
    BuildResult build(uint64_t generation)
    {
        BuildResult result{generation, false, false, {}};
        int output[2];
        if (pipe2(output, O_CLOEXEC | O_NONBLOCK)) {
            result.output = "Could not create the build output pipe";
            return result;
        }
        // Only the reader is nonblocking; compilers must be able to write their output normally.
        fcntl(output[1], F_SETFL, fcntl(output[1], F_GETFL) & ~O_NONBLOCK);
        const auto child = spawn(output[1]);
        close(output[1]);
        if (child < 0) {
            close(output[0]);
            result.output = "Could not start CMake";
            return result;
        }
        int status = 0;
        bool finished = false;
        while (!finished && !stopping) {
            pollfd fd{output[0], POLLIN, 0};
            poll(&fd, 1, 50);
            char buffer[16384];
            ssize_t count;
            while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
                result.output.append(buffer, static_cast<std::size_t>(count));
            if (result.output.size() > 128 * 1024)
                result.output.erase(0, result.output.size() - 128 * 1024);
            finished = waitpid(child, &status, WNOHANG) == child;
        }
        if (!finished) {
            kill(-child, SIGTERM);
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (waitpid(child, &status, WNOHANG) == child) {
                    finished = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!finished) {
                kill(-child, SIGKILL);
                waitpid(child, &status, 0);
            }
        }
        close(output[0]);
        result.success = finished && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        return result;
    }
    void run()
    {
        for (;;) {
            uint64_t generation;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping)
                    return;
                generation = *pending;
                pending.reset();
            }
            publish({generation, true, false, {}});
            try {
                publish(build(generation));
            } catch (const std::exception& error) {
                publish({generation, false, false, error.what()});
            }
        }
    }
    ~Impl()
    {
        stopping = true;
        condition.notify_one();
        worker.join();
        close(events);
    }
};
LiveBuild::LiveBuild(std::string directory) : impl(std::make_unique<Impl>(std::move(directory))) {}
LiveBuild::~LiveBuild() = default;
void LiveBuild::request(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->pending = generation;
    impl->condition.notify_one();
}
int LiveBuild::event_fd() const
{
    return impl->events;
}
std::vector<BuildResult> LiveBuild::take_results()
{
    uint64_t value;
    while (read(impl->events, &value, sizeof(value)) > 0) {
    }
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<BuildResult> results;
    results.swap(impl->results);
    return results;
}
