#pragma once
#include "gpu.h"
#include "lsp.h"
#include "structure.h"
#include <librsvg/rsvg.h>
#include <string>
#include <vector>

struct SourcePanel {
    std::string path;
    std::string uri;
    std::string source;
    bool cpp = false, cmake = false;
    int version = 1;
    RsvgHandle* icon = nullptr;
    GpuImage gpu_icon;
    GpuMesh mesh;
    GpuMesh header_mesh;
    std::vector<FoldBlock> folds;
    FoldView folded;
    std::vector<PangoLayout*> fold_labels;
    std::vector<SymbolNode> symbols;
    bool symbols_expanded = false;
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

const char* file_icon(const std::string& filename);

std::string fold_identity(const SourcePanel& panel, int line);
