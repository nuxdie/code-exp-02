#include "application.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

void Application::load_image_panel(SourcePanel& panel, const std::string& bytes, int title_width)
{
    auto* loader = gdk_pixbuf_loader_new();
    // Bound decode memory and texture size while preserving aspect ratio.
    g_signal_connect(loader, "size-prepared",
                     G_CALLBACK(+[](GdkPixbufLoader* decoder, int width, int height, gpointer) {
                         const double scale = std::min(1.0, 2048.0 / std::max(width, height));
                         gdk_pixbuf_loader_set_size(decoder, std::max(1, int(width * scale)),
                                                    std::max(1, int(height * scale)));
                     }),
                     nullptr);
    GError* error = nullptr;
    const bool written = gdk_pixbuf_loader_write(
        loader, reinterpret_cast<const guchar*>(bytes.data()), bytes.size(), &error);
    const bool closed = gdk_pixbuf_loader_close(loader, written ? &error : nullptr);
    auto* decoded = written && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (!decoded) {
        const std::string message = "Cannot display image: " +
                                    std::string(error ? error->message : "unsupported image data");
        auto* label = make_line(message, {});
        panel.bounds.size = {std::max(480.0, double(title_width)),
                             double(header_height + line_height)};
        renderer->upload(panel.mesh, {{label, {0, double(header_height)}}});
        g_object_unref(label);
    } else {
        auto* pixels = gdk_pixbuf_apply_embedded_orientation(decoded);
        const int width = gdk_pixbuf_get_width(pixels), height = gdk_pixbuf_get_height(pixels);
        const int channels = gdk_pixbuf_get_n_channels(pixels);
        const int stride = gdk_pixbuf_get_rowstride(pixels);
        const auto* data = gdk_pixbuf_read_pixels(pixels);
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        auto* output = cairo_image_surface_get_data(surface);
        const int output_stride = cairo_image_surface_get_stride(surface);
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(output + y * output_stride);
            for (int x = 0; x < width; ++x) {
                const auto* pixel = data + y * stride + x * channels;
                const unsigned alpha = channels == 4 ? pixel[3] : 255;
                row[x] = (alpha << 24) | ((pixel[0] * alpha / 255) << 16) |
                         ((pixel[1] * alpha / 255) << 8) | (pixel[2] * alpha / 255);
            }
        }
        cairo_surface_mark_dirty(surface);
        const double scale = std::min(1.0, 640.0 / std::max(width, height));
        panel.image_size = {width * scale, height * scale};
        panel.bounds.size = {std::max(double(title_width), panel.image_size.x),
                             header_height + panel.image_size.y};
        renderer->upload_image(panel.mesh, surface, {{0, double(header_height)}, panel.image_size});
        cairo_surface_destroy(surface);
        g_object_unref(pixels);
    }
    if (error)
        g_error_free(error);
    g_object_unref(loader);
}
