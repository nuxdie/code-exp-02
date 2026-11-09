#pragma once

#include <algorithm>

struct Vec2 {
    double x = 0;
    double y = 0;
};

struct Viewport {
    double x;
    double y;
    double width;
    double height;

    Vec2 center() const { return {x + width / 2, y + height / 2}; }

    bool contains(Vec2 point) const
    {
        return point.x >= x && point.y >= y &&
               point.x < x + width && point.y < y + height;
    }
};

// World coordinates stay independent of the window and its pixel density.
struct Camera {
    static constexpr double min_zoom = 0.02;
    static constexpr double max_zoom = 8.0;
    Vec2 center;
    double zoom = 1;

    Vec2 screen_to_world(Vec2 point, const Viewport& viewport) const
    {
        const auto origin = viewport.center();
        return {center.x + (point.x - origin.x) / zoom,
                center.y + (point.y - origin.y) / zoom};
    }

    Vec2 world_to_screen(Vec2 point, const Viewport& viewport) const
    {
        const auto origin = viewport.center();
        return {origin.x + (point.x - center.x) * zoom,
                origin.y + (point.y - center.y) * zoom};
    }

    void pan(Vec2 screen_delta)
    {
        center.x -= screen_delta.x / zoom;
        center.y -= screen_delta.y / zoom;
    }

    // Preserve the exact world point beneath the pointer, including at limits.
    void zoom_at(double factor, Vec2 pointer, const Viewport& viewport)
    {
        const auto anchor = screen_to_world(pointer, viewport);
        zoom = std::clamp(zoom * factor, min_zoom, max_zoom);
        const auto origin = viewport.center();
        center = {anchor.x - (pointer.x - origin.x) / zoom,
                  anchor.y - (pointer.y - origin.y) / zoom};
    }

    void home(const Viewport& viewport)
    {
        center = {viewport.width / (2 * zoom), viewport.height / (2 * zoom)};
    }

    void reset(const Viewport& viewport)
    {
        zoom = 1;
        home(viewport);
    }

    void fit(Vec2 content_size, const Viewport& viewport)
    {
        zoom = std::clamp(std::min(viewport.width / std::max(1.0, content_size.x),
                                  viewport.height / std::max(1.0, content_size.y)),
                          min_zoom, max_zoom);
        center = {content_size.x / 2, content_size.y / 2};
    }
};
