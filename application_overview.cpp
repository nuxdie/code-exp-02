#include "application.h"

// What the canvas says when the code is too small to read. Every label sits on the thing it
// names; one that would cover another is left out rather than moved somewhere untrue.

namespace {
constexpr double readable_zoom = 0.35;
constexpr double region_margin = 150;
constexpr double region_scale = 1.5;
constexpr double outline_scale = 0.85;
} // namespace

DocumentView& Application::overview_label(const std::string& text)
{
    if (overview_labels.size() > 4000) {
        for (auto& [name, label] : overview_labels)
            renderer->release(label.mesh);
        overview_labels.clear();
    }
    auto& label = overview_labels[text];
    update_document(label, text, 900, false);
    return label;
}

// A folder is a region: a faint ground under its files.
void Application::draw_regions(const Viewport& view)
{
    std::vector<std::pair<std::string, CanvasRect>> files;
    files.reserve(panels.size());
    for (const auto& panel : panels)
        files.emplace_back(panel.path, panel.bounds);
    regions = folder_regions(files);
    if (regions.size() < 2)
        return;
    for (const auto& region : regions) {
        const auto origin = camera.world_to_screen(
            {region.bounds.origin.x - region_margin, region.bounds.origin.y - region_margin}, view);
        renderer->rectangle({origin,
                             {(region.bounds.size.x + 2 * region_margin) * camera.zoom,
                              (region.bounds.size.y + 2 * region_margin) * camera.zoom}},
                            {0.955f, 0.96f, 0.97f, 1});
    }
}

void Application::draw_region_names(const Viewport& view, std::vector<CanvasRect>& taken)
{
    if (regions.size() < 2)
        return;
    const auto root = std::filesystem::path(workspace_root).filename().string();
    for (const auto& region : regions) {
        auto& label = overview_label(region.folder.empty() ? root : region.folder);
        const Vec2 size{label.size.x * region_scale, label.size.y * region_scale};
        const auto corner = camera.world_to_screen(
            {region.bounds.origin.x - region_margin, region.bounds.origin.y - region_margin}, view);
        const CanvasRect box{{corner.x, corner.y - size.y - 4}, size};
        if (box.origin.y > height || box.origin.y + size.y < 0 || box.origin.x > width ||
            box.origin.x + size.x < 0 || !claim_label(taken, box))
            continue;
        renderer->draw(label.mesh, box.origin, region_scale);
    }
}

// Names of what a file defines, over code that is too small to read. A name appears once
// its definition is tall enough on screen to sit behind it.
void Application::draw_outline(SourcePanel& panel, const Viewport& view,
                               std::vector<CanvasRect>& taken)
{
    const int key = panel.version * 2 + (panel.symbols.empty() ? 0 : 1);
    if (std::exchange(panel.outline_key, key) != key)
        panel.outline = panel.symbols.empty() ? text_outline(panel.path, panel.text)
                                              : symbol_outline(panel.symbols);
    const auto origin = camera.world_to_screen(panel.bounds.origin, view);
    const double room = panel.bounds.size.x * camera.zoom * 1.3 + 24;
    const int rows = static_cast<int>(panel.folded.display_lines.size());
    for (const auto& entry : panel.outline) {
        if (entry.line >= rows || entry.last >= rows)
            continue;
        const int row = panel.folded.display_lines[entry.line];
        const double tall =
            (panel.folded.display_lines[entry.last] - row + 1) * line_height * camera.zoom;
        // Entries are longest first: once one is too short, so are the rest.
        if (tall < 14)
            break;
        const double y = origin.y + (header_height + row * line_height) * camera.zoom;
        if (panel.folded.source_lines[row] != entry.line || y > height || y < -20)
            continue;
        auto& label = overview_label(entry.name);
        const CanvasRect box{{origin.x + 18 * camera.zoom, y},
                             {label.size.x * outline_scale + 8, label.size.y * outline_scale + 2}};
        if (box.size.x > room || !claim_label(taken, box))
            continue;
        renderer->rectangle(box, {1, 1, 1, 0.9f});
        renderer->draw(label.mesh, {box.origin.x + 4, box.origin.y + 1}, outline_scale);
    }
}

void Application::draw_headers(const Viewport& view)
{
    header_hits.assign(panels.size(), {});
    window_headers.clear();
    std::vector<CanvasRect> taken;
    draw_region_names(view, taken);
    // The file under the pointer always has its title; after it, the longest files first.
    std::vector<std::size_t> order(panels.size());
    std::iota(order.begin(), order.end(), 0);
    const int hovered = panel_at(mouse_position);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        const bool first = static_cast<int>(a) == hovered, second = static_cast<int>(b) == hovered;
        return first != second ? first : panels[a].bounds.size.y > panels[b].bounds.size.y;
    });
    const double scale = CanvasWindowManager::title_scale(camera.zoom);
    for (const auto i : order) {
        const auto& panel = panels[i];
        const auto point = camera.world_to_screen(panel.bounds.origin, view);
        int title_width = 0;
        pango_layout_get_pixel_size(panel.title, &title_width, nullptr);
        if (point.y < -27 * scale || point.y > height || point.x > width ||
            point.x + (title_width + 34) * scale < 0)
            continue;
        const auto bounds = CanvasWindowManager::title_bounds(
            point, {static_cast<double>(title_width + 34), 27}, camera.zoom, width, {});
        if (!claim_label(taken, bounds) && static_cast<int>(i) != hovered)
            continue;
        header_hits[i] = bounds;
        window_headers.push_back(bounds);
        renderer->rectangle(bounds, {1, 1, 1, 0.94f});
        renderer->draw(panel.header_mesh, bounds.origin, scale);
    }
    if (camera.zoom >= readable_zoom)
        return;
    const CanvasRect visible{camera.screen_to_world({view.x, view.y}, view),
                             {view.width / camera.zoom, view.height / camera.zoom}};
    for (const auto i : order)
        if (!panels[i].image && panels[i].bounds.intersects(visible))
            draw_outline(panels[i], view, taken);
}
