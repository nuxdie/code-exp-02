#pragma once

#include "canvas.h"

struct CanvasWindowManager {
    Vec2* moving = nullptr;

    static double title_scale(double zoom) { return std::max(0.7, zoom); }
    static bool contains(CanvasRect bounds, Vec2 point)
    {
        return bounds.size.x > 0 && bounds.size.y > 0 && point.x >= bounds.origin.x &&
               point.y >= bounds.origin.y && point.x < bounds.origin.x + bounds.size.x &&
               point.y < bounds.origin.y + bounds.size.y;
    }
    static CanvasRect title_bounds(Vec2 origin, Vec2 size, double zoom, double width,
                                   const std::vector<CanvasRect>& occupied)
    {
        const double scale = title_scale(zoom);
        CanvasRect result{origin, {size.x * scale, size.y * scale}};
        result.origin.x = std::clamp(result.origin.x, 0.0, std::max(0.0, width - result.size.x));
        for (const auto& other : occupied)
            if (other.size.x > 0 && other.size.y > 0 && result.intersects(other))
                result.origin.y = other.origin.y + other.size.y + 2;
        return result;
    }
    void begin(Vec2& origin) { moving = &origin; }
    void move(Vec2 delta, double zoom)
    {
        if (moving) {
            moving->x += delta.x / zoom;
            moving->y += delta.y / zoom;
        }
    }
    void end() { moving = nullptr; }
};
