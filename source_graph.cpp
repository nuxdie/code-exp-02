#include "source_graph.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
using FileIndex = std::map<std::string, int>;
std::string include_name(const std::string& line, bool& comment)
{
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
        return {};
    if (comment) {
        const auto end = line.find("*/", first);
        if (end == std::string::npos)
            return {};
        comment = false;
        first = line.find_first_not_of(" \t", end + 2);
        if (first == std::string::npos)
            return {};
    }
    if (line.compare(first, 2, "/*") == 0) {
        comment = line.find("*/", first + 2) == std::string::npos;
        return {};
    }
    if (line[first] != '#')
        return {};
    first = line.find_first_not_of(" \t", first + 1);
    if (first == std::string::npos || line.compare(first, 7, "include") != 0)
        return {};
    first = line.find_first_not_of(" \t", first + 7);
    if (first == std::string::npos || line[first] != '"')
        return {};
    const auto end = line.find('"', first + 1);
    return end == std::string::npos ? std::string{} : line.substr(first + 1, end - first - 1);
}
int include_target(const std::string& name, const std::string& source, const FileIndex& paths)
{
    if (name.empty())
        return -1;
    const auto relative =
        (std::filesystem::path(source).parent_path() / name).lexically_normal().string();
    for (const auto& candidate : {relative, name, name + ".in"}) {
        const auto found = paths.find(candidate);
        if (found != paths.end())
            return found->second;
    }
    return -1;
}
std::vector<int> build_targets(const std::string& line, const FileIndex& paths)
{
    std::vector<int> targets;
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#')
        return targets;
    for (const auto& [path, target] : paths)
        if (line.find(path) != std::string::npos)
            targets.push_back(target);
    return targets;
}
} // namespace
namespace {
std::vector<Dependency> scan_dependencies(const std::vector<GraphFile>& files,
                                          const FileIndex& paths, int source)
{
    std::vector<Dependency> result;
    std::set<int> seen;
    std::istringstream stream(files[source].source);
    std::string line;
    bool comment = false;
    int number = -1;
    const bool build = files[source].path == "CMakeLists.txt";
    while (std::getline(stream, line)) {
        ++number;
        const auto name = build ? std::string{} : include_name(line, comment);
        const auto targets =
            build ? build_targets(line, paths)
                  : std::vector<int>{include_target(name, files[source].path, paths)};
        for (int target : targets) {
            if (target < 0 || target == source || !seen.insert(target).second)
                continue;
            const auto& reference = build ? files[target].path : name;
            const auto end = line.find(reference) + reference.size() + (build ? 0 : 1);
            result.push_back({source, target, number, build, end});
        }
    }
    return result;
}
} // namespace

std::vector<Dependency> file_dependencies(const std::vector<GraphFile>& files)
{
    FileIndex paths;
    for (std::size_t i = 0; i < files.size(); ++i)
        paths[files[i].path] = static_cast<int>(i);
    std::vector<Dependency> result;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto edges = scan_dependencies(files, paths, static_cast<int>(i));
        result.insert(result.end(), edges.begin(), edges.end());
    }
    return result;
}

std::string graph_dot(const std::vector<GraphFile>& files, const std::vector<Dependency>& edges)
{
    std::ostringstream dot;
    dot << "digraph Sources {\n graph [layout=neato, start=42, overlap=prism, sep=\"+72\", "
           "splines=true, outputorder=edgesfirst];\n"
           " node [shape=box, fixedsize=true, margin=0, fontname=\"JetBrains Mono\"];\n";
    for (std::size_t i = 0; i < files.size(); ++i)
        dot << " n" << i << " [label=" << nlohmann::json(files[i].path).dump()
            << ",width=" << std::max(0.1, files[i].size.x / 96)
            << ",height=" << std::max(0.1, files[i].size.y / 96) << "];\n";
    for (const auto& edge : edges)
        dot << " n" << edge.source << " -> n" << edge.target
            << " [tailport=ne,headport=nw,len=4,weight=" << (edge.build ? 0.2 : 1)
            << ",color=\"#aab6c6\"] ;\n";
    dot << "}\n";
    return dot.str();
}

GraphLayout graph_layout(const std::vector<GraphFile>& files, const std::vector<Dependency>& edges,
                         const std::string& directory)
{
    GraphLayout result;
    if (files.empty())
        return result;
    const auto dot_path = directory + "/source-graph.dot";
    const auto plain_path = directory + "/source-graph.plain";
    std::ofstream(dot_path) << graph_dot(files, edges);
    std::vector<std::string> arguments{"neato", "-Tplain", dot_path, "-o", plain_path};
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child;
    if (posix_spawnp(&child, "neato", nullptr, nullptr, argv.data(), environ))
        throw std::runtime_error("Could not start Graphviz neato");
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::runtime_error("Could not wait for Graphviz");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("Graphviz layout failed");
    result.files.resize(files.size());
    result.routes.resize(edges.size());
    std::ifstream plain(plain_path);
    std::string line;
    double graph_height = 0;
    while (std::getline(plain, line)) {
        std::istringstream stream(line);
        std::string kind;
        stream >> kind;
        if (kind == "graph") {
            double scale, width;
            stream >> scale >> width >> graph_height;
            result.size = {width * 96, graph_height * 96};
        } else if (kind == "node") {
            std::string name;
            double x, y, width, height;
            stream >> name >> x >> y >> width >> height;
            const auto index = static_cast<std::size_t>(std::stoi(name.substr(1)));
            if (index < files.size())
                result.files[index] = {{(x - width / 2) * 96, (graph_height - y - height / 2) * 96},
                                       files[index].size};
        } else if (kind == "edge") {
            std::string source, target;
            int count;
            stream >> source >> target >> count;
            const int a = std::stoi(source.substr(1)), b = std::stoi(target.substr(1));
            const auto edge = std::find_if(edges.begin(), edges.end(), [&](const auto& edge) {
                return edge.source == a && edge.target == b;
            });
            if (edge == edges.end())
                continue;
            auto& route = result.routes[edge - edges.begin()];
            for (int i = 0; i < count; ++i) {
                double x, y;
                stream >> x >> y;
                route.push_back({x * 96, (graph_height - y) * 96});
            }
        }
    }
    return result;
}
