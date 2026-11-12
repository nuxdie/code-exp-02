#pragma once

#include <algorithm>
#include <cmath>

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
        return point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
    }
};

struct Camera {
    static constexpr double min_zoom = 0.005;
    static constexpr double max_zoom = 8.0;
    Vec2 center;
    double zoom = 1;

    Vec2 screen_to_world(Vec2 point, const Viewport& viewport) const
    {
        const auto origin = viewport.center();
        return {center.x + (point.x - origin.x) / zoom, center.y + (point.y - origin.y) / zoom};
    }

    Vec2 world_to_screen(Vec2 point, const Viewport& viewport) const
    {
        const auto origin = viewport.center();
        return {origin.x + (point.x - center.x) * zoom, origin.y + (point.y - center.y) * zoom};
    }

    void pan(Vec2 screen_delta)
    {
        center.x -= screen_delta.x / zoom;
        center.y -= screen_delta.y / zoom;
    }

    void set_zoom_at(double value, Vec2 pointer, const Viewport& viewport)
    {
        const auto anchor = screen_to_world(pointer, viewport);
        zoom = std::clamp(value, min_zoom, max_zoom);
        const auto origin = viewport.center();
        center = {anchor.x - (pointer.x - origin.x) / zoom,
                  anchor.y - (pointer.y - origin.y) / zoom};
    }

    void zoom_at(double factor, Vec2 pointer, const Viewport& viewport)
    {
        set_zoom_at(zoom * factor, pointer, viewport);
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

struct ZoomAnimation {
    double target = 1;
    Vec2 anchor;

    bool active(const Camera& camera) const { return target != camera.zoom; }
    void cancel(const Camera& camera) { target = camera.zoom; }

    void request(const Camera& camera, double factor, Vec2 pointer)
    {
        if (!active(camera))
            target = camera.zoom;
        target = std::clamp(target * factor, Camera::min_zoom, Camera::max_zoom);
        anchor = pointer;
    }

    bool advance(Camera& camera, const Viewport& viewport, double seconds) const
    {
        if (!active(camera) || seconds <= 0)
            return false;
        const double remaining = std::log(target / camera.zoom);
        const double next = std::abs(remaining) < 0.0001
                                ? target
                                : camera.zoom * std::exp(remaining * -std::expm1(-24 * seconds));
        camera.set_zoom_at(next, anchor, viewport);
        return true;
    }
};
