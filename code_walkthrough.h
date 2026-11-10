#pragma once

#include "document_view.h"
#include <nlohmann/json.hpp>

struct WalkthroughStep {
    std::string id, path, title, note, cached_source;
    int first = 1, last = 1;
    CanvasRect bounds;
    CanvasRect header_bounds{};
    DocumentView title_view, heading, explanation, location;
    GpuMesh code;
    double code_y = 0;
    bool available = false;
};
struct WalkthroughConnection {
    std::size_t from = 0, to = 0;
    std::string label;
    DocumentView caption;
    Vec2 caption_origin;
};
struct CodeWalkthrough {
    nlohmann::json specification;
    std::string title;
    Vec2 origin;
    CanvasRect bounds;
    CanvasRect header_bounds{};
    DocumentView title_view, heading;
    std::vector<WalkthroughStep> steps;
    std::vector<WalkthroughConnection> connections;
    GpuMesh arrows;
    std::vector<Vec2> arrow_points;
};
