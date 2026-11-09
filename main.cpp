#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "source_text.h"

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
        "Display", "Window", "Atom", "GC", "XEvent", "XFontStruct", "XColor",
        "Syntax", "Token", "HighlightState"
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

    const int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, 800, 600, 0,
        WhitePixel(display, screen), WhitePixel(display, screen));

    XStoreName(display, window, "Source Code - White Window");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask |
                                 KeyPressMask | ButtonPressMask);

    XFontStruct* font = XLoadQueryFont(display, "9x15");
    if (!font) {
        font = XLoadQueryFont(display, "fixed");
    }
    if (!font) {
        std::cerr << "Could not load a monospace font.\n";
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    GC gc = XCreateGC(display, window, 0, nullptr);
    XSetFont(display, gc, font->fid);
    XSetForeground(display, gc, BlackPixel(display, screen));

    // Colors follow the order of the Syntax enum and stay readable on white.
    const std::array<const char*, 8> color_names = {
        "#24292f", "#0033b3", "#6f42c1", "#a31515",
        "#39703a", "#006b6b", "#8546a3", "#795e26"
    };
    std::array<unsigned long, 8> colors{};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        XColor color{}, exact{};
        colors[i] = XAllocNamedColor(display, DefaultColormap(display, screen),
            color_names[i], &color, &exact) ? color.pixel : BlackPixel(display, screen);
    }

    std::vector<std::string> lines;
    std::vector<std::vector<Token>> highlighted_lines;
    HighlightState highlight_state;
    std::istringstream source(source_text);
    std::string line;
    int text_width = 0;
    while (std::getline(source, line)) {
        text_width = std::max(text_width, XTextWidth(
            font, line.c_str(), static_cast<int>(line.size())));
        highlighted_lines.push_back(highlight(line, highlight_state));
        lines.push_back(line);
    }

    constexpr int margin = 16;
    const int line_height = font->ascent + font->descent + 3;
    int width = 800;
    int height = 600;
    int first_line = 0;
    int horizontal_offset = 0;

    auto visible_lines = [&]() {
        return std::max(1, (height - 2 * margin) / line_height);
    };

    auto draw = [&]() {
        first_line = std::clamp(first_line, 0,
            std::max(0, static_cast<int>(lines.size()) - visible_lines()));
        horizontal_offset = std::clamp(horizontal_offset, 0,
            std::max(0, text_width - std::max(1, width - 2 * margin)));
        XClearWindow(display, window);
        for (int row = 0; row < visible_lines(); ++row) {
            const int index = first_line + row;
            if (index >= static_cast<int>(lines.size())) {
                break;
            }
            const auto& text = lines[index];
            int x = margin - horizontal_offset;
            const int y = margin + font->ascent + row * line_height;
            for (const auto& token : highlighted_lines[index]) {
                const char* start = text.c_str() + token.begin;
                const int length = static_cast<int>(token.length);
                XSetForeground(display, gc, colors[static_cast<std::size_t>(token.syntax)]);
                XDrawString(display, window, gc, x, y, start, length);
                x += XTextWidth(font, start, length);
            }
        }
    };

    Atom close_message = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    XSetWMProtocols(display, window, &close_message, 1);
    XMapWindow(display, window);

    bool running = true;
    while (running) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type == Expose) {
            if (event.xexpose.count == 0) {
                draw();
            }
        } else if (event.type == ConfigureNotify) {
            width = event.xconfigure.width;
            height = event.xconfigure.height;
            draw();
        } else if (event.type == ButtonPress) {
            if (event.xbutton.button == Button4) {
                first_line -= 3;
            } else if (event.xbutton.button == Button5) {
                first_line += 3;
            }
            draw();
        } else if (event.type == KeyPress) {
            switch (XLookupKeysym(&event.xkey, 0)) {
            case XK_Up:    --first_line; break;
            case XK_Down:  ++first_line; break;
            case XK_Left:  horizontal_offset -= 40; break;
            case XK_Right: horizontal_offset += 40; break;
            case XK_Page_Up:   first_line -= visible_lines(); break;
            case XK_Page_Down: first_line += visible_lines(); break;
            case XK_Home: first_line = 0; horizontal_offset = 0; break;
            case XK_End: first_line = static_cast<int>(lines.size()); break;
            }
            draw();
        } else if (event.type == ClientMessage &&
                   event.xclient.message_type == protocols &&
                   event.xclient.format == 32 &&
                   static_cast<Atom>(event.xclient.data.l[0]) == close_message) {
            XDestroyWindow(display, window);
            running = false;
        } else if (event.type == DestroyNotify) {
            running = false;
        }
    }

    XFreeGC(display, gc);
    XFreeFont(display, font);
    XCloseDisplay(display);
    return 0;
}
