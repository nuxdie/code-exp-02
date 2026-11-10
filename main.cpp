#include "application.h"

#include <cstring>
#include <unistd.h>

int main(int argc, char* argv[])
{
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [directory]\n";
        return 1;
    }
    std::string directory = argc == 2 ? argv[1] : "";
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    int result;
    try {
        Application application(directory);
        result = application.run();
        directory = application.directory();
    } catch (const std::exception& error) {
        std::cerr << "Could not open the workspace: " << error.what() << '\n';
        return 1;
    }
    if (result == Application::reload_exit_code) {
        execl(executable.c_str(), executable.c_str(),
              directory.empty() ? nullptr : directory.c_str(), nullptr);
        std::cerr << "Could not reload the viewer: " << std::strerror(errno) << '\n';
        return 1;
    }
    return result;
}
