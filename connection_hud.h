#pragma once

#include "gpu.h"

#include <string>
#include <string_view>
#include <vector>

struct ConnectionEnd {
    std::string uri;
    int line = 0;
    std::size_t begin = 0, end = 0;
    bool operator==(const ConnectionEnd& other) const
    {
        return uri == other.uri && line == other.line && begin == other.begin;
    }
};

struct ConnectionSubject {
    ConnectionEnd at;
    std::string name, file;
    bool active = false;
};

struct ConnectionLink {
    ConnectionEnd at;
    std::string name; // what to call it: the symbol, or the function a use sits in
    std::string file;
    bool defines = true; // where the subject is defined; otherwise a place that uses it
    int count = 1;
};

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

GpuPath connection_curve(Vec2 start, Vec2 end);

bool inside(const CanvasRect& view, Vec2 point);
Vec2 center(const CanvasRect& box);

Vec2 border_point(const CanvasRect& box, Vec2 toward);

Vec2 view_exit(const CanvasRect& view, Vec2 from, Vec2 to);

const char* direction_glyph(const CanvasRect& view, Vec2 target);

CanvasRect marker_box(const CanvasRect& view, Vec2 anchor, Vec2 size);

void separate_markers(const CanvasRect& view, std::vector<ConnectionMarker>& markers);

std::vector<MarkerPart> marker_parts(const std::vector<ConnectionLink>& links,
                                     const std::vector<std::size_t>& members);

const MarkerPart* marker_part_at(const ConnectionMarker& marker, Vec2 point);

std::pair<std::size_t, std::size_t> word_at(std::string_view line, std::size_t byte);
