#pragma once

#include "document_view.h"
#include "text_input.h"

struct SearchMatch {
    std::string path, uri, preview;
    int line = 0;
    std::size_t byte = 0, length = 0, preview_byte = 0;
    bool filename = false, cmake = false;
    DocumentView label, snippet;
};
struct SearchCanvas {
    bool active = false, more = false;
    TextInput query;
    std::vector<SearchMatch> matches;
    std::size_t selected = 0, offset = 0;
    double editor_scroll = 0;
    CanvasRect bounds{}, editor{};
    std::vector<std::pair<CanvasRect, std::size_t>> hits;
    DocumentView title, input, hint;
};
