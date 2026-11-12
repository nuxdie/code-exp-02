#pragma once
#include "gpu.h"
#include "lsp.h"
#include "structure.h"
#include <string>
#include <vector>

struct SourcePanel {
    std::string path;
    std::string uri;
    std::string source;
    bool image = false;
    Vec2 image_size;
    std::string image_error;
    bool cpp = false, cmake = false;
    int version = 1;
    GpuMesh mesh;
    GpuMesh header_mesh;
    std::vector<FoldBlock> folds;
    FoldView folded;
    std::vector<PangoLayout*> fold_labels;
    std::vector<SymbolNode> symbols;
    bool symbols_expanded = false;
    std::vector<OutlineEntry> outline; // for when the file is too small to read
    int outline_key = -1;
    PangoLayout* title = nullptr;
    std::vector<PangoLayout*> lines;
    std::vector<std::string> text;
    std::vector<PangoAttrList*> lexical;
    std::vector<lsp::SemanticToken> semantics;
    lsp::Json diagnostics = lsp::Json::array();
    lsp::Json links = lsp::Json::array();
    CanvasRect bounds{};
};

struct SourceHit {
    int panel = -1;
    int line = -1;
    std::size_t byte = 0;
};

// The blocks of a text that can be folded, found from its braces alone. Long blocks that
// are not inside another start collapsed, so a file opens as an outline of itself.
std::vector<FoldBlock> brace_folds(const std::string& text, const std::string& path);

// How many rows a text takes on its card once its long blocks are collapsed.
int folded_rows(const std::string& text, const std::string& path);

std::string fold_identity(const SourcePanel& panel, int line);
