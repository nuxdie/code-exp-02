#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "source_text.h"

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

    std::vector<std::string> lines;
    std::istringstream source(source_text);
    std::string line;
    int text_width = 0;
    while (std::getline(source, line)) {
        text_width = std::max(text_width, XTextWidth(
            font, line.c_str(), static_cast<int>(line.size())));
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
            XDrawString(display, window, gc,
                margin - horizontal_offset,
                margin + font->ascent + row * line_height,
                text.c_str(), static_cast<int>(text.size()));
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
