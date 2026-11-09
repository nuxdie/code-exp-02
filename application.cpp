#include "application.h"
#include <X11/Xatom.h>
#include <unistd.h>

namespace {
template <class Source> int wake_fd(const std::unique_ptr<Source>& source)
{
    return source ? source->event_fd() : -1;
}
} // namespace

bool Application::initialize()
{
    benchmark = std::getenv("SOURCE_CANVAS_BENCHMARK") != nullptr;
    display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Could not open the X display.\n";
        return false;
    }

    // Load the bundled font faces for this application only.
    std::error_code path_error;
    executable = std::filesystem::read_symlink("/proc/self/exe", path_error);
    if (path_error) {
        std::cerr << "Could not locate the executable: " << path_error.message() << '\n';
        return false;
    }
    for (const char* face : {"Regular", "Bold", "Italic"}) {
        const auto path =
            executable.parent_path() / "fonts" / (std::string("JetBrainsMono-") + face + ".ttf");
        if (!FcConfigAppFontAddFile(FcConfigGetCurrent(),
                                    reinterpret_cast<const FcChar8*>(path.c_str()))) {
            std::cerr << "Could not load bundled font: " << path << '\n';
            return false;
        }
    }

    for (const char* face : {"Regular", "Bold", "Italic", "BoldItalic"}) {
        const auto path = executable.parent_path() / "fonts/noto-serif" /
                          (std::string("NotoSerif-") + face + ".ttf");
        FcConfigAppFontAddFile(FcConfigGetCurrent(),
                               reinterpret_cast<const FcChar8*>(path.c_str()));
    }

    screen = DefaultScreen(display);

    try {
        renderer = std::make_unique<GpuRenderer>(display, screen, benchmark);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return false;
    }
    window = renderer->window();

    XStoreName(display, window, "Source Code - White Window");
    const unsigned long process = static_cast<unsigned long>(getpid());
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&process), 1);
    XSelectInput(display, window,
                 ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | LeaveWindowMask);
    normal_cursor = XCreateFontCursor(display, XC_left_ptr);
    text_cursor = XCreateFontCursor(display, XC_xterm);
    drag_cursor = XCreateFontCursor(display, XC_fleur);
    XDefineCursor(display, window, normal_cursor);
    active_cursor = normal_cursor;

    // Pango shapes at one stable size. OpenGL handles all camera transforms.
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surface);
    auto* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_set_font_options(cr, options);

    base_layout = pango_cairo_create_layout(cr);
    pango_cairo_context_set_font_options(pango_layout_get_context(base_layout), options);
    cairo_font_options_destroy(options);
    PangoFontDescription* font = pango_font_description_from_string("JetBrains Mono");
    pango_font_description_set_absolute_size(font, 16 * PANGO_SCALE);
    pango_layout_set_font_description(base_layout, font);
    pango_font_description_free(font);
    pango_layout_set_single_paragraph_mode(base_layout, TRUE);
    pango_layout_set_text(base_layout, "Ag", -1);

    pango_layout_get_pixel_size(base_layout, nullptr, &font_height);

    // Colors follow the order of the Syntax enum and stay readable on white.

    line_height = font_height + 4;
    header_height = line_height * 2;
    for (std::size_t i = 0; i < fold_toggle_meshes.size(); ++i) {
        const std::string marker = i == 0 ? "▾" : "▸";
        auto* layout = make_line(marker, {{0, marker.size(), Syntax::Comment}});
        renderer->upload(fold_toggle_meshes[i], {{layout, {0, 0}}});
        g_object_unref(layout);
    }

    system_theme = std::make_unique<SystemTheme>();
    initialize_workspace();
    initialize_intent();
    initialize_reload();

    popup_layout = pango_layout_copy(base_layout);
    pango_layout_set_single_paragraph_mode(popup_layout, FALSE);
    auto* popup_font = pango_font_description_from_string("JetBrains Mono");
    pango_font_description_set_absolute_size(popup_font, 14 * PANGO_SCALE);
    pango_layout_set_font_description(popup_layout, popup_font);
    pango_font_description_free(popup_font);
    pango_layout_set_wrap(popup_layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(popup_layout, PANGO_ELLIPSIZE_NONE);

    math_typesetter = std::make_unique<MathTypesetter>();
    close_message = XInternAtom(display, "WM_DELETE_WINDOW", False);
    protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    XSetWMProtocols(display, window, &close_message, 1);
    XMapWindow(display, window);

    return true;
}

int Application::benchmark_run()
{
    bool visible = false;
    for (int i = 0; i < 200; ++i) {
        XWindowAttributes attributes{};
        XGetWindowAttributes(display, window, &attributes);
        if (attributes.map_state == IsViewable) {
            visible = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!visible) {
        std::cerr << "Benchmark window did not become visible.\n";
        return 1;
    }
    initial_view = false;
    std::vector<double> milliseconds;
    for (int i = 0; i < 96; ++i) {
        camera.zoom = 0.15 * std::pow(12.0, (1.0 - std::cos(i * 0.13)) / 2.0);
        camera.home(viewport());
        const auto start = Clock::now();
        draw();
        renderer->finish();
        XSync(display, False);
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    std::cout << "Zoom draw benchmark: median " << milliseconds[48] << " ms, p95 "
              << milliseconds[91] << " ms\n";

    return 0;
}

int Application::run()
{
    if (!initialize())
        return 1;
    if (benchmark)
        return benchmark_run();

    running = !benchmark;

    auto last_frame = Clock::now();
    auto next_frame = last_frame;
    while (running) {
        if (!zoom_animation.active(camera))
            last_frame = Clock::now();
        // Drain input before advancing the camera, including during animation.
        while (running && XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);
            if (!XFilterEvent(&event, None))
                handle_event(event);
        }
        if (!running)
            break;

        if (language_server)
            for (const auto& message : language_server->take_messages())
                handle_message(message);

        if (cmake_server)
            for (const auto& message : cmake_server->take_messages())
                handle_message(message);

        const auto now = Clock::now();
        update_timers(now);
        if ((redraw || zoom_animation.active(camera)) && now >= next_frame) {
            zoom_animation.advance(camera, viewport(),
                                   std::chrono::duration<double>(now - last_frame).count());
            draw();
            redraw = false;
            last_frame = now;
            next_frame = now + std::chrono::microseconds(8333);
        }
        if (XPending(display) > 0)
            continue;

        wait_for_work(next_frame);
    }

    return reload_requested ? reload_exit_code : 0;
}

Application::~Application()
{
    if (!display)
        return;
    save_session();
    live_build.reset();
    system_theme.reset();
    if (intent)
        intent->client.reset();
    if (chat)
        chat->client.reset();
    if (input_context)
        XDestroyIC(input_context);
    if (input_method)
        XCloseIM(input_method);
    if (drag_button)
        XUngrabPointer(display, CurrentTime);
    if (normal_cursor)
        XFreeCursor(display, normal_cursor);
    if (text_cursor)
        XFreeCursor(display, text_cursor);
    if (drag_cursor)
        XFreeCursor(display, drag_cursor);
    language_server.reset();
    cmake_server.reset();
    if (popup_layout)
        g_object_unref(popup_layout);
    if (base_layout)
        g_object_unref(base_layout);
    project_watcher.reset();
    for (auto& panel : panels)
        release_panel(panel);

    for (const auto& icon : icons)
        if (icon.second)
            g_object_unref(icon.second);
    if (cr)
        cairo_destroy(cr);
    if (surface)
        cairo_surface_destroy(surface);
    if (window_destroyed && renderer)
        renderer->window_destroyed();
    renderer.reset();
    XCloseDisplay(display);
}

void Application::update_timers(Clock::time_point now)
{
    poll_project();
    poll_agent();
    poll_intent();
    if (system_theme) {
        const auto dark = system_theme->take_change();
        if (dark) {
            renderer->set_dark_theme(*dark);
            redraw = true;
        }
    }
    poll_reload(now);
    if (save_due <= now)
        save_session();
    poll_math();
    if (popup_dismiss_due <= now) {
        popup_dismiss_due = Clock::time_point::max();
        if (!Viewport{popup_bounds.origin.x, popup_bounds.origin.y, popup_bounds.size.x,
                      popup_bounds.size.y}
                 .contains(mouse_position)) {
            const auto scheduled = hover_due;
            hide_popup();
            hover_due = scheduled;
        }
    }
    if (hover_due <= now) {
        hover_due = Clock::time_point::max();
        if (!drag_button && !zoom_animation.active(camera))
            hover_request = query_at_pointer("textDocument/hover");
    }
}

void Application::wait_for_work(Clock::time_point next_frame)
{
    // Sleep until either new input or the next animation frame is due.
    // An idle window blocks indefinitely and consumes no animation CPU.
    int timeout = -1;
    if (redraw || zoom_animation.active(camera)) {
        timeout = static_cast<int>(std::max(
            0.0,
            std::ceil(
                std::chrono::duration<double, std::milli>(next_frame - Clock::now()).count())));
    }
    if (hover_due != Clock::time_point::max()) {
        const int hover_wait = static_cast<int>(std::max(
            0.0, std::ceil(
                     std::chrono::duration<double, std::milli>(hover_due - Clock::now()).count())));
        timeout = timeout < 0 ? hover_wait : std::min(timeout, hover_wait);
    }
    if (popup_dismiss_due != Clock::time_point::max()) {
        const int dismiss_wait = static_cast<int>(std::max(
            0.0,
            std::ceil(std::chrono::duration<double, std::milli>(popup_dismiss_due - Clock::now())
                          .count())));
        timeout = timeout < 0 ? dismiss_wait : std::min(timeout, dismiss_wait);
    }
    const auto pending =
        std::min({save_due, chat ? chat->reconnect : Clock::time_point::max(),
                  build_running ? Clock::time_point::max() : rebuild_due, binary_check_due,
                  intent ? intent->refresh : Clock::time_point::max()});
    if (pending != Clock::time_point::max()) {
        const int delay = static_cast<int>(std::max(
            0.0,
            std::ceil(std::chrono::duration<double, std::milli>(pending - Clock::now()).count())));
        timeout = timeout < 0 ? delay : std::min(timeout, delay);
    }
    pollfd connections[] = {{ConnectionNumber(display), POLLIN, 0},
                            {wake_fd(language_server), POLLIN, 0},
                            {wake_fd(math_typesetter), POLLIN, 0},
                            {wake_fd(project_watcher), POLLIN, 0},
                            {chat && chat->client ? chat->client->event_fd() : -1, POLLIN, 0},
                            {wake_fd(live_build), POLLIN, 0},
                            {intent && intent->client ? intent->client->event_fd() : -1, POLLIN, 0},
                            {wake_fd(system_theme), POLLIN, 0},
                            {wake_fd(cmake_server), POLLIN, 0}};
    if (poll(connections, 9, timeout) < 0 && errno != EINTR) {
        std::cerr << "Could not wait for window input.\n";
        running = false;
    }
}
