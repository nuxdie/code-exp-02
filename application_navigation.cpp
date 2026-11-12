#include "application.h"
#include <X11/XF86keysym.h>

namespace {
constexpr std::size_t history_limit = 100;

bool same_view(const Camera& a, const Camera& b)
{
    return std::abs(a.center.x - b.center.x) < 1e-8 && std::abs(a.center.y - b.center.y) < 1e-8 &&
           std::abs(a.zoom - b.zoom) < 1e-8;
}
} // namespace

Application::SavedView Application::current_view()
{
    int index = panel_at(viewport().center());
    if (index < 0)
        index = panel_at(mouse_position);
    SavedView saved{camera, "", camera.center};
    if (index >= 0) {
        saved.path = panels[index].path;
        saved.relative_center.x -= panels[index].bounds.origin.x;
        saved.relative_center.y -= panels[index].bounds.origin.y;
    }
    return saved;
}

Camera Application::saved_camera(const SavedView& saved)
{
    auto result = saved.camera;
    const auto panel = std::ranges::find(panels, saved.path, &SourcePanel::path);
    if (panel != panels.end())
        result.center = {panel->bounds.origin.x + saved.relative_center.x,
                         panel->bounds.origin.y + saved.relative_center.y};
    return result;
}

void Application::remember_view(bool continuous)
{
    if (initial_view)
        return;
    const auto now = Clock::now();
    const bool continuing =
        continuous && navigation_continuous &&
        (drag_button || now - navigation_input < std::chrono::milliseconds(350));
    if (!pending_view || !continuing) {
        finish_view();
        pending_view = current_view();
    }
    navigation_input = now;
    navigation_continuous = continuous;
}

void Application::finish_view()
{
    if (!pending_view)
        return;
    if (!same_view(saved_camera(*pending_view), camera)) {
        if (navigation_history.size() >= history_limit)
            navigation_history.erase(navigation_history.begin());
        navigation_history.push_back(*pending_view);
        navigation_forward.clear();
    }
    pending_view.reset();
}

bool Application::navigate_view(bool forward)
{
    finish_view();
    zoom_animation.cancel(camera);
    auto& source = forward ? navigation_forward : navigation_history;
    auto& destination = forward ? navigation_history : navigation_forward;
    if (source.empty())
        return false;
    if (destination.size() >= history_limit)
        destination.erase(destination.begin());
    destination.push_back(current_view());
    camera = saved_camera(source.back());
    source.pop_back();
    zoom_animation.cancel(camera);
    focused_line.reset();
    initial_view = false;
    hide_popup();
    schedule_save();
    redraw = true;
    return true;
}

bool Application::go_back()
{
    return navigate_view(false);
}

bool Application::go_forward()
{
    return navigate_view(true);
}

bool Application::navigation_key(const XEvent& event)
{
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    const bool alt = (event.xkey.state & Mod1Mask) && !(event.xkey.state & ControlMask);
    const bool back = key == XF86XK_Back || (alt && key == XK_Left);
    const bool forward = key == XF86XK_Forward || (alt && key == XK_Right);
    if (!back && !forward)
        return false;
    if (!drag_button)
        navigate_view(forward);
    return true;
}

lsp::Json Application::save_navigation()
{
    auto encode = [](const std::vector<SavedView>& views) {
        auto result = lsp::Json::array();
        for (const auto& saved : views)
            result.push_back({{"x", saved.camera.center.x},
                              {"y", saved.camera.center.y},
                              {"zoom", saved.camera.zoom},
                              {"path", saved.path},
                              {"relativeX", saved.relative_center.x},
                              {"relativeY", saved.relative_center.y}});
        return result;
    };
    // Include an unfinished gesture without splitting it in the running app.
    auto back = navigation_history;
    auto forward = navigation_forward;
    if (pending_view && !same_view(saved_camera(*pending_view), camera)) {
        if (back.size() >= history_limit)
            back.erase(back.begin());
        back.push_back(*pending_view);
        forward.clear();
    }
    return {{"back", encode(back)}, {"forward", encode(forward)}};
}

void Application::load_navigation(const lsp::Json& state)
{
    if (!state.is_object())
        return;
    auto decode = [](const lsp::Json& entries) {
        std::vector<SavedView> result;
        if (!entries.is_array())
            return result;
        for (std::size_t i = entries.size() > history_limit ? entries.size() - history_limit : 0;
             i < entries.size(); ++i) {
            try {
                const auto& entry = entries[i];
                SavedView saved{{{entry.at("x"), entry.at("y")}, entry.at("zoom")},
                                entry.value("path", ""),
                                {entry.at("relativeX"), entry.at("relativeY")}};
                if (std::isfinite(saved.camera.center.x) && std::isfinite(saved.camera.center.y) &&
                    std::isfinite(saved.relative_center.x) &&
                    std::isfinite(saved.relative_center.y) &&
                    saved.camera.zoom >= Camera::min_zoom && saved.camera.zoom <= Camera::max_zoom)
                    result.push_back(saved);
            } catch (const lsp::Json::exception&) {
                // Ignore a damaged history entry while keeping the rest of the session.
            }
        }
        return result;
    };
    navigation_history = decode(state.value("back", lsp::Json::array()));
    navigation_forward = decode(state.value("forward", lsp::Json::array()));
    pending_view.reset();
}

bool Application::navigation_button(unsigned button)
{
    if (button != 8 && button != 9)
        return false;
    if (!drag_button)
        navigate_view(button == 9);
    return true;
}

bool Application::camera_key(KeySym key)
{
    const auto view = viewport();
    if (key != XK_f && key != XK_F)
        return false;
    remember_view();
    camera.fit(content_size, view);
    zoom_animation.cancel(camera);
    return true;
}
