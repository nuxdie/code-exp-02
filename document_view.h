#pragma once

#include "gpu.h"
#include "popup_markup.h"

#include <memory>

struct PangoLayoutDeleter {
    void operator()(PangoLayout* layout) const { g_object_unref(layout); }
};
struct DocumentView {
    std::unique_ptr<PangoLayout, PangoLayoutDeleter> layout;
    PopupDocument document;
    GpuMesh mesh;
    std::string source;
    Vec2 size;
    int width = 0;
    unsigned generation = 0;
};
