#pragma once

#include "gpu.h"

#include <string>
#include <vector>

// One thing the code under the pointer relates to.
struct ConnectionLink {
    Vec2 near, far;   // canvas positions: in the focused code, and at the other end
    std::string name; // what is at the far end, when it has a name
    std::string file; // the file it lives in
    std::string uri;  // where a click goes
    int line = 0, column = 0;
    bool uses = true; // the focused code uses it; otherwise it uses the focused code
    int count = 1;
    std::vector<Vec2> sites{}; // every place in the focused code that refers to it
};

// A link's far end as drawn: on the thing itself, or on the window edge nearest to it.
// What lies off screen in one file, drawn on the window edge nearest to it.
struct ConnectionMarker {
    std::vector<std::size_t> links;
    CanvasRect bounds;
    std::string text;
};

// "camera.h  zoom ×2, pan +3": the file, then its most used names.
std::string marker_text(const std::vector<ConnectionLink>& links,
                        const std::vector<std::size_t>& members, const std::string& direction);

GpuPath connection_curve(Vec2 start, Vec2 end, std::vector<Vec2> controls = {});

bool inside(const CanvasRect& view, Vec2 point);

// Where the straight line from a point inside the view to a point outside it leaves the view.
Vec2 view_exit(const CanvasRect& view, Vec2 from, Vec2 to);

// An arrow for the direction of something off screen.
const char* direction_glyph(const CanvasRect& view, Vec2 target);

// A marker's box: touching `anchor` on the view's edge and lying wholly inside the view.
CanvasRect marker_box(const CanvasRect& view, Vec2 anchor, Vec2 size);

// Move overlapping boxes apart along the edge they sit on, keeping them inside the view.
void separate_markers(const CanvasRect& view, std::vector<ConnectionMarker>& markers);
