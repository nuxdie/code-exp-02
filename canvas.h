#pragma once

#include "camera.h"

#include <algorithm>
#include <vector>

struct CanvasRect {
    Vec2 origin;
    Vec2 size;

    bool intersects(const CanvasRect& other) const
    {
        return origin.x < other.origin.x + other.size.x && origin.x + size.x > other.origin.x &&
               origin.y < other.origin.y + other.size.y && origin.y + size.y > other.origin.y;
    }
};

// Stack shorter files in the shortest column, keeping the files independent.
inline std::vector<CanvasRect> arrange_files(const std::vector<Vec2>& sizes, int columns = 3,
                                             double gap = 96)
{
    if (sizes.empty())
        return {};
    columns = std::clamp(columns, 1, static_cast<int>(sizes.size()));
    std::vector<double> heights(columns, 0);
    std::vector<double> widths(columns, 0);
    std::vector<int> assignments;
    std::vector<CanvasRect> result;
    for (const auto size : sizes) {
        const int column = static_cast<int>(std::ranges::min_element(heights) - heights.begin());
        assignments.push_back(column);
        result.push_back({{0, heights[column]}, size});
        heights[column] += size.y + gap;
        widths[column] = std::max(widths[column], size.x);
    }
    std::vector<double> offsets(columns, 0);
    for (int column = 1; column < columns; ++column)
        offsets[column] = offsets[column - 1] + widths[column - 1] + gap;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i].origin.x = offsets[assignments[i]];
    return result;
}

inline Vec2 canvas_size(const std::vector<CanvasRect>& files)
{
    Vec2 size;
    for (const auto& file : files) {
        size.x = std::max(size.x, file.origin.x + file.size.x);
        size.y = std::max(size.y, file.origin.y + file.size.y);
    }
    return size;
}

// Place an automatically opened window beside its anchor without covering existing windows.
inline CanvasRect place_canvas_window(CanvasRect bounds, const std::vector<CanvasRect>& occupied,
                                      double gap = 24)
{
    bool moved;
    do {
        moved = false;
        for (const auto& other : occupied)
            if (bounds.intersects(other)) {
                bounds.origin.x = other.origin.x + other.size.x + gap;
                moved = true;
            }
    } while (moved);
    return bounds;
}

// Shared scrollbar geometry and drag mapping for canvas windows.
inline CanvasRect canvas_scrollbar_thumb(CanvasRect track, double content, double scroll)
{
    if (track.size.y <= 0 || content <= track.size.y)
        return {track.origin, {track.size.x, 0}};
    const double size =
        std::min(track.size.y, std::max(24.0, track.size.y * track.size.y / content));
    const double fraction = std::clamp(scroll / (content - track.size.y), 0.0, 1.0);
    return {{track.origin.x, track.origin.y + fraction * (track.size.y - size)},
            {track.size.x, size}};
}
inline double canvas_scrollbar_drag(CanvasRect track, double content, double scroll,
                                    double pointer_y, double grab_fraction)
{
    const auto thumb = canvas_scrollbar_thumb(track, content, scroll);
    const double travel = track.size.y - thumb.size.y;
    if (travel <= 0)
        return scroll;
    return std::clamp((pointer_y - track.origin.y - grab_fraction * thumb.size.y) / travel, 0.0,
                      1.0) *
           std::max(0.0, content - track.size.y);
}
