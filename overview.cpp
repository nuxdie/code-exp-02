#include "overview.h"

#include <filesystem>
#include <map>

namespace {
bool identifier(const std::string& name)
{
    return !name.empty() &&
           (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_');
}

bool blank_or_closer(const std::string& row)
{
    return row.find_first_not_of(" \t})];,") == std::string::npos;
}

std::string defined(const std::vector<std::string>& text, std::size_t line)
{
    const auto& row = text[line];
    const auto first = row.find_first_not_of(" \t");
    if (first == std::string::npos || structure_text::comment(row, first) ||
        structure_text::indent(row) > 8 || row.back() == ';' || row.back() == ',')
        return "";
    static const std::vector<std::string> control{"if", "for", "while", "switch", "catch"};
    const auto name = structure_text::declared(row);
    if (!identifier(name) || structure_text::listed(control, name))
        return "";
    if (!structure_text::named_by_keyword(row, first).empty() || row.back() == '{')
        return name;
    auto next = line + 1;
    while (next < text.size() && text[next].find_first_not_of(" \t") == std::string::npos)
        ++next;
    return next < text.size() && text[next].find_first_not_of(" \t{") == std::string::npos &&
                   text[next].contains('{')
               ? name
               : "";
}

int definition_end(const std::vector<std::string>& text, std::size_t line)
{
    const int depth = structure_text::indent(text[line]);
    auto last = line;
    for (auto next = line + 1; next < text.size(); ++next) {
        const auto& row = text[next];
        if (row.find_first_not_of(" \t") == std::string::npos)
            continue;
        if (structure_text::indent(row) <= depth && !blank_or_closer(row) &&
            row.find_first_not_of(" \t{") != std::string::npos)
            break;
        last = next;
    }
    return static_cast<int>(last);
}

std::vector<OutlineEntry> headings(const std::vector<std::string>& text)
{
    std::vector<OutlineEntry> result;
    bool fenced = false;
    for (std::size_t line = 0; line < text.size(); ++line) {
        const auto& row = text[line];
        if (row.starts_with("```"))
            fenced = !fenced;
        const auto level = row.find_first_not_of('#');
        if (fenced || level == 0 || level == std::string::npos || level > 3 || row[level] != ' ')
            continue;
        if (!result.empty())
            result.back().last = static_cast<int>(line) - 1;
        result.push_back(
            {row.substr(level + 1), static_cast<int>(line), static_cast<int>(text.size()) - 1});
    }
    return result;
}

void longest_first(std::vector<OutlineEntry>& entries)
{
    std::ranges::stable_sort(
        entries, [](const auto& a, const auto& b) { return a.last - a.line > b.last - b.line; });
}

void add_symbols(std::vector<OutlineEntry>& result, const std::vector<SymbolNode>& nodes, int depth)
{
    for (const auto& node : nodes) {
        // LSP kinds: 2 module, 3 namespace, 5 class, 6 method, 9 constructor, 10 enum,
        // 11 interface, 12 function, 23 struct.
        const bool container = node.kind == 2 || node.kind == 3 || node.kind == 5 ||
                               node.kind == 11 || node.kind == 23;
        const bool callable = node.kind == 6 || node.kind == 9 || node.kind == 12;
        if ((container || callable || node.kind == 10) && node.last > node.first)
            result.push_back({node.name.substr(node.name.rfind(':') + 1), node.line, node.last});
        if (container && depth < 2)
            add_symbols(result, node.children, depth + 1);
    }
}
} // namespace

std::vector<OutlineEntry> text_outline(const std::string& path,
                                       const std::vector<std::string>& text)
{
    const auto extension = std::filesystem::path(path).extension().string();
    std::vector<OutlineEntry> result;
    if (extension == ".md" || extension == ".markdown") {
        result = headings(text);
    } else {
        for (std::size_t line = 0; line < text.size(); ++line) {
            const auto name = defined(text, line);
            if (!name.empty())
                result.push_back({name, static_cast<int>(line), definition_end(text, line)});
        }
    }
    longest_first(result);
    return result;
}

std::vector<OutlineEntry> symbol_outline(const std::vector<SymbolNode>& symbols)
{
    std::vector<OutlineEntry> result;
    add_symbols(result, symbols, 0);
    longest_first(result);
    return result;
}

std::vector<FolderRegion>
folder_regions(const std::vector<std::pair<std::string, CanvasRect>>& files)
{
    std::map<std::string, CanvasRect> folders;
    const auto folder_of = [](const std::string& path) {
        return std::filesystem::path(path).parent_path().string();
    };
    for (const auto& [path, bounds] : files) {
        if (bounds.size.x <= 0)
            continue;
        const auto [found, added] = folders.emplace(folder_of(path), bounds);
        if (added)
            continue;
        auto& all = found->second;
        const Vec2 corner{std::max(all.origin.x + all.size.x, bounds.origin.x + bounds.size.x),
                          std::max(all.origin.y + all.size.y, bounds.origin.y + bounds.size.y)};
        all.origin = {std::min(all.origin.x, bounds.origin.x),
                      std::min(all.origin.y, bounds.origin.y)};
        all.size = {corner.x - all.origin.x, corner.y - all.origin.y};
    }
    std::vector<FolderRegion> result;
    for (const auto& [folder, bounds] : folders) {
        const bool mixed = std::ranges::any_of(files, [&](const auto& file) {
            const Vec2 middle{file.second.origin.x + file.second.size.x / 2,
                              file.second.origin.y + file.second.size.y / 2};
            return folder_of(file.first) != folder && middle.x > bounds.origin.x &&
                   middle.x < bounds.origin.x + bounds.size.x && middle.y > bounds.origin.y &&
                   middle.y < bounds.origin.y + bounds.size.y;
        });
        if (!mixed)
            result.push_back({folder, bounds});
    }
    return result;
}

bool claim_label(std::vector<CanvasRect>& taken, const CanvasRect& box)
{
    for (const auto& other : taken)
        if (other.size.x > 0 && box.intersects(other))
            return false;
    taken.push_back(box);
    return true;
}
