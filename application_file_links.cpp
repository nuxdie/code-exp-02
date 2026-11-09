#include "application.h"

namespace {
std::string destination_at(const std::vector<PopupSpan>& spans, std::size_t byte)
{
    for (const auto& span : spans)
        if (span.style == PopupStyle::Link && byte >= span.begin && byte < span.begin + span.length)
            return span.destination;
    return {};
}
} // namespace

std::string Application::text_link_at(Vec2 pointer)
{
    const auto target = text_at(pointer);
    if (target.surface == TextSurface::Source) {
        const auto hit = hit_test(pointer);
        return hit.panel < 0
                   ? ""
                   : destination_at(web_links(panels[hit.panel].text[hit.line]), hit.byte);
    }
    if (target.surface != TextSurface::Chat || !target.layout)
        return {};
    const auto* card = chat_at(pointer);
    const auto found = card->messages.find(target.key);
    if (found == card->messages.end())
        return {};
    int index = 0, trailing = 0;
    if (!pango_layout_xy_to_index(
            target.layout,
            static_cast<int>((pointer.x - target.origin.x) / target.scale * PANGO_SCALE),
            static_cast<int>((pointer.y - target.origin.y) / target.scale * PANGO_SCALE), &index,
            &trailing))
        return {};
    return destination_at(found->second.document.styles, static_cast<std::size_t>(index));
}

bool Application::open_link(const std::string& destination)
{
    if (destination.empty())
        return false;
    if (destination.compare(0, 7, "http://") == 0 || destination.compare(0, 8, "https://") == 0) {
        // GLib passes the URL as one argument and reaps the asynchronous browser launcher.
        const char* arguments[]{"xdg-open", destination.c_str(), nullptr};
        GError* error = nullptr;
        const bool opened = g_spawn_async(nullptr, const_cast<char**>(arguments), nullptr,
                                          G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
        if (error) {
            std::cerr << "Open link: " << error->message << '\n';
            g_error_free(error);
        }
        return opened;
    }
    auto path = destination;
    int line = 0, column = 0;
    auto take_number = [&]() -> std::optional<int> {
        const auto colon = path.find_last_of(':');
        if (colon == std::string::npos || colon + 1 == path.size() ||
            !std::all_of(path.begin() + colon + 1, path.end(),
                         [](unsigned char c) { return std::isdigit(c); }))
            return {};
        const auto number = std::max(0, std::atoi(path.c_str() + colon + 1) - 1);
        path.resize(colon);
        return number;
    };
    if (const auto last = take_number()) {
        line = *last;
        if (const auto previous = take_number()) {
            column = line;
            line = *previous;
        }
    }
    const auto file =
        path.compare(0, 7, "file://") == 0          ? std::filesystem::path(lsp::uri_path(path))
        : std::filesystem::path(path).is_absolute() ? std::filesystem::path(path)
                                                    : std::filesystem::path(workspace_root) / path;
    return jump_to({{"uri", lsp::file_uri(file.lexically_normal().string())},
                    {"range", {{"start", {{"line", line}, {"character", column}}}}}});
}

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
