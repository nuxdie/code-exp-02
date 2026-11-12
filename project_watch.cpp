#include "project_watch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>

namespace {
bool excluded_directory(const std::filesystem::path& path)
{
    for (const auto& part : path) {
        const auto name = part.string();
        if (name == ".git" || name == ".cache" || name == "fonts" || name == "__pycache__" ||
            name.compare(0, 5, "build") == 0)
            return true;
    }
    return false;
}
std::vector<std::filesystem::path> project_paths(const std::string& root, bool directories)
{
    std::vector<std::filesystem::path> result;
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    auto iterator = std::filesystem::recursive_directory_iterator(root, options, error);
    for (auto end = std::filesystem::recursive_directory_iterator(); iterator != end;
         iterator.increment(error)) {
        const auto relative = iterator->path().lexically_relative(root);
        if (excluded_directory(relative)) {
            if (iterator->is_directory(error))
                iterator.disable_recursion_pending();
            continue;
        }
        if (directories ? iterator->is_directory(error) : iterator->is_regular_file(error))
            result.push_back(relative);
    }
    return result;
}
void signal_fd(int fd)
{
    const uint64_t value = 1;
    (void)!write(fd, &value, sizeof(value));
}
void drain_fd(int fd)
{
    uint64_t value;
    while (read(fd, &value, sizeof(value)) > 0) {
    }
}
} // namespace

bool project_image(const std::string& path)
{
    auto extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::set<std::string> images{".png",  ".jpg", ".jpeg", ".gif",  ".bmp",
                                              ".webp", ".svg", ".tif",  ".tiff", ".ico"};
    return images.count(extension) != 0;
}
bool project_source(const std::string& path)
{
    if (excluded_directory(path))
        return false;
    const auto file = std::filesystem::path(path);
    const auto name = file.filename().string();
    static const std::set<std::string> extensions{".cpp",  ".h",  ".hpp",   ".cc", ".cxx",
                                                  ".c",    ".in", ".cmake", ".py", ".yml",
                                                  ".yaml", ".md", ".tex"};
    return project_image(path) || name == "CMakeLists.txt" || name == ".clang-format" ||
           name == ".clang-tidy" || name == ".cmake-format.json" ||
           extensions.count(file.extension().string()) != 0;
}
std::pair<int, int> text_extent(const std::string& text)
{
    int lines = 0, columns = 0, width = 0;
    for (const char character : text) {
        if (character == '\n') {
            ++lines;
            columns = std::max(columns, width);
            width = 0;
        } else if (character == '\t') {
            width += 8 - width % 8;
        } else if ((static_cast<unsigned char>(character) & 0xC0) != 0x80) {
            ++width;
        }
    }
    if (!text.empty() && text.back() != '\n')
        ++lines;
    return {lines, std::max(columns, width)};
}

std::vector<ProjectFile> list_project_files(const std::string& root)
{
    std::vector<ProjectFile> files;
    for (const auto& relative : project_paths(root, false)) {
        auto path = relative.string();
        if (!project_source(path))
            continue;
        std::error_code error;
        const auto bytes =
            std::filesystem::file_size(std::filesystem::path(root) / relative, error);
        files.push_back({std::move(path), error ? 0 : bytes});
    }
    return files;
}

std::optional<std::string> read_project_file(const std::string& root, const std::string& path)
{
    std::ifstream file(std::filesystem::path(root) / path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(file), {}};
    if (project_image(path) || text.find('\0') == std::string::npos)
        return text;
    return std::nullopt;
}

ProjectSnapshot read_project(const std::string& root)
{
    ProjectSnapshot snapshot;
    for (const auto& relative : project_paths(root, false)) {
        auto path = relative.string();
        if (project_source(path))
            if (auto text = read_project_file(root, path))
                snapshot.emplace(std::move(path), std::move(*text));
    }
    return snapshot;
}
std::vector<ProjectChange> project_changes(const ProjectSnapshot& before,
                                           const ProjectSnapshot& after)
{
    std::vector<ProjectChange> changes;
    for (const auto& [path, text] : after) {
        const auto old = before.find(path);
        if (old == before.end() || old->second != text)
            changes.push_back({path, text});
    }
    for (const auto& [path, text] : before)
        if (!after.count(path))
            changes.push_back({path, std::nullopt});
    return changes;
}

struct ProjectWatcher::Impl {
    std::string root;
    ProjectSnapshot snapshot;
    int notify = -1, events = -1, stop = -1;
    std::set<std::string> watched;
    std::mutex mutex;
    std::vector<ProjectChange> changes;
    std::thread worker;

    Impl(std::string root, ProjectSnapshot initial)
        : root(std::move(root)), snapshot(std::move(initial))
    {
        notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        stop = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (notify < 0 || events < 0 || stop < 0)
            throw std::runtime_error("Could not create project watch descriptors");
        add_watches();
        worker = std::thread([this] { run(); });
    }
    void add_watches()
    {
        auto directories = project_paths(root, true);
        directories.emplace_back(".");
        for (const auto& directory : directories) {
            const auto path = (std::filesystem::path(root) / directory).string();
            if (watched.count(path))
                continue;
            const auto mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE |
                              IN_DELETE_SELF | IN_MOVE_SELF;
            if (inotify_add_watch(notify, path.c_str(), mask) >= 0)
                watched.insert(path);
        }
    }
    void rescan()
    {
        auto next = read_project(root);
        auto batch = project_changes(snapshot, next);
        snapshot = std::move(next);
        if (batch.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            changes.insert(changes.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
        }
        signal_fd(events);
    }
    void run()
    {
        using Clock = std::chrono::steady_clock;
        auto due = Clock::now();
        for (;;) {
            const int timeout =
                due == Clock::time_point::max()
                    ? -1
                    : static_cast<int>(std::max<int64_t>(
                          0,
                          std::chrono::duration_cast<std::chrono::milliseconds>(due - Clock::now())
                              .count()));
            pollfd fds[]{{notify, POLLIN, 0}, {stop, POLLIN, 0}};
            if (poll(fds, 2, timeout) < 0 && errno != EINTR)
                return;
            if (fds[1].revents)
                return;
            if (fds[0].revents) {
                alignas(inotify_event) char buffer[65536];
                while (read(notify, buffer, sizeof(buffer)) > 0) {
                }
                // Re-register deleted/recreated directories and refresh newly created trees.
                watched.clear();
                add_watches();
                due = Clock::now() + std::chrono::milliseconds(120);
            }
            if (Clock::now() >= due) {
                rescan();
                due = Clock::time_point::max();
            }
        }
    }
    ~Impl()
    {
        signal_fd(stop);
        worker.join();
        close(notify);
        close(events);
        close(stop);
    }
};
ProjectWatcher::ProjectWatcher(std::string root, ProjectSnapshot initial)
    : impl(std::make_unique<Impl>(std::move(root), std::move(initial)))
{
}
ProjectWatcher::~ProjectWatcher() = default;
int ProjectWatcher::event_fd() const
{
    return impl->events;
}
std::vector<ProjectChange> ProjectWatcher::take_changes()
{
    drain_fd(impl->events);
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<ProjectChange> changes;
    changes.swap(impl->changes);
    return changes;
}
