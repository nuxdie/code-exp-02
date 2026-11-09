#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <cairo-xlib.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "camera.h"
#include "source_text.h"

// Style complete lines so programming ligatures keep their natural spacing.
enum class Syntax { Plain, Keyword, Type, String, Comment, Number, Directive, Function };

struct Token {
    std::size_t begin;
    std::size_t length;
    Syntax syntax;
};

struct HighlightState {
    bool block_comment = false;
    std::string raw_string_end;
};

// Keep comment and raw string state between lines, even when they are offscreen.
std::vector<Token> highlight(const std::string& line, HighlightState& state)
{
    static const std::unordered_set<std::string> keywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "break", "case", "catch", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "dynamic_cast", "else",
        "enum", "explicit", "export", "extern", "false", "for", "friend", "goto",
        "if", "inline", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "using", "virtual", "volatile", "while", "xor", "xor_eq"
    };
    static const std::unordered_set<std::string> types = {
        "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int",
        "long", "short", "signed", "unsigned", "void", "wchar_t", "size_t",
        "string", "vector", "array", "unordered_set", "istringstream",
        "Display", "Window", "Atom", "XEvent", "PangoLayout", "PangoAttribute",
        "PangoAttrList", "PangoFontDescription", "cairo_t", "cairo_surface_t",
        "Syntax", "Token", "HighlightState", "Vec2", "Viewport", "Camera"
    };
    auto identifier = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    std::vector<Token> tokens;
    const auto first = line.find_first_not_of(" \t");
    bool include_header = false;
    std::size_t i = 0;
    while (i < line.size()) {
        const auto begin = i;
        Syntax syntax = Syntax::Plain;
        if (state.block_comment) {
            const auto end = line.find("*/", i);
            i = end == std::string::npos ? line.size() : end + 2;
            state.block_comment = end == std::string::npos;
            syntax = Syntax::Comment;
        } else if (!state.raw_string_end.empty()) {
            const auto end = line.find(state.raw_string_end, i);
            i = end == std::string::npos ? line.size() : end + state.raw_string_end.size();
            if (end != std::string::npos) state.raw_string_end.clear();
            syntax = Syntax::String;
        } else if (line.compare(i, 2, "//") == 0) {
            i = line.size();
            syntax = Syntax::Comment;
        } else if (line.compare(i, 2, "/*") == 0) {
            state.block_comment = true;
            i += 2;
            const auto end = line.find("*/", i);
            i = end == std::string::npos ? line.size() : end + 2;
            state.block_comment = end == std::string::npos;
            syntax = Syntax::Comment;
        } else if (line.compare(i, 2, "R\"") == 0 &&
                   line.find('(', i + 2) != std::string::npos &&
                   line.find('(', i + 2) - (i + 2) <= 16) {
            const auto opening = line.find('(', i + 2);
            state.raw_string_end = ")" + line.substr(i + 2, opening - i - 2) + "\"";
            const auto end = line.find(state.raw_string_end, opening + 1);
            i = end == std::string::npos ? line.size() : end + state.raw_string_end.size();
            if (end != std::string::npos) state.raw_string_end.clear();
            syntax = Syntax::String;
        } else if (line[i] == '"' || line[i] == '\'') {
            const char quote = line[i++];
            while (i < line.size()) {
                if (line[i] == '\\') {
                    i += std::min<std::size_t>(2, line.size() - i);
                } else if (line[i++] == quote) {
                    break;
                }
            }
            syntax = Syntax::String;
        } else if (i == first && line[i] == '#') {
            ++i;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            const auto directive = i;
            while (i < line.size() && identifier(line[i])) ++i;
            include_header = line.substr(directive, i - directive) == "include";
            syntax = Syntax::Directive;
        } else if (include_header && line[i] == '<') {
            const auto end = line.find('>', i + 1);
            i = end == std::string::npos ? line.size() : end + 1;
            syntax = Syntax::String;
        } else if (std::isdigit(static_cast<unsigned char>(line[i])) ||
                   (line[i] == '.' && i + 1 < line.size() &&
                    std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            ++i;
            while (i < line.size()) {
                if (identifier(line[i]) || line[i] == '.' || line[i] == '\'') {
                    ++i;
                } else if ((line[i] == '+' || line[i] == '-') &&
                           std::string("eEpP").find(line[i - 1]) != std::string::npos) {
                    ++i;
                } else {
                    break;
                }
            }
            syntax = Syntax::Number;
        } else if (identifier(line[i])) {
            while (i < line.size() && identifier(line[i])) ++i;
            const auto word = line.substr(begin, i - begin);
            const auto next = line.find_first_not_of(" \t", i);
            if (keywords.count(word)) syntax = Syntax::Keyword;
            else if (types.count(word)) syntax = Syntax::Type;
            else if (next != std::string::npos && line[next] == '(') syntax = Syntax::Function;
        } else {
            ++i;
        }
        if (!tokens.empty() && tokens.back().syntax == syntax) {
            tokens.back().length += i - begin;
        } else {
            tokens.push_back({begin, i - begin, syntax});
        }
    }
    return tokens;
}

int main()
{
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Could not open the X display.\n";
        return 1;
    }

    // Load the bundled font faces for this application only.
    std::error_code path_error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", path_error);
    if (path_error) {
        std::cerr << "Could not locate the executable: " << path_error.message() << '\n';
        XCloseDisplay(display);
        return 1;
    }
    for (const char* face : {"Regular", "Bold", "Italic"}) {
        const auto path = executable.parent_path() / "fonts" /
            (std::string("JetBrainsMono-") + face + ".ttf");
        if (!FcConfigAppFontAddFile(FcConfigGetCurrent(),
                reinterpret_cast<const FcChar8*>(path.c_str()))) {
            std::cerr << "Could not load bundled font: " << path << '\n';
            XCloseDisplay(display);
            return 1;
        }
    }

    const int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, 800, 600, 0,
        WhitePixel(display, screen), WhitePixel(display, screen));

    XStoreName(display, window, "Source Code - White Window");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask |
        KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    Cursor hand_cursor = XCreateFontCursor(display, XC_hand1);
    Cursor drag_cursor = XCreateFontCursor(display, XC_fleur);
    XDefineCursor(display, window, hand_cursor);

    cairo_surface_t* surface = cairo_xlib_surface_create(
        display, window, DefaultVisual(display, screen), 800, 600);
    cairo_t* cr = cairo_create(surface);
    auto* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_set_font_options(cr, options);

    PangoLayout* base_layout = pango_cairo_create_layout(cr);
    pango_cairo_context_set_font_options(pango_layout_get_context(base_layout), options);
    cairo_font_options_destroy(options);
    PangoFontDescription* font = pango_font_description_from_string("JetBrains Mono");
    pango_font_description_set_absolute_size(font, 16 * PANGO_SCALE);
    pango_layout_set_font_description(base_layout, font);
    pango_font_description_free(font);
    pango_layout_set_single_paragraph_mode(base_layout, TRUE);
    pango_layout_set_text(base_layout, "Ag", -1);
    int font_height = 0;
    pango_layout_get_pixel_size(base_layout, nullptr, &font_height);

    // Colors follow the order of the Syntax enum and stay readable on white.
    const std::array<unsigned, 8> colors = {
        0x24292f, 0x0033b3, 0x6f42c1, 0xa31515,
        0x39703a, 0x006b6b, 0x8546a3, 0x795e26
    };

    std::vector<PangoLayout*> layouts;
    HighlightState highlight_state;
    std::istringstream source(source_text);
    std::string line;
    int text_width = 0;
    while (std::getline(source, line)) {
        PangoLayout* layout = pango_layout_copy(base_layout);
        pango_layout_set_text(layout, line.c_str(), static_cast<int>(line.size()));
        PangoAttrList* attributes = pango_attr_list_new();
        pango_attr_list_insert(attributes,
            pango_attr_font_features_new("liga=1,clig=1,calt=1"));
        for (const auto& token : highlight(line, highlight_state)) {
            auto add = [&](PangoAttribute* attribute) {
                attribute->start_index = static_cast<guint>(token.begin);
                attribute->end_index = static_cast<guint>(token.begin + token.length);
                pango_attr_list_insert(attributes, attribute);
            };
            const auto color = colors[static_cast<std::size_t>(token.syntax)];
            add(pango_attr_foreground_new(((color >> 16) & 255) * 257,
                ((color >> 8) & 255) * 257, (color & 255) * 257));
            if (token.syntax == Syntax::Keyword || token.syntax == Syntax::Directive) {
                add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            } else if (token.syntax == Syntax::Comment) {
                add(pango_attr_style_new(PANGO_STYLE_ITALIC));
            }
        }
        pango_layout_set_attributes(layout, attributes);
        pango_attr_list_unref(attributes);
        int line_width = 0;
        pango_layout_get_pixel_size(layout, &line_width, nullptr);
        text_width = std::max(text_width, line_width);
        layouts.push_back(layout);
    }
    g_object_unref(base_layout);

    const int line_height = font_height + 4;
    int width = 800;
    int height = 600;
    auto viewport = [&]() -> Viewport {
        return {0, 0, static_cast<double>(std::max(1, width)),
            static_cast<double>(std::max(1, height))};
    };
    Camera camera;
    camera.reset(viewport());
    const Vec2 content_size{static_cast<double>(text_width + 4),
        static_cast<double>(layouts.size()) * line_height};
    unsigned int drag_button = 0;
    Vec2 last_pointer;

    auto draw = [&]() {
        const auto view = viewport();
        // Paint a complete frame before presenting it to avoid scroll flicker.
        cairo_push_group(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_save(cr);
        cairo_rectangle(cr, view.x, view.y, view.width, view.height);
        cairo_clip(cr);
        const auto origin = view.center();
        cairo_translate(cr, origin.x, origin.y);
        cairo_scale(cr, camera.zoom, camera.zoom);
        cairo_translate(cr, -camera.center.x, -camera.center.y);

        // Cull in world space; partial lines remain visible at viewport edges.
        const double top = camera.screen_to_world({view.x, view.y}, view).y;
        const double bottom = top + view.height / camera.zoom;
        const double count = static_cast<double>(layouts.size());
        const int first = static_cast<int>(std::clamp(std::floor(top / line_height) - 1, 0.0, count));
        const int last = static_cast<int>(std::clamp(std::ceil(bottom / line_height) + 1, 0.0, count));
        for (int index = first; index < last; ++index) {
            cairo_move_to(cr, 0, index * line_height);
            pango_cairo_update_layout(cr, layouts[index]);
            pango_cairo_show_layout(cr, layouts[index]);
        }
        cairo_restore(cr);

        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
        cairo_surface_flush(surface);
        XFlush(display);
    };

    Atom close_message = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    XSetWMProtocols(display, window, &close_message, 1);
    XMapWindow(display, window);

    bool running = true;
    bool window_destroyed = false;
    bool redraw = true;
    while (running) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type == Expose) {
            if (event.xexpose.count == 0) {
                redraw = true;
            }
        } else if (event.type == ConfigureNotify) {
            width = event.xconfigure.width;
            height = event.xconfigure.height;
            cairo_xlib_surface_set_size(surface, width, height);
            redraw = true;
        } else if (event.type == ButtonPress) {
            const Vec2 pointer{static_cast<double>(event.xbutton.x),
                static_cast<double>(event.xbutton.y)};
            const auto view = viewport();
            if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
                if (drag_button) {
                    camera.pan({pointer.x - last_pointer.x, pointer.y - last_pointer.y});
                    last_pointer = pointer;
                }
                camera.zoom_at(event.xbutton.button == Button4 ? 1.15 : 1 / 1.15,
                    drag_button || view.contains(pointer) ? pointer : view.center(), view);
            } else if ((event.xbutton.button == Button1 || event.xbutton.button == Button2) &&
                       !drag_button && view.contains(pointer)) {
                // Keep receiving wheel presses while either pan button is held.
                if (XGrabPointer(display, window, False,
                        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                        GrabModeAsync, GrabModeAsync, None, drag_cursor, CurrentTime) == GrabSuccess) {
                    drag_button = event.xbutton.button;
                    last_pointer = pointer;
                }
            }
            redraw = true;
        } else if (event.type == MotionNotify && drag_button) {
            const Vec2 pointer{static_cast<double>(event.xmotion.x),
                static_cast<double>(event.xmotion.y)};
            camera.pan({pointer.x - last_pointer.x, pointer.y - last_pointer.y});
            last_pointer = pointer;
            redraw = true;
        } else if (event.type == ButtonRelease && event.xbutton.button == drag_button) {
            drag_button = 0;
            XUngrabPointer(display, CurrentTime);
        } else if (event.type == KeyPress) {
            const auto view = viewport();
            switch (XLookupKeysym(&event.xkey, 0)) {
            case XK_Up:    camera.pan({0, 40}); break;
            case XK_Down:  camera.pan({0, -40}); break;
            case XK_Left:  camera.pan({40, 0}); break;
            case XK_Right: camera.pan({-40, 0}); break;
            case XK_Page_Up:   camera.pan({0, view.height * 0.9}); break;
            case XK_Page_Down: camera.pan({0, -view.height * 0.9}); break;
            case XK_Home: camera.home(view); break;
            case XK_End:
                camera.home(view);
                camera.center.y += std::max(0.0, content_size.y - view.height / camera.zoom);
                break;
            case XK_0: case XK_KP_0: camera.reset(view); break;
            case XK_f: case XK_F: camera.fit(content_size, view); break;
            case XK_equal: case XK_plus: case XK_KP_Add:
                camera.zoom_at(1.15, view.center(), view); break;
            case XK_minus: case XK_KP_Subtract:
                camera.zoom_at(1 / 1.15, view.center(), view); break;
            case XK_Escape:
                drag_button = 0;
                XUngrabPointer(display, CurrentTime);
                break;
            }
            redraw = true;
        } else if (event.type == ClientMessage &&
                   event.xclient.message_type == protocols &&
                   event.xclient.format == 32 &&
                   static_cast<Atom>(event.xclient.data.l[0]) == close_message) {
            running = false;
        } else if (event.type == DestroyNotify) {
            window_destroyed = true;
            running = false;
        }
        // Apply queued pan and zoom input together before presenting a frame.
        if (running && redraw && XPending(display) == 0) {
            draw();
            redraw = false;
        }
    }

    if (drag_button) XUngrabPointer(display, CurrentTime);
    XFreeCursor(display, hand_cursor);
    XFreeCursor(display, drag_cursor);
    for (auto* layout : layouts) g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    if (!window_destroyed) XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
