#pragma once

#include "canvas.h"

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
    std::vector<std::vector<Vec2>> routes;
    Vec2 size;
};

std::vector<Dependency> file_dependencies(const std::vector<GraphFile>& files);
std::string graph_dot(const std::vector<GraphFile>& files, const std::vector<Dependency>& edges);
GraphLayout graph_layout(const std::vector<GraphFile>& files, const std::vector<Dependency>& edges,
                         const std::string& directory);
