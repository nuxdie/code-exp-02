#include "camera.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
bool near(Vec2 a, Vec2 b) { return near(a.x, b.x) && near(a.y, b.y); }

int main()
{
    const Viewport viewport{24, 24, 752, 520};
    Camera camera;
    camera.reset(viewport);
    expect(near(camera.world_to_screen({0, 0}, viewport), {24, 24}),
        "Reset must place the document origin at the viewport origin");

    for (double zoom : {Camera::min_zoom, 0.5, 1.0, 2.0, Camera::max_zoom}) {
        camera.zoom = zoom;
        camera.center = {-150, 2034};
        const Vec2 world{123, 4321};
        expect(near(camera.screen_to_world(camera.world_to_screen(world, viewport), viewport), world),
            "World and screen transforms must round trip");

        const auto before = camera.world_to_screen(world, viewport);
        camera.pan({137, -89});
        const auto after = camera.world_to_screen(world, viewport);
        expect(near(after, {before.x + 137, before.y - 89}),
            "Dragging must move content by the pointer delta at every zoom");
    }

    for (Vec2 pointer : {Vec2{24, 24}, Vec2{347, 191}, viewport.center(), Vec2{775, 543}}) {
        camera.reset(viewport);
        camera.pan({-123, 789});
        const auto anchor = camera.screen_to_world(pointer, viewport);
        for (double factor : {1.15, 1 / 1.15, 10000.0, 10000.0, 0.000001, 0.000001}) {
            camera.zoom_at(factor, pointer, viewport);
            expect(near(camera.screen_to_world(pointer, viewport), anchor),
                "Pointer anchor must survive zooming and repeated limit hits");
            expect(camera.zoom >= Camera::min_zoom && camera.zoom <= Camera::max_zoom,
                "Zoom must stay within limits");
        }
    }

    // Interleave drag motion and wheel zoom without releasing the drag.
    for (bool zoom_first : {false, true}) {
        camera.reset(viewport);
        Vec2 pointer{280, 190};
        if (zoom_first) camera.zoom_at(1.15, pointer, viewport);
        const auto grabbed = camera.screen_to_world(pointer, viewport);
        for (double factor : {1.15, 1.15, 1 / 1.15, 1000.0, 0.000001, 1.15}) {
            const Vec2 delta{37, -19};
            camera.pan(delta);
            pointer = {pointer.x + delta.x, pointer.y + delta.y};
            camera.zoom_at(factor, pointer, viewport);
            expect(near(camera.world_to_screen(grabbed, viewport), pointer),
                "Combined pan and zoom must keep the grabbed world point under the pointer");
        }
    }

    const Vec2 document{1600, 14000};
    camera.fit(document, viewport);
    const auto top = camera.world_to_screen({0, 0}, viewport);
    const auto bottom = camera.world_to_screen(document, viewport);
    expect(top.x >= viewport.x - 1e-8 && top.y >= viewport.y - 1e-8 &&
           bottom.x <= viewport.x + viewport.width + 1e-8 &&
           bottom.y <= viewport.y + viewport.height + 1e-8,
        "Fit must place the whole document inside the viewport");
    expect(near(camera.screen_to_world(viewport.center(), viewport), {800, 7000}),
        "Fit must center the document");

    const Viewport resized{24, 24, 1120, 800};
    expect(near(camera.screen_to_world(resized.center(), resized), camera.center),
        "Resizing must preserve the world point at the viewport center");
    camera.home(resized);
    expect(near(camera.world_to_screen({0, 0}, resized), {24, 24}),
        "Home must restore the document origin at the current zoom");
    camera.reset(resized);
    expect(near(camera.zoom, 1), "Reset must restore 100 percent zoom");
    camera.fit({0, 0}, {0, 0, 1, 1});
    expect(std::isfinite(camera.zoom), "Empty content and tiny viewports must be safe");
    expect(viewport.contains({24, 24}) && !viewport.contains({776, 544}),
        "Viewport hit testing must honor its bounds");
    std::cout << "Camera transforms, anchoring, panning, limits, fit and resize checks passed.\n";
}
