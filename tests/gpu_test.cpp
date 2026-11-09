#include "gpu.h"

#include <epoxy/gl.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void test_clip(GpuRenderer& renderer)
{
    renderer.begin(320, 240);
    renderer.clip({{40, 40}, {80, 60}}, 240);
    renderer.rectangle({{0, 0}, {320, 240}}, {1, 0, 0, 1});
    renderer.clear_clip();
    renderer.rectangle({{200, 200}, {20, 20}}, {0, 1, 0, 1});
    renderer.finish();
    auto pixel = [](int x, int y) {
        std::array<unsigned char, 4> color{};
        glReadPixels(x, 239 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
        return color;
    };
    expect(pixel(50, 50)[1] == 0 && pixel(10, 10)[1] == 255 && pixel(210, 210)[0] == 0,
           "Documentation clipping must contain its content and release the clip for later draws");
}

void test_theme(GpuRenderer& renderer)
{
    renderer.set_dark_theme(true);
    renderer.begin(320, 240);
    renderer.rectangle({{20, 20}, {40, 40}}, {1, 1, 1, 1});
    renderer.rectangle({{70, 20}, {40, 40}}, {0.14f, 0.16f, 0.19f, 1});
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
    auto* context = cairo_create(surface);
    cairo_set_source_rgb(context, 0, 0, 0);
    cairo_paint(context);
    auto icon = renderer.image(surface);
    auto formula = icon;
    formula.monochrome = true;
    GpuMesh images;
    renderer.upload(images, {}, {{icon, {{120, 20}, {16, 16}}}, {formula, {{150, 20}, {16, 16}}}});
    renderer.draw(images, {}, 1);
    renderer.finish();
    auto pixel = [](int x, int y) {
        std::array<unsigned char, 4> color{};
        glReadPixels(x, 239 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
        return color;
    };
    expect(pixel(5, 5)[0] < 35 && pixel(30, 30)[0] < 35 && pixel(80, 30)[0] > 180,
           "Dark theme must provide dark surfaces and legible text colors");
    expect(pixel(125, 25)[0] < 5 && pixel(155, 25)[0] > 200,
           "Dark theme preserves file artwork and makes monochrome LaTeX readable");
    renderer.set_dark_theme(false);
    renderer.begin(320, 240);
    renderer.finish();
    expect(pixel(5, 5)[0] == 255, "Returning to light theme restores the background immediately");
    renderer.release(images);
    cairo_destroy(context);
    cairo_surface_destroy(surface);
}

int main()
{
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return 77;
    try {
        for (const char* face : {"Regular", "Bold", "Italic"}) {
            const auto path = std::string(TEST_FONTS) + "/JetBrainsMono-" + face + ".ttf";
            expect(FcConfigAppFontAddFile(FcConfigGetCurrent(),
                                          reinterpret_cast<const FcChar8*>(path.c_str())),
                   "Bundled fonts must load");
        }
        {
            GpuRenderer renderer(display, DefaultScreen(display), true);
            XResizeWindow(display, renderer.window(), 320, 240);
            XMapWindow(display, renderer.window());
            bool visible = false;
            for (int i = 0; i < 200; ++i) {
                XWindowAttributes attributes{};
                XGetWindowAttributes(display, renderer.window(), &attributes);
                if (attributes.map_state == IsViewable) {
                    visible = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            expect(visible, "GPU test window must be mapped before reading its framebuffer");
            auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            auto* cr = cairo_create(surface);
            auto* layout = pango_cairo_create_layout(cr);
            auto* font = pango_font_description_from_string("JetBrains Mono");
            pango_font_description_set_absolute_size(font, 20 * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            pango_layout_set_text(layout, "a != b -> c && d", -1);
            auto* attributes = pango_attr_list_new();
            pango_attr_list_insert(attributes,
                                   pango_attr_font_features_new("liga=1,calt=1,clig=1"));
            pango_attr_list_insert(attributes, pango_attr_foreground_new(0xffff, 0, 0));
            pango_layout_set_attributes(layout, attributes);
            pango_attr_list_unref(attributes);
            GpuMesh mesh;
            renderer.upload(mesh, {{layout, {0, 0}}});
            const auto cached_glyphs = renderer.glyph_count();
            expect(cached_glyphs > 0, "Shaped glyphs must be uploaded to the GPU atlas");
            renderer.begin(320, 240);
            renderer.draw(mesh, {20, 20}, 1);
            renderer.finish();
            std::vector<unsigned char> pixels(320 * 240 * 4);
            glReadPixels(0, 0, 320, 240, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            int red = 0, white = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 4) {
                if (pixels[i] > 160 && pixels[i + 1] < 120 && pixels[i + 2] < 120)
                    ++red;
                if (pixels[i] == 255 && pixels[i + 1] == 255 && pixels[i + 2] == 255)
                    ++white;
            }
            expect(red > 100 && white > 60000,
                   "GPU text must preserve color and transparent glyph backgrounds");
            for (int frame = 0; frame < 120; ++frame) {
                renderer.begin(320, 240);
                renderer.draw(mesh, {20.0 + frame / 10.0, 20}, 0.1 + frame / 20.0);
                renderer.present();
            }
            renderer.finish();
            expect(renderer.glyph_count() == cached_glyphs,
                   "Zoom and pan must reuse cached glyphs without any rerasterization");
            expect(glGetError() == GL_NO_ERROR, "GPU navigation must not produce OpenGL errors");
            attributes = pango_attr_list_new();
            pango_attr_list_insert(attributes,
                                   pango_attr_font_features_new("liga=0,calt=0,clig=0"));
            pango_layout_set_attributes(layout, attributes);
            pango_attr_list_unref(attributes);
            renderer.upload(mesh, {{layout, {0, 0}}});
            expect(renderer.glyph_count() > cached_glyphs,
                   "The renderer must use Pango's ligature glyphs rather than rendering separate "
                   "characters");
            test_clip(renderer);
            test_theme(renderer);
            std::cout << "GPU atlas, color, ligature and cache reuse checks passed on "
                      << renderer.device() << '\n';
            g_object_unref(layout);
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        }
        XCloseDisplay(display);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        XCloseDisplay(display);
        return 1;
    }
}
