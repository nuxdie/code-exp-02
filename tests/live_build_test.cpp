#include "live_build.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
BuildResult wait_build(LiveBuild& builder, uint64_t generation)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{builder.event_fd(), POLLIN, 0};
        poll(&fd, 1, 100);
        for (auto& result : builder.take_results())
            if (!result.started && result.generation == generation)
                return result;
    }
    throw std::runtime_error("Timed out waiting for a viewer build");
}
int main()
{
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-build-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "CMakeLists.txt")
            << "cmake_minimum_required(VERSION 3.16)\nproject(test LANGUAGES "
               "CXX)\nadd_executable(white_window main.cpp)\n";
        std::ofstream(root / "main.cpp") << "int main() { return 0; }\n";
        const auto source = root.string(), build = (root / "build").string();
        const char* args[]{"cmake", "-S", source.c_str(), "-B", build.c_str(), nullptr};
        pid_t child;
        expect(!posix_spawnp(&child, "cmake", nullptr, nullptr, const_cast<char**>(args), environ),
               "Fixture CMake must start");
        int status;
        waitpid(child, &status, 0);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Fixture CMake must configure");
        {
            LiveBuild builder(build);
            builder.request(1);
            expect(wait_build(builder, 1).success,
                   "A successful source edit must produce a new executable");
            const auto binary = root / "build/white_window";
            const auto previous = std::filesystem::last_write_time(binary);
            std::ofstream(root / "main.cpp") << "int main() { this will not compile; }\n";
            builder.request(2);
            const auto failure = wait_build(builder, 2);
            expect(!failure.success && !failure.output.empty() &&
                       std::filesystem::last_write_time(binary) == previous,
                   "A failing edit must preserve the last working executable and report compiler "
                   "output");
            std::ofstream(root / "main.cpp") << "int main() { return 42; }\n";
            builder.request(3);
            expect(wait_build(builder, 3).success &&
                       std::filesystem::last_write_time(binary) != previous,
                   "Correcting an error must resume successful live builds");
        }
        std::filesystem::remove_all(root);
        std::cout << "Successful builds, failed-edit recovery and executable replacement passed.\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
