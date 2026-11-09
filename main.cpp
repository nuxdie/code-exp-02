#include "application.h"

#include <cstring>
#include <unistd.h>

int main()
{
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    int result;
    {
        Application application;
        result = application.run();
    }
    if (result == Application::reload_exit_code) {
        execl(executable.c_str(), executable.c_str(), nullptr);
        std::cerr << "Could not reload the viewer: " << std::strerror(errno) << '\n';
        return 1;
    }
    return result;
}
