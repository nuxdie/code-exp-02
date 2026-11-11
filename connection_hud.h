#pragma once

#include "gpu.h"

#include <string>
#include <vector>

// A word in a file.
struct ConnectionEnd {
    std::string uri;
    int line = 0;
    std::size_t begin = 0, end = 0;
    bool operator==(const ConnectionEnd& other) const
    {
        return uri == other.uri && line == other.line && begin == other.begin;
    }
};

// The symbol the arrows are about: the one that was clicked.
struct ConnectionSubject {
    ConnectionEnd at;
    std::string name, file;
    bool active = false;
};

// One place related to the subject.
struct ConnectionLink {
    ConnectionEnd at;
    std::string name; // what to call it: the symbol, or the function a use sits in
    std::string file;
    bool defines = true; // where the subject is defined; otherwise a place that uses it
    int count = 1;
};

// What lies off screen in one file, drawn on the window edge nearest to it.
// One clickable word of a marker: the file, or a function that uses the subject.
struct MarkerPart {
    std::string text;
    std::size_t link = 0; // the place it flies to
    double offset = 0, width = 0;
};

struct ConnectionMarker {
    std::vector<std::size_t> links; // empty for the subject's own marker
    CanvasRect bounds;
    std::string text;
    std::vector<MarkerPart> parts;
};

GpuPath connection_curve(Vec2 start, Vec2 end, std::vector<Vec2> controls = {});

bool inside(const CanvasRect& view, Vec2 point);
Vec2 center(const CanvasRect& box);

// The point on a box's border facing another point: where an arrow should touch it.
Vec2 border_point(const CanvasRect& box, Vec2 toward);

// Where the straight line from a point inside the view to a point outside it leaves the view.
Vec2 view_exit(const CanvasRect& view, Vec2 from, Vec2 to);

// An arrow for the direction of something off screen.
const char* direction_glyph(const CanvasRect& view, Vec2 target);

// A marker's box: touching `anchor` on the view's edge and lying wholly inside the view.
CanvasRect marker_box(const CanvasRect& view, Vec2 anchor, Vec2 size);

// Move overlapping boxes apart along the edge they sit on, keeping them inside the view.
void separate_markers(const CanvasRect& view, std::vector<ConnectionMarker>& markers);

// "camera.h  zoom, pan +3": the file, then its most frequent names, each its own part.
std::vector<MarkerPart> marker_parts(const std::vector<ConnectionLink>& links,
                                     const std::vector<std::size_t>& members);

// The part of a marker under a point, or null.
const MarkerPart* marker_part_at(const ConnectionMarker& marker, Vec2 point);

// The identifier around a byte of a line, as [begin, end); empty when there is none.
std::pair<std::size_t, std::size_t> word_at(const std::string& line, std::size_t byte);
