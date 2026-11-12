#include "application.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

namespace {
// GIF blocks are length-prefixed. Stop at the first image's data terminator.
std::size_t first_gif_frame(const std::string& bytes)
{
    if (bytes.size() < 13 || (bytes.compare(0, 6, "GIF87a") && bytes.compare(0, 6, "GIF89a")))
        return 0;
    const auto table_size = [](unsigned char packed) {
        return (packed & 0x80) ? (3u << ((packed & 7) + 1)) : 0u;
    };
    std::size_t offset = 13 + table_size(bytes[10]);
    while (offset < bytes.size()) {
        const unsigned char marker = bytes[offset++];
        if (marker == 0x21 && offset < bytes.size()) {
            ++offset; // Extension label.
        } else if (marker == 0x2c && bytes.size() - offset >= 9) {
            offset += 9 + table_size(bytes[offset + 8]) + 1; // Descriptor, table, LZW code size.
        } else {
            return 0;
        }
        while (offset < bytes.size()) {
            const unsigned char length = bytes[offset++];
            if (!length) {
                if (marker == 0x2c)
                    return offset;
                break;
            }
            if (length > bytes.size() - offset)
                return 0;
            offset += length;
        }
    }
    return 0;
}

void copy_image_pixels(cairo_surface_t* surface, GdkPixbuf* pixels)
{
    const int width = gdk_pixbuf_get_width(pixels), height = gdk_pixbuf_get_height(pixels);
    const int channels = gdk_pixbuf_get_n_channels(pixels);
    const int stride = gdk_pixbuf_get_rowstride(pixels);
    const auto* data = gdk_pixbuf_read_pixels(pixels);
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
}
} // namespace

Application::ImagePreview Application::decode_image(const std::string& bytes)
{
    ImagePreview preview;
    const GObjectHandle<GdkPixbufLoader> loader{gdk_pixbuf_loader_new()};
    g_signal_connect(loader.get(), "size-prepared",
                     G_CALLBACK(+[](GdkPixbufLoader* decoder, int width, int height, gpointer) {
                         const double scale = std::min(1.0, 2048.0 / std::max(width, height));
                         gdk_pixbuf_loader_set_size(decoder, std::max(1, int(width * scale)),
                                                    std::max(1, int(height * scale)));
                     }),
                     nullptr);
    GError* failure = nullptr;
    const auto frame = first_gif_frame(bytes);
    const guchar trailer = 0x3b;
    const bool written =
        gdk_pixbuf_loader_write(loader.get(), reinterpret_cast<const guchar*>(bytes.data()),
                                frame ? frame : bytes.size(), &failure) &&
        (!frame || gdk_pixbuf_loader_write(loader.get(), &trailer, 1, &failure));
    const bool closed = gdk_pixbuf_loader_close(loader.get(), written ? &failure : nullptr);
    const GErrorHandle error{failure};
    auto* decoded = written && closed ? gdk_pixbuf_loader_get_pixbuf(loader.get()) : nullptr;
    if (!decoded) {
        preview.error = "Cannot display image: " +
                        std::string(error ? error->message : "unsupported image data");
    } else {
        const GObjectHandle<GdkPixbuf> pixels{gdk_pixbuf_apply_embedded_orientation(decoded)};
        const int width = gdk_pixbuf_get_width(pixels.get());
        const int height = gdk_pixbuf_get_height(pixels.get());
        preview.pixels.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(preview.pixels.get()) == CAIRO_STATUS_SUCCESS) {
            copy_image_pixels(preview.pixels.get(), pixels.get());
        } else {
            preview.pixels.reset();
            preview.error = "Cannot allocate image preview";
        }
    }
    return preview;
}

void Application::load_image_panel(SourcePanel& panel, ImagePreview preview, int title_width)
{
    panel.image_error = std::move(preview.error);
    if (preview.pixels) {
        const int width = cairo_image_surface_get_width(preview.pixels.get());
        const int height = cairo_image_surface_get_height(preview.pixels.get());
        const double scale = std::min(1.0, 640.0 / std::max(width, height));
        panel.image_size = {width * scale, height * scale};
        renderer->upload_image(panel.mesh, preview.pixels.get(), {{0, 0}, panel.image_size});
    }
    update_image_geometry(panel, title_width);
}

void Application::update_image_geometry(SourcePanel& panel, int title_width)
{
    if (panel.image_error.empty()) {
        panel.bounds.size = {std::max(double(title_width), panel.image_size.x),
                             header_height + panel.image_size.y};
    } else {
        const auto label = make_line(panel.image_error, {});
        panel.bounds.size = {std::max(480.0, double(title_width)),
                             double(header_height + line_height)};
        renderer->upload(panel.mesh, {{label.get(), {0, 0}}});
    }
}
