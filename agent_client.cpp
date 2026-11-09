#include "agent_client.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace {
void wake(int fd)
{
    const uint64_t value = 1;
    (void)!write(fd, &value, sizeof(value));
}
void drain(int fd)
{
    uint64_t value;
    while (read(fd, &value, sizeof(value)) > 0) {
    }
}
} // namespace

struct AgentClient::Impl {
    int events = -1, commands = -1, stream = -1;
    pid_t child = -1;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::string outgoing, incoming;
    std::vector<Json> received;
    std::thread worker;

    Impl(std::string python, std::string bridge)
    {
        events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        commands = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (events < 0 || commands < 0)
            throw std::runtime_error("Could not create agent transport descriptors");
        worker = std::thread([this, python = std::move(python), bridge = std::move(bridge)] {
            try {
                spawn(python, bridge);
                run();
            } catch (const std::exception& error) {
                publish(
                    {{"method", "canvas/disconnected"}, {"params", {{"message", error.what()}}}});
            }
            cleanup();
        });
    }
    void spawn(const std::string& python, const std::string& bridge)
    {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
            throw std::runtime_error("Could not create agent socket pair");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, sockets[0]);
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
        std::vector<std::string> args{python, "-u", bridge};
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        const int result =
            posix_spawnp(&child, python.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (result) {
            close(sockets[0]);
            child = -1;
            throw std::runtime_error("Could not start agent connection: " +
                                     std::string(strerror(result)));
        }
        stream = sockets[0];
        fcntl(stream, F_SETFL, fcntl(stream, F_GETFL) | O_NONBLOCK);
    }
    void publish(Json message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            received.push_back(std::move(message));
        }
        wake(events);
    }
    bool read_messages()
    {
        char bytes[65536];
        const auto count = recv(stream, bytes, sizeof(bytes), 0);
        if (!count)
            throw std::runtime_error("The agent connection closed");
        if (count < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return true;
            throw std::runtime_error("Could not read the agent connection");
        }
        incoming.append(bytes, static_cast<std::size_t>(count));
        if (incoming.size() > 64 * 1024 * 1024)
            throw std::runtime_error("Agent message exceeds the transport limit");
        std::size_t end;
        while ((end = incoming.find('\n')) != std::string::npos) {
            const auto message =
                Json::parse(incoming.begin(), incoming.begin() + end, nullptr, false);
            incoming.erase(0, end + 1);
            if (!message.is_discarded())
                publish(message);
        }
        return true;
    }
    void run()
    {
        std::string pending;
        std::size_t written = 0;
        while (!stopping) {
            pollfd fds[]{{stream, static_cast<short>(POLLIN | (pending.empty() ? 0 : POLLOUT)), 0},
                         {commands, POLLIN, 0}};
            if (poll(fds, 2, -1) < 0 && errno != EINTR)
                throw std::runtime_error("Could not poll the agent connection");
            if (fds[1].revents) {
                drain(commands);
                std::lock_guard<std::mutex> lock(mutex);
                pending += outgoing;
                outgoing.clear();
            }
            if ((fds[0].revents & POLLOUT) && !pending.empty()) {
                const auto count = ::send(stream, pending.data() + written,
                                          pending.size() - written, MSG_NOSIGNAL);
                if (count > 0) {
                    written += static_cast<std::size_t>(count);
                    if (written == pending.size()) {
                        pending.clear();
                        written = 0;
                    }
                } else if (errno != EAGAIN && errno != EINTR)
                    throw std::runtime_error("Could not write to the agent connection");
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
                read_messages();
        }
    }
    void cleanup()
    {
        if (stream >= 0)
            close(stream);
        if (child > 0) {
            for (int i = 0; i < 20; ++i) {
                if (waitpid(child, nullptr, WNOHANG) != 0)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            kill(child, SIGTERM);
            waitpid(child, nullptr, 0);
        }
    }
    ~Impl()
    {
        stopping = true;
        wake(commands);
        worker.join();
        close(events);
        close(commands);
    }
};
AgentClient::AgentClient(std::string python, std::string bridge)
    : impl(std::make_unique<Impl>(std::move(python), std::move(bridge)))
{
}
AgentClient::~AgentClient() = default;
int AgentClient::event_fd() const
{
    return impl->events;
}
void AgentClient::send(Json message)
{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->outgoing += message.dump() + '\n';
    }
    wake(impl->commands);
}
std::vector<AgentClient::Json> AgentClient::take_messages()
{
    drain(impl->events);
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<Json> result;
    result.swap(impl->received);
    return result;
}
