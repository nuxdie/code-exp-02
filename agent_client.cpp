#include "agent_client.h"

#include "process.h"
#include "wakeup.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>

using namespace std::chrono_literals;

struct AgentClient::Impl {
    static constexpr std::size_t largest_message = 64 * 1024 * 1024;
    Mailbox<Json> received;
    WakeSignal commands;
    std::mutex mutex;
    std::string outgoing; // guarded by mutex
    std::string incoming;
    bool disconnected = false;
    ChildProcess bridge;
    std::jthread worker;

    explicit Impl(std::vector<std::string> arguments)
        : worker([this, arguments = std::move(arguments)](const std::stop_token& stopping) {
              const std::stop_callback wake(stopping, [this] { commands.raise(); });
              try {
                  bridge = ChildProcess(arguments, "agent connection");
                  run(stopping);
              } catch (const std::exception& error) {
                  if (!disconnected)
                      publish({{"method", "canvas/disconnected"},
                               {"params", {{"message", error.what()}}}});
              }
              bridge.stop(200ms);
          })
    {
    }
    void publish(Json message)
    {
        if (message.value("method", "") == "canvas/disconnected")
            disconnected = true;
        received.post(std::move(message));
    }
    void read_messages()
    {
        std::array<char, 65536> bytes;
        const auto count = recv(bridge.stream(), bytes.data(), bytes.size(), 0);
        if (!count)
            throw std::runtime_error("The agent connection closed");
        if (count < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            throw std::runtime_error("Could not read the agent connection");
        }
        incoming.append(bytes.data(), static_cast<std::size_t>(count));
        if (incoming.size() > largest_message)
            throw std::runtime_error("Agent message exceeds the transport limit");
        for (std::size_t end; (end = incoming.find('\n')) != std::string::npos;) {
            auto message = Json::parse(std::string_view(incoming).substr(0, end), nullptr, false);
            incoming.erase(0, end + 1);
            if (!message.is_discarded())
                publish(std::move(message));
        }
    }
    std::string_view write_some(std::string_view pending) const
    {
        const auto count = ::send(bridge.stream(), pending.data(), pending.size(), MSG_NOSIGNAL);
        if (count >= 0)
            return pending.substr(static_cast<std::size_t>(count));
        if (errno != EAGAIN && errno != EINTR)
            throw std::runtime_error("Could not write to the agent connection");
        return pending;
    }
    void run(const std::stop_token& stopping)
    {
        std::string pending;
        std::size_t written = 0;
        while (!stopping.stop_requested()) {
            std::array<pollfd, 2> fds{
                {{bridge.stream(), static_cast<short>(POLLIN | (pending.empty() ? 0 : POLLOUT)), 0},
                 {commands.fd(), POLLIN, 0}}};
            if (poll(fds.data(), fds.size(), -1) < 0 && errno != EINTR)
                throw std::runtime_error("Could not poll the agent connection");
            if (fds[1].revents) {
                commands.clear();
                const std::scoped_lock lock(mutex);
                pending += std::exchange(outgoing, {});
            }
            if ((fds[0].revents & POLLOUT) && !pending.empty()) {
                const auto rest = write_some(std::string_view(pending).substr(written));
                written = pending.size() - rest.size();
                if (rest.empty()) {
                    pending.clear();
                    written = 0;
                }
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
                read_messages();
        }
    }
};
AgentClient::AgentClient(std::string python, std::string bridge)
    : impl(std::make_unique<Impl>(
          std::vector<std::string>{std::move(python), "-u", std::move(bridge)}))
{
}
AgentClient::~AgentClient() = default;
int AgentClient::event_fd() const
{
    return impl->received.fd();
}
void AgentClient::send(const Json& message)
{
    {
        const std::scoped_lock lock(impl->mutex);
        impl->outgoing += message.dump() + '\n';
    }
    impl->commands.raise();
}
std::vector<AgentClient::Json> AgentClient::take_messages()
{
    return impl->received.take();
}
