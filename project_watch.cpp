#include "project_watch.h"

#include "wakeup.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>

namespace {
// Text beyond this is generated or data, not something to read on a canvas.
constexpr std::uintmax_t largest_text = 256 * 1024;

bool excluded_directory(const std::filesystem::path& path)
{
    static const std::set<std::string> names{".git", ".cache", "__pycache__", "node_modules"};
    for (const auto& part : path) {
        const auto name = part.string();
        if (names.count(name) || name.compare(0, 5, "build") == 0)
            return true;
    }
    return false;
}
// The files git tracks or would track, so each project's own ignore rules decide what is shown.
std::vector<std::filesystem::path> git_files(const std::string& root)
{
    std::string quoted = "'";
    for (const char character : root)
        quoted += character == '\'' ? std::string("'\\''") : std::string(1, character);
    const auto command =
        "git -C " + quoted + "' ls-files -z --cached --others --exclude-standard 2>/dev/null";
    std::vector<std::filesystem::path> files;
    auto* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return files;
    std::string output;
    char buffer[65536];
    for (std::size_t count; (count = fread(buffer, 1, sizeof(buffer), pipe)) > 0;)
        output.append(buffer, count);
    if (pclose(pipe) != 0)
        return {};
    for (std::size_t begin = 0; begin < output.size();) {
        const auto end = output.find('\0', begin);
        files.emplace_back(output.substr(begin, end - begin));
        begin = end == std::string::npos ? output.size() : end + 1;
    }
    return files;
}
std::vector<std::filesystem::path> walked_paths(const std::string& root, bool directories)
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
std::vector<std::filesystem::path> project_paths(const std::string& root, bool directories)
{
    const auto tracked = git_files(root);
    if (tracked.empty())
        return walked_paths(root, directories);
    std::set<std::filesystem::path> result;
    std::error_code error;
    for (const auto& file : tracked) {
        if (excluded_directory(file))
            continue;
        if (!directories) {
            if (std::filesystem::is_regular_file(std::filesystem::path(root) / file, error))
                result.insert(file);
            continue;
        }
        for (auto parent = file.parent_path(); !parent.empty(); parent = parent.parent_path())
            result.insert(parent);
    }
    return {result.begin(), result.end()};
}
bool binary_head(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    char head[8192];
    input.read(head, sizeof(head));
    return std::find(head, head + input.gcount(), '\0') != head + input.gcount();
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
    return !excluded_directory(path);
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
        const auto file = std::filesystem::path(root) / relative;
        std::error_code error;
        const auto bytes = std::filesystem::file_size(file, error);
        if (error || (!project_image(path) && (bytes > largest_text || binary_head(file))))
            continue;
        files.push_back({std::move(path), bytes});
    }
    return files;
}

std::optional<std::string> read_project_file(const std::string& root, const std::string& path)
{
    const auto location = std::filesystem::path(root) / path;
    std::error_code error;
    if (!project_image(path) && std::filesystem::file_size(location, error) > largest_text)
        return std::nullopt;
    std::ifstream file(location, std::ios::binary);
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
    static constexpr auto settle = std::chrono::milliseconds(120);
    std::string root;
    ProjectSnapshot snapshot;
    FileDescriptor notify{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
    WakeSignal stop;
    std::set<std::string> watched;
    Mailbox<ProjectChange> changes;
    std::jthread worker;

    Impl(std::string root, ProjectSnapshot initial)
        : root(std::move(root)), snapshot(std::move(initial))
    {
        if (!notify)
            throw std::runtime_error("Could not create project watch descriptors");
        add_watches();
        worker = std::jthread([this](const std::stop_token& stopping) {
            const std::stop_callback wake(stopping, [this] { stop.raise(); });
            run(stopping);
        });
    }
    void add_watches()
    {
        auto directories = project_paths(root, true);
        directories.emplace_back(".");
        for (const auto& directory : directories) {
            const auto path = (std::filesystem::path(root) / directory).string();
            if (watched.contains(path))
                continue;
            const auto mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE |
                              IN_DELETE_SELF | IN_MOVE_SELF;
            if (inotify_add_watch(notify.get(), path.c_str(), mask) >= 0)
                watched.insert(path);
        }
    }
    void rescan()
    {
        auto next = read_project(root);
        changes.post(project_changes(snapshot, next));
        snapshot = std::move(next);
    }
    void run(const std::stop_token& stopping)
    {
        using Clock = std::chrono::steady_clock;
        auto due = Clock::now();
        while (!stopping.stop_requested()) {
            int timeout = -1;
            if (due != Clock::time_point::max()) {
                const auto wait = std::chrono::ceil<std::chrono::milliseconds>(due - Clock::now());
                timeout = static_cast<int>(std::max<std::int64_t>(0, wait.count()));
            }
            std::array<pollfd, 2> fds{{{notify.get(), POLLIN, 0}, {stop.fd(), POLLIN, 0}}};
            if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR)
                return;
            if (fds[1].revents)
                return;
            if (fds[0].revents) {
                alignas(inotify_event) std::array<char, 65536> buffer;
                while (read(notify.get(), buffer.data(), buffer.size()) > 0) {
                }
                // Re-register deleted/recreated directories and refresh newly created trees.
                watched.clear();
                add_watches();
                due = Clock::now() + settle;
            }
            if (Clock::now() >= due) {
                rescan();
                due = Clock::time_point::max();
            }
        }
    }
};
ProjectWatcher::ProjectWatcher(std::string root, ProjectSnapshot initial)
    : impl(std::make_unique<Impl>(std::move(root), std::move(initial)))
{
}
ProjectWatcher::~ProjectWatcher() = default;
int ProjectWatcher::event_fd() const
{
    return impl->changes.fd();
}
std::vector<ProjectChange> ProjectWatcher::take_changes()
{
    return impl->changes.take();
}
