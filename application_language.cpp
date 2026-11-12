#include "application.h"

void Application::handle_message(const lsp::Message& message)
{
    if (std::getenv("SOURCE_CANVAS_TRACE_LSP"))
        std::cerr << "LSP response " << message.method << " id=" << message.id << '\n';
    if (message.error.empty() && receive_connections(message))
        return;
    const auto panel = std::find_if(panels.begin(), panels.end(),
                                    [&](const auto& entry) { return entry.uri == message.uri; });
    if (message.version > 0 && (panel == panels.end() || panel->version != message.version))
        return;
    if (!message.error.empty()) {
        language_error(message);
        return;
    }
    if (message.method == "initialized") {
        language_initialized(message);
    } else if (message.method == "textDocument/documentLink") {
        language_links(message);
    } else if (message.method == "documentLink/resolve" && message.id == navigation_request) {
        language_definition(message);
    } else if (message.method == "textDocument/documentSymbol" ||
               message.method == "textDocument/foldingRange") {
        language_structure(message);
    } else if (message.method == "textDocument/semanticTokens/full" ||
               message.method == "textDocument/publishDiagnostics" ||
               message.method == "textDocument/diagnostic") {
        language_styles(message);
    } else if (message.method == "textDocument/hover" && message.id == hover_request) {
        language_hover(message);
    }
}

void Application::language_error(const lsp::Message& message)
{
    std::cerr << "Language feature: " << message.error << '\n';
    if (message.method == "error") {
        language_ready = false;
        language_failure = message.error;
    }
    if (message.id > 0 && (message.id == hover_request || message.id == navigation_request))
        show_popup(message.error, mouse_position);
}

void Application::language_initialized(const lsp::Message& message)
{
    language_ready = true;
    if (message.result.contains("semanticTokensProvider")) {
        const auto& legend = message.result["semanticTokensProvider"]["legend"];
        token_legend = legend["tokenTypes"].get<std::vector<std::string>>();
        const auto modifiers = legend["tokenModifiers"].get<std::vector<std::string>>();
        for (std::size_t i = 0; i < modifiers.size() && i < 32; ++i) {
            if (modifiers[i] == "deprecated")
                deprecated_modifier = 1u << i;
            else if (modifiers[i] == "definition")
                definition_modifier = 1u << i;
            if (modifiers[i] == "declaration" || modifiers[i] == "definition")
                declaration_modifiers |= 1u << i;
        }
    }
}

void Application::language_structure(const lsp::Message& message)
{

    auto found = std::find_if(panels.begin(), panels.end(),
                              [&](const auto& panel) { return panel.uri == message.uri; });
    if (found == panels.end())
        return;
    if (message.method == "textDocument/documentSymbol") {
        found->symbols = symbol_tree(message.result);
    } else if (message.result.is_array()) {
        std::vector<FoldBlock> blocks;
        for (const auto& range : message.result) {
            const int first = range.value("startLine", -1), last = range.value("endLine", -1);
            if (first < 0 || last <= first || last >= static_cast<int>(found->lines.size()))
                continue;
            bool collapsed = false;
            for (const auto& previous : found->folds)
                if (std::abs(previous.first - first) <= 1 && std::abs(previous.last - last) <= 1)
                    collapsed = previous.collapsed;
            blocks.push_back({first, last, collapsed});
        }
        found->folds = std::move(blocks);
        update_mesh(*found);
    }
    redraw = true;
}

void Application::language_styles(const lsp::Message& message)
{

    auto found = std::find_if(panels.begin(), panels.end(),
                              [&](const auto& panel) { return panel.uri == message.uri; });
    if (found == panels.end())
        return;
    if (message.method == "textDocument/semanticTokens/full" && message.result.is_object()) {
        found->semantics = lsp::semantic_tokens(message.result.value("data", lsp::Json::array()),
                                                token_legend, found->text);
    } else if (message.method == "textDocument/publishDiagnostics")
        found->diagnostics = message.result;
    else if (message.method == "textDocument/diagnostic")
        found->diagnostics = message.result.value("items", lsp::Json::array());
    apply_styles(*found);
    redraw = true;
}

void Application::language_hover(const lsp::Message& message)
{

    const auto text =
        message.result.is_object()
            ? hover_documentation(lsp::hover_text(message.result.value("contents", lsp::Json())))
            : "";
    show_popup(hover_prefix + (!hover_prefix.empty() && !text.empty() ? "\n\n" : "") + text,
               hover_origin, true);
}

void Application::language_definition(const lsp::Message& message)
{
    if (message.result.is_object())
        open_link(message.result.value("target", ""));
}
