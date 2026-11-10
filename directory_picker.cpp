#include "directory_picker.h"

#include <atomic>
#include <gio/gio.h>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

struct DirectoryPicker::Impl {
    int events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    GSubprocess* process = nullptr;
    std::thread worker;
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::optional<DirectorySelection> result;

    Impl(const std::string& directory, unsigned long parent)
    {
        if (events < 0)
            throw std::runtime_error("Could not create the directory picker notification.");
        const auto filename = "--filename=" + directory + "/";
        const auto attach = "--attach=" + std::to_string(parent);
        GError* error = nullptr;
        process = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                   &error, "zenity", "--file-selection", "--directory", "--modal",
                                   "--title=Open project directory", filename.c_str(),
                                   attach.c_str(), nullptr);
        if (!process) {
            const std::string message = "Could not start the directory picker. Install zenity. " +
                                        std::string(error->message);
            g_error_free(error);
            close(events);
            throw std::runtime_error(message);
        }
        worker = std::thread([this] { run(); });
    }

    void run()
    {
        gchar* output = nullptr;
        GError* error = nullptr;
        DirectorySelection selection;
        if (!g_subprocess_communicate_utf8(process, nullptr, nullptr, &output, nullptr, &error)) {
            selection.error = error->message;
            g_error_free(error);
        } else if (g_subprocess_get_successful(process)) {
            selection.directory = output ? output : "";
            // Zenity appends one newline; whitespace within the path is significant.
            if (!selection.directory.empty() && selection.directory.back() == '\n')
                selection.directory.pop_back();
        } else if (!g_subprocess_get_if_exited(process) ||
                   g_subprocess_get_exit_status(process) != 1) {
            selection.error = "The directory picker could not open.";
        }
        g_free(output);
        {
            std::lock_guard<std::mutex> lock(mutex);
            result = std::move(selection);
        }
        finished = true;
        const uint64_t wake = 1;
        (void)!write(events, &wake, sizeof(wake));
    }

    ~Impl()
    {
        if (!finished)
            g_subprocess_force_exit(process);
        worker.join();
        g_object_unref(process);
        close(events);
    }
};

DirectoryPicker::DirectoryPicker(const std::string& directory, unsigned long parent)
    : impl(std::make_unique<Impl>(directory, parent))
{
}
DirectoryPicker::~DirectoryPicker() = default;
int DirectoryPicker::event_fd() const
{
    return impl->events;
}
std::optional<DirectorySelection> DirectoryPicker::take_result()
{
    uint64_t wake;
    if (read(impl->events, &wake, sizeof(wake)) < 0)
        return {};
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto result = std::move(impl->result);
    impl->result.reset();
    return result;
}
