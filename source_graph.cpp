#include "source_graph.h"

#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace {
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

std::vector<Dependency> scan_dependencies(const std::vector<GraphFile>& files,
                                          const FileIndex& paths, int source,
                                          const std::string& content)
{
    std::vector<Dependency> result;
    std::set<int> seen;
    std::istringstream stream(content);
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

std::vector<Dependency> file_dependencies(const std::vector<GraphFile>& files)
{
    FileIndex paths;
    for (std::size_t i = 0; i < files.size(); ++i)
        paths[files[i].path] = static_cast<int>(i);
    std::vector<Dependency> result;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto edges = scan_dependencies(files, paths, static_cast<int>(i), files[i].source);
        result.insert(result.end(), edges.begin(), edges.end());
    }
    return result;
}

namespace {
// Pull connected items together while anchoring each to its original order.
std::vector<double> relax_order(std::vector<double> rank, const std::vector<Dependency>& edges)
{
    if (edges.empty())
        return rank;
    const auto anchor = rank;
    for (int step = 0; step < 4; ++step) {
        auto sum = anchor;
        std::vector<double> weight(rank.size(), 1);
        for (const auto& edge : edges) {
            const double strength = edge.build ? 0.15 : 1;
            sum[edge.source] += strength * rank[edge.target];
            sum[edge.target] += strength * rank[edge.source];
            weight[edge.source] += strength;
            weight[edge.target] += strength;
        }
        for (std::size_t i = 0; i < rank.size(); ++i)
            rank[i] = sum[i] / weight[i];
    }
    return rank;
}
} // namespace

namespace {
// Folders stand apart by enough for a name to fit between them when the whole project is in
// view, which takes more canvas the bigger the project is.
double folder_gap(const std::vector<Vec2>& folders)
{
    double area = 0;
    for (const auto size : folders)
        area += size.x * size.y;
    return std::clamp(0.06 * std::sqrt(area), 320.0, 1400.0);
}

// The number of columns that shapes the block most like a window, so a fitted view uses the
// screen instead of a strip of it.
int window_columns(const std::vector<Vec2>& sizes, double gap)
{
    int best = 1;
    double error = 1e9;
    for (int columns = 1; columns <= static_cast<int>(sizes.size()); ++columns) {
        const auto size = canvas_size(arrange_files(sizes, columns, gap));
        const double off = std::abs(std::log(size.x / std::max(1.0, size.y) / 1.7));
        if (off < error) {
            error = off;
            best = columns;
        }
    }
    return best;
}
// One folder's files in columns. Unusually wide files go last, so they share columns
// instead of widening everyone's.
std::vector<CanvasRect> pack_folder(const std::vector<GraphFile>& files,
                                    std::vector<std::size_t>& members)
{
    std::vector<Vec2> widths;
    for (const auto index : members)
        widths.push_back(files[index].size);
    std::nth_element(widths.begin(), widths.begin() + widths.size() / 2, widths.end(),
                     [](Vec2 a, Vec2 b) { return a.x < b.x; });
    const double wide = 1.8 * widths[widths.size() / 2].x;
    std::stable_partition(members.begin(), members.end(),
                          [&](auto index) { return files[index].size.x <= wide; });
    std::vector<Vec2> usual, broad;
    for (const auto index : members)
        (files[index].size.x <= wide ? usual : broad).push_back(files[index].size);
    auto packed = arrange_files(usual, window_columns(usual, 96));
    const double right = usual.empty() ? 0 : canvas_size(packed).x + 96;
    for (auto file : arrange_files(broad, window_columns(broad, 96))) {
        file.origin.x += right;
        packed.push_back(file);
    }
    return packed;
}
} // namespace

GraphLayout folder_layout(const std::vector<GraphFile>& files, const std::vector<Dependency>& edges)
{
    std::map<std::filesystem::path, std::vector<std::size_t>> folders;
    for (std::size_t i = 0; i < files.size(); ++i)
        if (files[i].size.x > 0)
            folders[std::filesystem::path(files[i].path).parent_path()].push_back(i);

    std::vector<std::vector<std::size_t>> groups;
    std::vector<int> group_for(files.size(), -1);
    std::vector<double> file_order(files.size()), group_order;
    for (auto& [path, members] : folders) {
        const auto group = static_cast<int>(groups.size());
        for (std::size_t i = 0; i < members.size(); ++i) {
            group_for[members[i]] = group;
            file_order[members[i]] = static_cast<double>(i);
        }
        group_order.push_back(group);
        groups.push_back(std::move(members));
    }

    std::vector<Dependency> within_folder, between_folders;
    for (const auto& edge : edges) {
        const int from = group_for[edge.source], to = group_for[edge.target];
        if (from < 0 || to < 0)
            continue;
        if (from == to)
            within_folder.push_back(edge);
        else
            between_folders.push_back({from, to, 0, edge.build});
    }
    file_order = relax_order(std::move(file_order), within_folder);
    group_order = relax_order(std::move(group_order), between_folders);

    GraphLayout result;
    result.files.resize(files.size());
    std::vector<std::size_t> order(groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](auto a, auto b) { return group_order[a] < group_order[b]; });
    std::vector<Vec2> group_sizes;
    for (const auto group : order) {
        auto& members = groups[group];
        std::stable_sort(members.begin(), members.end(),
                         [&](auto a, auto b) { return file_order[a] < file_order[b]; });
        const auto packed = pack_folder(files, members);
        group_sizes.push_back(canvas_size(packed));
        for (std::size_t i = 0; i < members.size(); ++i)
            result.files[members[i]] = packed[i];
    }
    const double gap = folder_gap(group_sizes);
    const auto packed_groups = arrange_files(group_sizes, window_columns(group_sizes, gap), gap);
    // Room around the canvas for a region's ground, and above it for the first row of names.
    for (std::size_t i = 0; i < order.size(); ++i)
        for (const auto index : groups[order[i]]) {
            result.files[index].origin.x += packed_groups[i].origin.x + gap / 4;
            result.files[index].origin.y += packed_groups[i].origin.y + gap * 0.65;
        }
    result.size = canvas_size(result.files);
    result.size.x += gap / 4;
    result.size.y += gap / 4;
    return result;
}
