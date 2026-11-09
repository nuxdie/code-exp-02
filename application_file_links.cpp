#include "application.h"

bool Application::open_literal_file(const SourceHit& hit)
{
    const auto& source = panels[hit.panel];
    if (!source.cmake)
        return false;
    const auto& line = source.text[hit.line];
    HighlightState state;
    for (const auto& token : highlight(line, state, true))
        if (token.syntax == Syntax::Comment && hit.byte >= token.begin)
            return false;
    std::string target;
    std::size_t longest = 0;
    const auto base =
        std::filesystem::path(workspace_root) / std::filesystem::path(source.path).parent_path();
    for (const auto& panel : panels) {
        const auto absolute =
            (std::filesystem::path(workspace_root) / panel.path).lexically_normal();
        const auto candidate = absolute.lexically_relative(base).generic_string();
        if (candidate.empty())
            continue;
        auto position = line.find(candidate);
        while (position != std::string::npos) {
            const auto end = position + candidate.size();
            const bool left = position == 0 || std::string(" \t\"'();").find(line[position - 1]) !=
                                                   std::string::npos;
            const bool right =
                end == line.size() || std::string(" \t\"'();").find(line[end]) != std::string::npos;
            if (left && right && hit.byte >= position && hit.byte < end &&
                candidate.size() > longest) {
                target = panel.uri;
                longest = candidate.size();
            }
            position = line.find(candidate, end);
        }
    }
    return !target.empty() &&
           jump_to({{"uri", target}, {"range", {{"start", {{"line", 0}, {"character", 0}}}}}});
}
