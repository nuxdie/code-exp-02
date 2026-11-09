#include "application.h"

void Application::handle_message(const lsp::Message& message)
{
    if (std::getenv("SOURCE_CANVAS_TRACE_LSP"))
        std::cerr << "LSP response " << message.method << " id=" << message.id << '\n';
    if (!message.error.empty()) {
        std::cerr << "Language feature: " << message.error << '\n';
        if (message.method == "error") {
            language_ready = false;
            language_failure = message.error;
        }
        if (message.id > 0 && (message.id == hover_request || message.id == navigation_request ||
                               message.id == list_request))
            show_popup(message.error, mouse_position);
        return;
    }
    if (message.method == "initialized") {
        language_initialized(message);
    } else if (message.method == "textDocument/documentSymbol" ||
               message.method == "textDocument/foldingRange") {
        language_structure(message);
    } else if (message.method == "textDocument/semanticTokens/full" ||
               message.method == "textDocument/publishDiagnostics") {
        language_styles(message);
    } else if (message.method == "textDocument/hover" && message.id == hover_request) {
        language_hover(message);
    } else if (message.method == "textDocument/definition" && message.id == navigation_request) {
        language_definition(message);
    } else if (message.id == list_request && message.method == "textDocument/references") {
        language_references(message);
    } else if (message.id == list_request && message.method == "textDocument/completion") {
        language_completion(message);
    }
}

void Application::language_initialized(const lsp::Message& message)
{

    language_ready = true;
    if (message.result.contains("semanticTokensProvider")) {
        const auto& legend = message.result["semanticTokensProvider"]["legend"];
        token_legend = legend["tokenTypes"].get<std::vector<std::string>>();
        const auto modifiers = legend["tokenModifiers"].get<std::vector<std::string>>();
        for (std::size_t i = 0; i < modifiers.size() && i < 32; ++i)
            if (modifiers[i] == "deprecated")
                deprecated_modifier = 1u << i;
            else if (modifiers[i] == "definition")
                definition_modifier = 1u << i;
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
        if (outline_panel == static_cast<int>(found - panels.begin()))
            show_outline(outline_panel);
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
    if (message.method == "textDocument/semanticTokens/full" && message.result.is_object())
        found->semantics = lsp::semantic_tokens(message.result.value("data", lsp::Json::array()),
                                                token_legend, found->text);
    else if (message.method == "textDocument/publishDiagnostics")
        found->diagnostics = message.result;
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

    const auto location = message.result.is_array()
                              ? (message.result.empty() ? lsp::Json() : message.result.front())
                              : message.result;
    if (!location.is_object() || !jump_to(location))
        show_popup("No definition found.", mouse_position);
}

void Application::language_references(const lsp::Message& message)
{

    popup_locations.clear();
    jump_rows.clear();
    list_title = "Usages · click to jump";
    popup_origin = mouse_position;
    list_offset = 0;
    if (message.result.is_array())
        for (const auto& location : message.result) {
            const auto path = lsp::uri_path(location.value("uri", ""));
            const auto line = location["range"]["start"].value("line", 0);
            const auto label =
                std::filesystem::path(path).filename().string() + ":" + std::to_string(line + 1);
            jump_rows.push_back({label, location});
        }
    refresh_list();
}

void Application::language_completion(const lsp::Message& message)
{

    auto items =
        message.result.is_array()
            ? message.result
            : (message.result.is_object() ? message.result.value("items", lsp::Json::array())
                                          : lsp::Json::array());
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return a.value("sortText", a.value("label", "")) <
               b.value("sortText", b.value("label", ""));
    });
    std::string text = "Completion suggestions";
    int count = 0;
    for (const auto& item : items) {
        if (count++ == 12)
            break;
        text += "\n" + item.value("label", "");
    }
    if (items.empty())
        text += "\nNo suggestions at this position.";
    show_popup(text, mouse_position);
}
