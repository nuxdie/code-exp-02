#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <iostream>

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

    XStoreName(display, window, "White Window");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);

    Atom close_message = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    XSetWMProtocols(display, window, &close_message, 1);
    XMapWindow(display, window);

    bool running = true;
    while (running) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type == Expose) {
            XClearWindow(display, window);
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

    XCloseDisplay(display);
    return 0;
}
