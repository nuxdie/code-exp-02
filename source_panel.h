#pragma once
#include "gpu.h"
#include "handles.h"
#include "lsp.h"
#include "structure.h"
#include <string>
#include <string_view>
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
    std::vector<LayoutHandle> fold_labels;
    std::vector<SymbolNode> symbols;
    bool symbols_expanded = false;
    std::vector<OutlineEntry> outline; // for when the file is too small to read
    int outline_key = -1;
    LayoutHandle title;
    std::vector<LayoutHandle> lines;
    std::vector<std::string> text;
    std::vector<AttributesHandle> lexical;
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

std::vector<FoldBlock> brace_folds(std::string_view text, std::string_view path);

int folded_rows(std::string_view text, std::string_view path);

std::string fold_identity(const SourcePanel& panel, int line);
