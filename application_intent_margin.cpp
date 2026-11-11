#include "application.h"

#include <cstdio>

namespace {
constexpr double note_width = 380;
constexpr double readable_zoom = 0.35;

std::string shell_quote(const std::string& text)
{
    std::string quoted = "'";
    for (const char character : text)
        quoted += character == '\'' ? std::string("'\\''") : std::string(1, character);
    return quoted + "'";
}

std::string read_intent_view(const std::string& command)
{
    std::string output;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        std::array<char, 65536> buffer{};
        while (const auto count = fread(buffer.data(), 1, buffer.size(), pipe))
            output.append(buffer.data(), count);
        if (pclose(pipe) != 0)
            output.clear();
    }
    return output;
}
} // namespace

void Application::load_intent_margin()
{
    if (benchmark || std::getenv("SOURCE_CANVAS_DISABLE_INTENT"))
        return;
    if (intent_margin.loading.valid()) {
        intent_margin.stale = true;
        return;
    }
    const auto command = shell_quote(python_executable) + " " +
                         shell_quote((executable.parent_path() / "intents.py").string()) +
                         " view --refresh --root " + shell_quote(workspace_root) + " 2>/dev/null";
    intent_margin.loading = std::async(std::launch::async, read_intent_view, command);
}

void Application::poll_intent_margin()
{
    auto& loading = intent_margin.loading;
    if (!loading.valid() || loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const auto view = nlohmann::json::parse(loading.get(), nullptr, false);
    intent_margin.view = parse_intent_view(view);
    intent_margin.pinned.reset();
    redraw = true;
    if (std::exchange(intent_margin.stale, false))
        load_intent_margin();
}

std::optional<SourceHit> Application::source_line_at(Vec2 screen)
{
    if (window_at(screen) != WindowKind::Source && window_at(screen) != WindowKind::Canvas)
        return std::nullopt;
    const auto world = camera.screen_to_world(screen, viewport());
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto& panel = panels[i];
        const double x = world.x - panel.bounds.origin.x;
        const double y = world.y - panel.bounds.origin.y - header_height;
        if (panel.image || x < 0 || x >= panel.bounds.size.x || y < 0)
            continue;
        const auto row = static_cast<std::size_t>(y / line_height);
        if (row < panel.folded.source_lines.size())
            return SourceHit{static_cast<int>(i), panel.folded.source_lines[row], 0};
    }
    return std::nullopt;
}

CanvasRect Application::intent_run_bounds(const SourcePanel& panel, const IntentRun& run)
{
    const int count = static_cast<int>(panel.folded.display_lines.size());
    if (count == 0 || run.first >= count)
        return {};
    const int first = panel.folded.display_lines[std::max(0, run.first)];
    const int last = panel.folded.display_lines[std::min(run.last, count - 1)];
    const auto origin = camera.world_to_screen(
        {panel.bounds.origin.x, panel.bounds.origin.y + header_height + first * line_height},
        viewport());
    return {origin,
            {panel.bounds.size.x * camera.zoom, (last - first + 1) * line_height * camera.zoom}};
}

void Application::draw_intent_run(const SourcePanel& panel, const IntentRun& run)
{
    const auto bounds = intent_run_bounds(panel, run);
    if (bounds.size.y <= 0 || bounds.origin.y > height || bounds.origin.y + bounds.size.y < 0)
        return;
    const auto& selected = intent_margin.selected;
    const auto& intent = intent_margin.view.intent_of(run);
    const bool lit = !selected.empty() && intent == selected;
    if (lit) {
        // Zoomed out, the lit lines are the picture; zoomed in, the code must stay legible.
        const auto tint = intent_color(intent, camera.zoom < readable_zoom ? 0.55 : 0.9);
        renderer->rectangle(bounds, {tint[0], tint[1], tint[2], 1});
    }
    // Below a pixel a bar is noise, unless it is the intent being looked for.
    if (bounds.size.y < 1.5 && !lit)
        return;
    // Lines whose turn has not been distilled into an intent yet still show who asked.
    const auto color =
        intent.empty() ? std::array<float, 3>{0.55f, 0.57f, 0.6f} : intent_color(intent, 0.5);
    const float strength = selected.empty() || lit ? 1.0f : 0.25f;
    const double inset = std::min(1.0, bounds.size.y / 4);
    renderer->rectangle(
        {{bounds.origin.x + 8 * camera.zoom, bounds.origin.y + inset},
         {std::max(2.0, 4 * camera.zoom), std::max(1.0, bounds.size.y - 2 * inset)}},
        {color[0], color[1], color[2], strength});
}

void Application::draw_intent_margin(const Viewport& view)
{
    const auto& files = intent_margin.view.files;
    const CanvasRect visible{camera.screen_to_world({view.x, view.y}, view),
                             {view.width / camera.zoom, view.height / camera.zoom}};
    for (const auto& panel : panels) {
        const auto runs = files.find(panel.path);
        if (runs == files.end() || !panel.bounds.intersects(visible))
            continue;
        for (const auto& run : runs->second)
            draw_intent_run(panel, run);
    }
}

void Application::draw_intent_note()
{
    auto& margin = intent_margin;
    margin.note_bounds = {};
    const SourcePanel* panel = nullptr;
    std::optional<IntentRun> run = margin.pinned;
    if (run) {
        const auto found = std::find_if(panels.begin(), panels.end(), [&](const auto& source) {
            return source.path == margin.pinned_path;
        });
        panel = found == panels.end() ? nullptr : &*found;
    } else if (!drag_button && popup_text.empty() && camera.zoom >= readable_zoom) {
        if (const auto hit = source_line_at(mouse_position)) {
            panel = &panels[hit->panel];
            if (const auto* found = margin.view.run_at(panel->path, hit->line))
                run = *found;
        }
    }
    if (!panel || !run)
        return;
    const auto text = intent_note(margin.view, *run, margin.pinned.has_value());
    if (text.empty())
        return;
    update_document(margin.note, text, static_cast<int>(note_width));
    const auto code = intent_run_bounds(*panel, *run);
    const Vec2 size{note_width + 28, margin.note.size.y + 20};
    // A sidenote: left of the card, else right of it, else the window's right edge.
    double x = code.origin.x - size.x - 12;
    if (x < 8)
        x = code.origin.x + code.size.x + 12;
    if (x + size.x > width - 8)
        x = width - size.x - 8;
    const double y = std::clamp(code.origin.y, 8.0, std::max(8.0, height - size.y - 8));
    margin.note_bounds = {{x, y}, size};
    const auto color = intent_color(margin.view.intent_of(*run), 0.5);
    renderer->rectangle(margin.note_bounds, {1, 1, 1, 0.97f});
    renderer->rectangle({{x, y}, {3, size.y}}, {color[0], color[1], color[2], 1});
    renderer->draw(margin.note.mesh, {x + 16, y + 10}, 1);
}

// Select the intent behind a source line, or clear the selection when asked again.
void Application::select_intent_at(Vec2 pointer)
{
    auto& margin = intent_margin;
    const auto hit = source_line_at(pointer);
    const auto* run = hit ? margin.view.run_at(panels[hit->panel].path, hit->line) : nullptr;
    if (!run) {
        clear_intent_selection();
        return;
    }
    const auto& intent = margin.view.intent_of(*run);
    const bool same = margin.pinned && margin.pinned->turn == run->turn &&
                      margin.pinned->first == run->first &&
                      margin.pinned_path == panels[hit->panel].path;
    if (same) {
        clear_intent_selection();
        return;
    }
    margin.selected = intent;
    margin.pinned = *run;
    margin.pinned_path = panels[hit->panel].path;
    redraw = true;
}

bool Application::clear_intent_selection()
{
    auto& margin = intent_margin;
    if (margin.selected.empty() && !margin.pinned)
        return false;
    margin.selected.clear();
    margin.pinned.reset();
    redraw = true;
    return true;
}

bool Application::intent_margin_button(const XEvent& event, Vec2 pointer)
{
    const auto button = event.xbutton.button;
    if (drag_button || (button != Button1 && button != Button2))
        return false;
    const bool on_note = CanvasWindowManager::contains(intent_margin.note_bounds, pointer);
    if (button == Button2)
        return on_note && clear_intent_selection();
    if (on_note)
        return intent_margin.pinned.has_value();
    // The gutter bar is the handle for the intent behind those lines.
    const auto hit = source_line_at(pointer);
    if (!hit || window_at(pointer) != WindowKind::Source)
        return false;
    const auto world = camera.screen_to_world(pointer, viewport());
    if (world.x - panels[hit->panel].bounds.origin.x >= 20)
        return false;
    select_intent_at(pointer);
    return true;
}

bool Application::intent_margin_key(const XEvent& event)
{
    auto key = event.xkey;
    return XLookupKeysym(&key, 0) == XK_Escape && popup_text.empty() && !drag_button &&
           clear_intent_selection();
}
