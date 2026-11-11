#include "connection_hud.h"

#include <algorithm>
#include <cmath>

bool inside(const CanvasRect& view, Vec2 point)
{
    return point.x >= view.origin.x && point.y >= view.origin.y &&
           point.x <= view.origin.x + view.size.x && point.y <= view.origin.y + view.size.y;
}

Vec2 view_exit(const CanvasRect& view, Vec2 from, Vec2 to)
{
    from = {std::clamp(from.x, view.origin.x, view.origin.x + view.size.x),
            std::clamp(from.y, view.origin.y, view.origin.y + view.size.y)};
    double reach = 1;
    const auto limit = [&](double start, double delta, double low, double high) {
        if (delta > 0)
            reach = std::min(reach, (high - start) / delta);
        else if (delta < 0)
            reach = std::min(reach, (low - start) / delta);
    };
    limit(from.x, to.x - from.x, view.origin.x, view.origin.x + view.size.x);
    limit(from.y, to.y - from.y, view.origin.y, view.origin.y + view.size.y);
    reach = std::max(0.0, reach);
    return {from.x + (to.x - from.x) * reach, from.y + (to.y - from.y) * reach};
}

const char* direction_glyph(const CanvasRect& view, Vec2 target)
{
    const double left = view.origin.x - target.x;
    const double right = target.x - view.origin.x - view.size.x;
    const double up = view.origin.y - target.y;
    const double down = target.y - view.origin.y - view.size.y;
    const double horizontal = std::max(left, right), vertical = std::max(up, down);
    if (horizontal <= 0 && vertical <= 0)
        return "";
    if (horizontal >= vertical)
        return left > right ? "←" : "→";
    return up > down ? "↑" : "↓";
}

CanvasRect marker_box(const CanvasRect& view, Vec2 anchor, Vec2 size)
{
    const double x = std::clamp(anchor.x - size.x / 2, view.origin.x,
                                std::max(view.origin.x, view.origin.x + view.size.x - size.x));
    const double y = std::clamp(anchor.y - size.y / 2, view.origin.y,
                                std::max(view.origin.y, view.origin.y + view.size.y - size.y));
    return {{x, y}, size};
}

void separate_markers(const CanvasRect& view, std::vector<ConnectionMarker>& markers)
{
    std::sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) {
        return a.bounds.origin.y != b.bounds.origin.y ? a.bounds.origin.y < b.bounds.origin.y
                                                      : a.bounds.origin.x < b.bounds.origin.x;
    });
    const double bottom = view.origin.y + view.size.y;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        auto& box = markers[i].bounds;
        const auto blocker = [&]() -> const CanvasRect* {
            for (std::size_t j = 0; j < i; ++j)
                if (box.intersects(markers[j].bounds))
                    return &markers[j].bounds;
            return nullptr;
        };
        for (std::size_t pass = 0; pass < i; ++pass)
            if (const auto* other = blocker())
                box.origin.y = other->origin.y + other->size.y + 2;
        if (box.origin.y + box.size.y <= bottom)
            continue;
        // A column that ran out of room continues upward from the bottom edge.
        box.origin.y = bottom - box.size.y;
        for (std::size_t pass = 0; pass < i; ++pass)
            if (const auto* other = blocker())
                box.origin.y = other->origin.y - box.size.y - 2;
    }
}

std::string marker_text(const std::vector<ConnectionLink>& links,
                        const std::vector<std::size_t>& members, const std::string& direction)
{
    auto order = members;
    std::stable_sort(order.begin(), order.end(),
                     [&](auto a, auto b) { return links[a].count > links[b].count; });
    std::string text = direction + " " + links[order.front()].file;
    std::size_t named = 0, total = 0;
    for (const auto index : order) {
        const auto& link = links[index];
        total += link.name.empty() ? 0 : 1;
        if (link.name.empty() || named == 3)
            continue;
        text += (named++ ? ", " : "  ") + link.name;
        if (link.count > 1)
            text += " ×" + std::to_string(link.count);
    }
    if (total > named)
        text += " +" + std::to_string(total - named);
    if (total == 0 && links[order.front()].count > 1)
        text += " ×" + std::to_string(links[order.front()].count);
    return text;
}

Vec2 center(const CanvasRect& box)
{
    return {box.origin.x + box.size.x / 2, box.origin.y + box.size.y / 2};
}

Vec2 border_point(const CanvasRect& box, Vec2 toward)
{
    const auto middle = center(box);
    const double dx = toward.x - middle.x, dy = toward.y - middle.y;
    if (dx == 0 && dy == 0)
        return middle;
    // Scale the direction until it reaches the nearer pair of sides.
    const double horizontal = dx == 0 ? 1e9 : box.size.x / 2 / std::abs(dx);
    const double vertical = dy == 0 ? 1e9 : box.size.y / 2 / std::abs(dy);
    const double reach = std::min({horizontal, vertical, 1.0});
    return {middle.x + dx * reach, middle.y + dy * reach};
}
