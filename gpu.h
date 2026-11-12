#pragma once

#include "canvas.h"

#include <X11/Xlib.h>
#include <cairo.h>
#include <pango/pango.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

struct GpuMesh {
    unsigned buffer = 0;
    int vertices = 0;
    unsigned texture = 0;
};

struct GpuImage {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool monochrome = false;
};

struct PlacedText {
    PangoLayout* layout;
    Vec2 origin;
};

struct PlacedImage {
    GpuImage image;
    CanvasRect bounds;
};

struct GpuPath {
    std::vector<Vec2> points;
    std::array<float, 4> color;
    float width = 1;
    bool arrow = true;
};

// Shapes are cached in VRAM. Camera movement never calls the text rasterizer.
class GpuRenderer {
  public:
    GpuRenderer(Display* display, int screen);
    ~GpuRenderer();
    Window window() const;
    std::string device() const;
    GpuImage image(cairo_surface_t* surface);
    void upload(GpuMesh& mesh, const std::vector<PlacedText>& text,
                const std::vector<PlacedImage>& images = {});
    void release(GpuMesh& mesh);
    void upload_image(GpuMesh& mesh, cairo_surface_t* surface, CanvasRect bounds);
    void paths(GpuMesh& mesh, const std::vector<GpuPath>& paths);
    void set_dark_theme(bool dark);
    void begin(int width, int height);
    void draw(const GpuMesh& mesh, Vec2 screen_origin, double zoom);
    void rectangle(CanvasRect screen_bounds, std::array<float, 4> color);
    void clip(CanvasRect screen_bounds, int viewport_height);
    void clear_clip();
    void present();
    void finish();
    void window_destroyed();
    std::size_t glyph_count() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
