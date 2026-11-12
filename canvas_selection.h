#pragma once

#include "gpu.h"
#include "popup_markup.h"
#include "text_input.h"

#include <string>

// Byte ranges use the same UTF-8 text that Pango shapes and that copying returns.
enum class TextSurface { Inactive, Source, Popup, Chat, Composer, Search };
struct ChatCanvas;
struct TextTarget {
    TextSurface surface = TextSurface::Inactive;
    std::string key;
    const std::string* text = nullptr;
    PangoLayout* layout = nullptr;
    Vec2 origin{};
    double scale = 1;
    std::size_t prefix = 0;
};
struct CanvasSelection {
    const ChatCanvas* chat = nullptr;
    TextSurface surface = TextSurface::Inactive;
    std::string key;
    TextInput range;
    Vec2 press{};
    bool dragging = false, moved = false, extend = false;
};
