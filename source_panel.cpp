#include "source_panel.h"
#include <filesystem>

const char* file_icon(const std::string& filename)
{
    auto path = std::filesystem::path(filename);
    if (path.filename() == ".clang-format" || path.filename() == ".clang-tidy")
        return "file_type_yaml.svg";
    if (path.extension() == ".in")
        path = path.stem();
    const auto ext = path.extension().string();
    if (ext == ".py")
        return "file_type_python.svg";
    if (ext == ".yml" || ext == ".yaml")
        return "file_type_yaml.svg";
    if (path.filename() == "CMakeLists.txt" || ext == ".cmake")
        return "file_type_cmake.svg";
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx")
        return "file_type_cppheader.svg";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c")
        return "file_type_cpp.svg";
    return "default_file.svg";
}
