#include "application.h"

void Application::prepare_walkthrough_step(WalkthroughStep& step, std::size_t index)
{
    constexpr int text_width = 600;
    update_document(step.heading, std::to_string(index + 1) + ". " + step.title, text_width, false);
    update_document(step.explanation, step.note, text_width, false);
    const auto found = std::find_if(panels.begin(), panels.end(),
                                    [&](const auto& panel) { return panel.path == step.path; });
    step.available = found != panels.end() && step.last <= static_cast<int>(found->text.size());
    auto location = step.path + ":" + std::to_string(step.first) + "–" + std::to_string(step.last);
    if (!step.available)
        location += " · source range unavailable";
    update_document(step.location, location, text_width, false);
    step.code_y = step.heading.size.y + step.explanation.size.y + step.location.size.y + 44;
    step.bounds.size = {text_width + 32.0,
                        step.code_y + (step.last - step.first + 1) * line_height + 16};
    const auto source = step.available ? found->source : "";
    if (step.cached_source == source && step.code.buffer)
        return;
    step.cached_source = source;
    std::vector<PlacedText> text;
    std::vector<std::unique_ptr<PangoLayout, PangoLayoutDeleter>> numbers;
    if (step.available)
        for (int line = step.first; line <= step.last; ++line) {
            numbers.emplace_back(make_line(std::to_string(line), {}));
            const double y = (line - step.first) * line_height;
            text.push_back({numbers.back().get(), {0, y}});
            text.push_back({found->lines[line - 1], {64, y}});
            int width = 0;
            pango_layout_get_pixel_size(found->lines[line - 1], &width, nullptr);
            step.bounds.size.x = std::max(step.bounds.size.x, width + 96.0);
        }
    renderer->upload(step.code, text);
}

void Application::prepare_walkthrough(bool arrange)
{
    if (!walkthrough)
        return;
    auto& board = *walkthrough;
    update_document(board.heading, "Walkthrough · " + popup_literal(board.title), 1000, false);
    // Preserve user placement when live source changes; only dimensions and excerpts refresh.
    for (std::size_t i = 0; i < board.steps.size(); ++i) {
        auto& step = board.steps[i];
        const auto previous_width = step.bounds.size.x;
        prepare_walkthrough_step(step, i);
        step.bounds.size.x = std::max(previous_width, step.bounds.size.x);
    }
    if (arrange) {
        const auto layout = board.specification.value("layout", "grid");
        const int columns = layout == "horizontal" ? static_cast<int>(board.steps.size())
                            : layout == "vertical"
                                ? 1
                                : std::min(2, static_cast<int>(board.steps.size()));
        double cell_width = 0, cell_height = 0;
        for (const auto& step : board.steps) {
            cell_width = std::max(cell_width, step.bounds.size.x + 160);
            cell_height = std::max(cell_height, step.bounds.size.y + 100);
        }
        for (std::size_t i = 0; i < board.steps.size(); ++i) {
            const auto& saved = board.specification["steps"][i];
            auto& origin = board.steps[i].bounds.origin;
            if (!saved.contains("x"))
                origin = {static_cast<double>(i % columns) * cell_width,
                          static_cast<double>(i / columns) * cell_height};
            origin.x += board.origin.x;
            origin.y += board.origin.y + board.heading.size.y + 32;
        }
    }
    auto end = Vec2{board.origin.x + board.heading.size.x, board.origin.y + board.heading.size.y};
    auto start = board.origin;
    for (const auto& step : board.steps) {
        start.x = std::min(start.x, step.bounds.origin.x);
        start.y = std::min(start.y, step.bounds.origin.y);
        end.x = std::max(end.x, step.bounds.origin.x + step.bounds.size.x);
        end.y = std::max(end.y, step.bounds.origin.y + step.bounds.size.y);
    }
    board.bounds = {start, {end.x - start.x, end.y - start.y}};
    content_size.x = std::max(content_size.x, end.x + 32);
    content_size.y = std::max(content_size.y, end.y + 32);
}

void Application::draw_walkthrough()
{
    if (!walkthrough)
        return;
    prepare_walkthrough(false);
    auto& board = *walkthrough;
    auto point = [&](Vec2 world) { return camera.world_to_screen(world, viewport()); };
    auto rectangle = [&](CanvasRect world, std::array<float, 4> color) {
        renderer->rectangle(
            {point(world.origin), {world.size.x * camera.zoom, world.size.y * camera.zoom}}, color);
    };
    std::vector<GpuPath> arrows;
    std::vector<Vec2> arrow_points;
    for (auto& connection : board.connections) {
        const auto& a = board.steps[connection.from].bounds;
        const auto& b = board.steps[connection.to].bounds;
        Vec2 from{a.origin.x + a.size.x / 2, a.origin.y + a.size.y / 2};
        Vec2 to{b.origin.x + b.size.x / 2, b.origin.y + b.size.y / 2};
        if (std::abs(to.x - from.x) > std::abs(to.y - from.y)) {
            from.x += (to.x > from.x ? 1 : -1) * a.size.x / 2;
            to.x += (to.x > from.x ? -1 : 1) * b.size.x / 2;
        } else {
            from.y += (to.y > from.y ? 1 : -1) * a.size.y / 2;
            to.y += (to.y > from.y ? -1 : 1) * b.size.y / 2;
        }
        arrows.push_back({{from, to}, {0.2f, 0.42f, 0.72f, 0.9f}, 2, true});
        arrow_points.push_back(from);
        arrow_points.push_back(to);
        update_document(connection.caption, popup_literal(connection.label), 136, false);
        connection.caption_origin = {(from.x + to.x - connection.caption.size.x) / 2,
                                     (from.y + to.y - connection.caption.size.y) / 2};
    }
    rectangle({{board.bounds.origin.x - 20, board.bounds.origin.y - 16},
               {board.bounds.size.x + 40, board.bounds.size.y + 32}},
              {0.94f, 0.96f, 0.99f, 1});
    if (board.arrow_points.size() != arrow_points.size() ||
        !std::equal(arrow_points.begin(), arrow_points.end(), board.arrow_points.begin(),
                    [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; })) {
        renderer->paths(board.arrows, arrows);
        board.arrow_points = std::move(arrow_points);
    }
    renderer->draw(board.arrows, point({0, 0}), camera.zoom);
    auto& occupied = window_headers;
    draw_window_title(board.title_view, board.title, {board.origin, board.bounds.size},
                      board.header_bounds, occupied);
    for (auto& step : board.steps) {
        rectangle(step.bounds, {1, 1, 1, 1});
        auto origin = step.bounds.origin;
        origin.x += 16;
        origin.y += 12;
        draw_window_title(step.title_view, step.heading.source, step.bounds, step.header_bounds,
                          occupied);
        origin.y += step.heading.size.y + 8;
        renderer->draw(step.explanation.mesh, point(origin), camera.zoom);
        origin.y += step.explanation.size.y + 8;
        renderer->draw(step.location.mesh, point(origin), camera.zoom);
        draw_walkthrough_selection(step);
        renderer->draw(step.code,
                       point({step.bounds.origin.x + 16, step.bounds.origin.y + step.code_y}),
                       camera.zoom);
    }
    for (const auto& connection : board.connections) {
        const auto& caption = connection.caption;
        const auto origin = connection.caption_origin;
        rectangle({{origin.x - 4, origin.y - 4}, {caption.size.x + 8, caption.size.y + 8}},
                  {0.94f, 0.96f, 0.99f, 1});
        renderer->draw(caption.mesh, point(origin), camera.zoom);
    }
}

void Application::draw_walkthrough_selection(const WalkthroughStep& step)
{
    if (text_selection.surface == TextSurface::Source && text_selection.key == step.path) {
        const auto found = std::find_if(panels.begin(), panels.end(),
                                        [&](const auto& panel) { return panel.path == step.path; });
        if (step.available && found != panels.end() && text_selection.range.text == found->source) {
            const auto [begin, end] = text_selection.range.selection();
            std::size_t offset = 0;
            for (int line = 0; line < step.last; ++line) {
                const auto length = found->text[line].size();
                if (line >= step.first - 1 && begin < offset + length + 1 && end > offset)
                    draw_selection_layout(
                        found->lines[line],
                        camera.world_to_screen(
                            {step.bounds.origin.x + 80, step.bounds.origin.y + step.code_y +
                                                            (line - step.first + 1) * line_height},
                            viewport()),
                        camera.zoom,
                        {begin > offset ? begin - offset : 0, std::min(length, end - offset)},
                        {0.70f, 0.81f, 0.98f, 0.85f});
                offset += length + 1;
            }
        }
    }
}
