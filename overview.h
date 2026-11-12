#pragma once

#include "canvas.h"
#include "structure.h"

#include <string>
#include <utility>
#include <vector>

std::vector<OutlineEntry> text_outline(const std::string& path,
                                       const std::vector<std::string>& text);

std::vector<OutlineEntry> symbol_outline(const std::vector<SymbolNode>& symbols);

struct FolderRegion {
    std::string folder;
    CanvasRect bounds;
};

std::vector<FolderRegion>
folder_regions(const std::vector<std::pair<std::string, CanvasRect>>& files);

bool claim_label(std::vector<CanvasRect>& taken, const CanvasRect& box);
