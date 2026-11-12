#pragma once

#include "gpu.h"
#include "handles.h"
#include "popup_markup.h"

#include <memory>

struct DocumentView {
    LayoutHandle layout;
    PopupDocument document;
    GpuMesh mesh;
    std::string source;
    Vec2 size;
    int width = 0;
    unsigned generation = 0;
};
