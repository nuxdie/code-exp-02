#pragma once

#include "canvas.h"

#include <map>
#include <string>
#include <vector>

struct GraphFile {
    std::string path;
    std::string source;
    Vec2 size;
};
struct Dependency {
    int source;
    int target;
    int line;
    bool build = false;
    std::size_t byte = 0;
};
struct GraphLayout {
    std::vector<CanvasRect> files;
    Vec2 size;
};

using FileIndex = std::map<std::string, int>;
std::vector<Dependency> scan_dependencies(const std::vector<GraphFile>& files,
                                          const FileIndex& paths, int source,
                                          std::string_view content);
GraphLayout folder_layout(const std::vector<GraphFile>& files,
                          const std::vector<Dependency>& edges);
std::vector<Dependency> file_dependencies(const std::vector<GraphFile>& files);
