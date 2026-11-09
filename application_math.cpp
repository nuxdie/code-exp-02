#include "application.h"

Application::MathArtwork Application::load_math_artwork(const std::string& svg)
{
    GError* error = nullptr;
    auto* handle =
        rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(svg.data()), svg.size(), &error);
    if (!handle) {
        if (error)
            g_error_free(error);
        return {};
    }
    double width = 0, height = 0;
    const bool sized = rsvg_handle_get_intrinsic_size_in_pixels(handle, &width, &height);
    if (!sized || width <= 0 || height <= 0) {
        g_object_unref(handle);
        return {};
    }
    const double scale = std::min(2.0, 1400.0 / std::max(width, height));
    auto* image =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(width * scale)),
                                   static_cast<int>(std::ceil(height * scale)));
    auto* context = cairo_create(image);
    const RsvgRectangle bounds{0, 0, width * scale, height * scale};
    const bool drawn = rsvg_handle_render_document(handle, context, &bounds, nullptr);
    MathArtwork result;
    if (drawn)
        result = {renderer->image(image), {width, height}, true};
    cairo_destroy(context);
    cairo_surface_destroy(image);
    g_object_unref(handle);
    return result;
}

void Application::poll_math()
{
    if (!math_typesetter)
        return;
    for (const auto& result : math_typesetter->take_results()) {
        math_artwork[result.key] = load_math_artwork(result.svg);
        cached_popup.clear();
        ++document_generation;
        redraw = true;
    }
}

void Application::shape_document_math(PangoAttrList* attributes,
                                      const std::vector<PopupMath>& math_items, int layout_width)
{
    for (const auto& math : math_items) {
        const auto key = MathTypesetter::key(math.expression, math.display);
        const auto found = math_artwork.find(key);
        Vec2 size{std::min(550.0, math.expression.size() * 9.0), 24};
        if (found != math_artwork.end() && found->second.ready)
            size = found->second.size;
        else if (found == math_artwork.end() && math_typesetter)
            math_typesetter->request(math.expression, math.display);
        const double scale = std::min(1.0, std::max(1, layout_width) / std::max(1.0, size.x));
        size.x *= scale;
        size.y *= scale;
        PangoRectangle rectangle{0, static_cast<int>(-size.y * 0.8 * PANGO_SCALE),
                                 static_cast<int>(size.x * PANGO_SCALE),
                                 static_cast<int>(size.y * PANGO_SCALE)};
        auto* shape = pango_attr_shape_new(&rectangle, &rectangle);
        shape->start_index = static_cast<guint>(math.begin);
        shape->end_index = static_cast<guint>(math.begin + 3);
        pango_attr_list_insert(attributes, shape);
    }
}

void Application::upload_document_mesh(GpuMesh& mesh, PangoLayout* layout,
                                       const std::vector<PopupMath>& math_items)
{
    std::vector<PlacedText> text{{layout, {0, 0}}};
    std::vector<PlacedImage> images;
    std::vector<PangoLayout*> fallback;
    for (const auto& math : math_items) {
        PangoRectangle rectangle{};
        pango_layout_index_to_pos(layout, static_cast<int>(math.begin), &rectangle);
        const CanvasRect bounds{{static_cast<double>(rectangle.x) / PANGO_SCALE,
                                 static_cast<double>(rectangle.y) / PANGO_SCALE},
                                {static_cast<double>(rectangle.width) / PANGO_SCALE,
                                 static_cast<double>(rectangle.height) / PANGO_SCALE}};
        const auto found = math_artwork.find(MathTypesetter::key(math.expression, math.display));
        if (found != math_artwork.end() && found->second.ready) {
            const auto size = found->second.size;
            const double scale = std::min(1.0, bounds.size.x / std::max(1.0, size.x));
            images.push_back(
                {found->second.image,
                 {{bounds.origin.x, bounds.origin.y + (bounds.size.y - size.y * scale) / 2},
                  {size.x * scale, size.y * scale}}});
        } else {
            auto* fallback_layout = make_line(math.expression, {});
            pango_layout_set_width(fallback_layout, static_cast<int>(bounds.size.x * PANGO_SCALE));
            pango_layout_set_ellipsize(fallback_layout, PANGO_ELLIPSIZE_END);
            fallback.push_back(fallback_layout);
            text.push_back({fallback_layout, bounds.origin});
        }
    }
    renderer->upload(mesh, text, images);
    for (auto* layout : fallback)
        g_object_unref(layout);
}
