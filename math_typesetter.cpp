#include "math_typesetter.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {
bool run_tool(std::vector<std::string> arguments, const std::string& directory)
{
    std::vector<std::string> command{
        "env",     "openin_any=p", "openout_any=p",  "shell_escape=f",
        "prlimit", "--cpu=3",      "--as=536870912", "--fsize=16777216",
        "--"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : command)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t child = -1;
    const int error = posix_spawnp(&child, "env", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error)
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}

TypesetMath typeset(const std::string& expression, bool display)
{
    TypesetMath result{MathTypesetter::key(expression, display), {}, {}};
    auto pattern = (std::filesystem::temp_directory_path() / "source-math-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
        result.error = "Could not create a typesetting directory";
        return result;
    }
    const std::string document = "\\documentclass[12pt]{article}\n\\usepackage{amsmath,amssymb}\n"
                                 "\\pagestyle{empty}\n\\begin{document}\n\\(" +
                                 std::string(display ? "\\displaystyle " : "\\textstyle ") +
                                 expression + "\\)\n\\end{document}\n";
    std::ofstream(pattern + "/formula.tex") << document;
    const bool compiled = run_tool(
        {"latex", "-no-shell-escape", "-interaction=nonstopmode", "-halt-on-error", "formula.tex"},
        pattern);
    const bool converted =
        compiled && run_tool({"dvisvgm", "--no-fonts", "--bbox=min", "--exact", "--scale=1.45",
                              "--output=formula.svg", "formula.dvi"},
                             pattern);
    if (converted) {
        std::ifstream svg(pattern + "/formula.svg");
        result.svg.assign(std::istreambuf_iterator<char>(svg), {});
    }
    if (result.svg.empty())
        result.error = "Could not typeset this equation";
    std::error_code ignored;
    std::filesystem::remove_all(pattern, ignored);
    return result;
}
} // namespace

struct MathTypesetter::Impl {
    struct Request {
        std::string expression;
        bool display;
    };
    int ready_fd = -1;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Request> requests;
    std::vector<TypesetMath> results;
    std::set<std::string> requested;
    bool stopping = false;
    std::thread worker;

    Impl()
    {
        ready_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (ready_fd < 0)
            throw std::runtime_error("Could not create math event fd");
        worker = std::thread([this] { run(); });
    }
    void run()
    {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || !requests.empty(); });
                if (stopping)
                    return;
                request = std::move(requests.front());
                requests.pop_front();
            }
            auto result = typeset(request.expression, request.display);
            {
                std::lock_guard<std::mutex> lock(mutex);
                results.push_back(std::move(result));
            }
            const uint64_t signal = 1;
            const auto ignored = write(ready_fd, &signal, sizeof(signal));
            (void)ignored;
        }
    }
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_one();
        worker.join();
        close(ready_fd);
    }
};

MathTypesetter::MathTypesetter() : impl(std::make_unique<Impl>()) {}
MathTypesetter::~MathTypesetter() = default;
std::string MathTypesetter::key(const std::string& expression, bool display)
{
    return (display ? "display:" : "inline:") + expression;
}
void MathTypesetter::request(const std::string& expression, bool display)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->requested.insert(key(expression, display)).second)
        return;
    impl->requests.push_back({expression, display});
    impl->condition.notify_one();
}
int MathTypesetter::event_fd() const
{
    return impl->ready_fd;
}
std::vector<TypesetMath> MathTypesetter::take_results()
{
    uint64_t signal;
    while (read(impl->ready_fd, &signal, sizeof(signal)) > 0) {
    }
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto results = std::move(impl->results);
    impl->results.clear();
    return results;
}
