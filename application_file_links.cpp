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
    int index = 0, trailing = 0;
    if (!pango_layout_xy_to_index(
            target.layout,
            static_cast<int>((pointer.x - target.origin.x) / target.scale * PANGO_SCALE),
            static_cast<int>((pointer.y - target.origin.y) / target.scale * PANGO_SCALE), &index,
            &trailing))
        return {};
    const auto* card = chat_at(pointer);
    const auto found = card->messages.find(target.key);
    return found == card->messages.end()
               ? ""
               : destination_at(found->second.document.styles, static_cast<std::size_t>(index));
}

bool Application::open_link(const std::string& destination)
{
    if (destination.empty())
        return false;
    if (destination.starts_with("http://") || destination.starts_with("https://")) {
        std::string program = "xdg-open", address = destination;
        std::array<char*, 3> arguments{program.data(), address.data(), nullptr};
        GError* failure = nullptr;
        const bool opened = g_spawn_async(nullptr, arguments.data(), nullptr, G_SPAWN_SEARCH_PATH,
                                          nullptr, nullptr, nullptr, &failure);
        if (const GErrorHandle error{failure})
            std::cerr << "Open link: " << error->message << '\n';
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
    const auto file = path.starts_with("file://") ? std::filesystem::path(lsp::uri_path(path))
                      : std::filesystem::path(path).is_absolute()
                          ? std::filesystem::path(path)
                          : std::filesystem::path(workspace_root) / path;
    return jump_to({{"uri", lsp::file_uri(file.lexically_normal().string())},
                    {"range", {{"start", {{"line", line}, {"character", column}}}}}});
}

lsp::LanguageServer* Application::server_for(const SourcePanel& panel)
{
    return panel.cpp ? language_server.get() : nullptr;
}
bool Application::open_document_link(Vec2 pointer)
{
    const auto literal = text_link_at(pointer);
    if (!literal.empty()) {
        auto* companion = chat_at(pointer);
        const auto before = camera;
        const bool opened = open_link(literal);
        if (opened && companion) {
            companion->follow_camera(before, camera, viewport());
            schedule_save();
        }
        return opened;
    }
    const auto hit = hit_test(pointer);
    if (hit.panel < 0)
        return false;
    const auto& panel = panels[hit.panel];
    const auto column = lsp::byte_to_utf16(panel.text[hit.line], hit.byte);
    for (const auto& link : panel.links) {
        const auto& range = link.at("range");
        const auto& start = range.at("start");
        const auto& end = range.at("end");
        const bool after =
            hit.line > start.value("line", 0) ||
            (hit.line == start.value("line", 0) && column >= start.value("character", 0));
        const bool before = hit.line < end.value("line", 0) || (hit.line == end.value("line", 0) &&
                                                                column < end.value("character", 0));
        if (!after || !before)
            continue;
        const auto target = link.value("target", "");
        if (!target.empty())
            return open_link(target);
        auto* server = server_for(panel);
        if (server) {
            navigation_request = server->request("documentLink/resolve", panel.uri, link);
            return true;
        }
    }
    return false;
}
void Application::language_links(const lsp::Message& message)
{
    const auto found = std::ranges::find(panels, message.uri, &SourcePanel::uri);
    if (found == panels.end())
        return;
    found->links = message.result.is_array() ? message.result : lsp::Json::array();
}
