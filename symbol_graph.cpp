#include "symbol_graph.h"

#include <algorithm>

namespace {
bool logical_symbol(const std::string& type)
{
    return type == "function" || type == "method" || type == "class" || type == "struct" ||
           type == "interface" || type == "enum" || type == "type" || type == "enumMember";
}
} // namespace

void SymbolGraph::reset(std::set<std::string> paths)
{
    documents = std::move(paths);
    generations.clear();
    queued.clear();
    edges.clear();
    // Keep in-flight requests until they finish so repeated edits cannot flood the server.
}

void SymbolGraph::update(const std::string& uri, int version,
                         const std::vector<lsp::SemanticToken>& tokens, unsigned declarations)
{
    const auto current = generations[uri] = ++generation;
    queued.erase(std::remove_if(queued.begin(), queued.end(),
                                [&](const auto& query) { return query.uri == uri; }),
                 queued.end());
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [&](const auto& edge) { return edge.source.uri == uri; }),
                edges.end());
    for (const auto& token : tokens) {
        const bool local = logical_symbol(token.type);
        if ((!local && token.type != "variable" && token.type != "property") ||
            (token.modifiers & declarations) || token.line < 0 || token.length == 0)
            continue;
        queued.push_back(
            {uri, version, token.line, token.begin, token.begin + token.length, local, current});
    }
}

std::optional<SymbolQuery> SymbolGraph::next()
{
    if (pending.size() >= 8 || queued.empty())
        return {};
    auto query = std::move(queued.front());
    queued.pop_front();
    return query;
}

void SymbolGraph::started(int id, const SymbolQuery& query)
{
    pending.emplace(id, query);
}

void SymbolGraph::connect(const SymbolQuery& source, const lsp::Json& location)
{
    if (!location.is_object())
        return;
    const auto uri = location.value("uri", location.value("targetUri", ""));
    const auto range =
        location.value("targetSelectionRange", location.value("range", lsp::Json::object()));
    if (!documents.count(uri) || !range.is_object() || !range.contains("start"))
        return;
    const int line = range["start"].value("line", -1);
    const int column = range["start"].value("character", -1);
    if (line < 0 || column < 0 || (!source.local && source.uri == uri))
        return;
    const auto duplicate = std::find_if(edges.begin(), edges.end(), [&](const auto& edge) {
        return edge.source.uri == source.uri && edge.source.line == source.line &&
               edge.source.begin == source.begin && edge.target_uri == uri && edge.line == line &&
               edge.column == column;
    });
    if (duplicate == edges.end())
        edges.push_back({source, uri, line, column});
}

bool SymbolGraph::complete(const lsp::Message& message)
{
    if (message.language != "cpp" || message.method != "textDocument/definition")
        return false;
    const auto found = pending.find(message.id);
    if (found == pending.end())
        return false;
    const auto source = std::move(found->second);
    pending.erase(found);
    const auto current = generations.find(source.uri);
    if (!message.error.empty() || current == generations.end() ||
        source.generation != current->second ||
        (message.version > 0 && message.version != source.version))
        return true;
    if (message.result.is_array()) {
        for (const auto& location : message.result)
            connect(source, location);
    } else {
        connect(source, message.result);
    }
    return true;
}
