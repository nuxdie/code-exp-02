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
        const int column =
            static_cast<int>(std::min_element(heights.begin(), heights.end()) - heights.begin());
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
