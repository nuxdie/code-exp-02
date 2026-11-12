#pragma once

#include "canvas.h"
#include "structure.h"

#include <string>
#include <utility>
#include <vector>

// The names worth showing when a file is too small to read: what it defines, or for prose
// its headings. Read from the text alone, so it works without a language server. Longest
// first, since the longest stay legible furthest out.
std::vector<OutlineEntry> text_outline(const std::string& path,
                                       const std::vector<std::string>& text);

// The same from a language server's symbols, when there are any.
std::vector<OutlineEntry> symbol_outline(const std::vector<SymbolNode>& symbols);

// A folder whose files sit together on the canvas.
struct FolderRegion {
    std::string folder;
    CanvasRect bounds;
};

// One region per folder, from where its files are now. A folder with another folder's file
// in its midst is not a region.
std::vector<FolderRegion>
folder_regions(const std::vector<std::pair<std::string, CanvasRect>>& files);

// Take a place for a label unless it would cover one already placed.
bool claim_label(std::vector<CanvasRect>& taken, const CanvasRect& box);
