#include "application.h"

void Application::initialize_cmake()
{
    if (benchmark)
        return;
    std::vector<lsp::Document> cmake_documents;
    for (const auto& panel : panels)
        if (panel.cmake) {
            cmake_documents.push_back({panel.uri, panel.source});
            opened_documents.insert(panel.uri);
        }
    try {
        cmake_server = std::make_unique<lsp::LanguageServer>(
            cmake_lsp_executable, workspace_root, workspace_build, std::move(cmake_documents),
            lsp::ServerOptions{"cmake", {"stdio"}});
    } catch (const std::exception& error) {
        cmake_failure = error.what();
    }
}
lsp::LanguageServer* Application::server_for(const SourcePanel& panel)
{
    return panel.cpp ? language_server.get() : panel.cmake ? cmake_server.get() : nullptr;
}
bool Application::open_document_link(Vec2 pointer)
{
    const auto literal = text_link_at(pointer);
    if (!literal.empty())
        return open_link(literal);
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
    return open_literal_file(hit);
}
void Application::language_links(const lsp::Message& message)
{
    const auto found = std::find_if(panels.begin(), panels.end(),
                                    [&](const auto& panel) { return panel.uri == message.uri; });
    if (found == panels.end())
        return;
    found->links = message.result.is_array() ? message.result : lsp::Json::array();
}
