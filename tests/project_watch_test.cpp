#include "project_watch.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
std::vector<ProjectChange> wait_for(ProjectWatcher& watcher)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{watcher.event_fd(), POLLIN, 0};
        if (poll(&fd, 1, 100) > 0) {
            auto changes = watcher.take_changes();
            if (!changes.empty())
                return changes;
        }
    }
    throw std::runtime_error("Timed out waiting for live source changes");
}
int main()
{
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-watch-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(root / "build");
        std::ofstream(root / "main.cpp") << "int one;\n";
        std::ofstream(root / "build/generated.cpp") << "excluded";
        std::filesystem::create_directories(root / "assets");
        const std::string binary_image("PNG\0pixels", 10);
        std::ofstream(root / "assets/photo.PNG", std::ios::binary)
            .write(binary_image.data(), binary_image.size());
        const auto images = read_project(root.string());
        expect(images.at("assets/photo.PNG") == binary_image && project_image("photo.PNG"),
               "Image discovery must preserve binary bytes and accept uppercase extensions");
        std::filesystem::remove_all(root / "assets");
        const auto initial = read_project(root.string());
        expect(initial.size() == 1 && project_source("notes.tex") &&
                   project_source("docs/notes.md"),
               "The live project must include code and prose while excluding generated output");
        {
            ProjectWatcher watcher(root.string(), initial);
            std::ofstream(root / "replacement.tmp") << "int two;\n";
            std::filesystem::rename(root / "replacement.tmp", root / "main.cpp");
            const auto replaced = wait_for(watcher);
            expect(replaced.size() == 1 && replaced[0].source == "int two;\n",
                   "Atomic editor saves of equal-sized content must update the canvas");
            std::filesystem::create_directories(root / "new/sub");
            std::ofstream(root / "new/sub/café.h") << "#pragma once\n";
            const auto added = wait_for(watcher);
            expect(added.size() == 1 && added[0].path == "new/sub/café.h",
                   "New directories and Unicode filenames must be discovered without restarting");
            std::filesystem::remove(root / "main.cpp");
            const auto removed = wait_for(watcher);
            expect(removed.size() == 1 && !removed[0].source,
                   "Deleted files must leave the live canvas");
            std::ofstream(root / "build/generated.cpp") << "still excluded";
            pollfd fd{watcher.event_fd(), POLLIN, 0};
            expect(poll(&fd, 1, 250) == 0, "Build output must not trigger source reload loops");
        }
        std::filesystem::remove_all(root);
        std::cout << "Live project scanning, atomic saves, additions and deletions passed.\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
