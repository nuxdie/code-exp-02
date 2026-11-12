#include "text_input.h"

#include <algorithm>

std::size_t TextInput::previous(std::size_t byte) const
{
    if (byte)
        --byte;
    while (byte && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        --byte;
    return byte;
}
std::size_t TextInput::next(std::size_t byte) const
{
    byte = std::min(byte + 1, text.size());
    while (byte < text.size() && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        ++byte;
    return byte;
}
std::pair<std::size_t, std::size_t> TextInput::selection() const
{
    return std::minmax(cursor, anchor);
}
std::string TextInput::selected() const
{
    const auto [begin, end] = selection();
    return text.substr(begin, end - begin);
}
void TextInput::set(std::string value)
{
    text = std::move(value);
    cursor = anchor = text.size();
}
void TextInput::place(std::size_t byte, bool extend)
{
    cursor = std::min(byte, text.size());
    while (cursor && cursor < text.size() &&
           (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80)
        --cursor;
    if (!extend)
        anchor = cursor;
}
void TextInput::insert(std::string_view value)
{
    const auto [begin, end] = selection();
    std::string clean(value);
    std::erase(clean, '\0');
    text.replace(begin, end - begin, clean);
    cursor = anchor = begin + clean.size();
}
void TextInput::erase(bool backwards)
{
    if (cursor == anchor)
        anchor = backwards ? previous(cursor) : next(cursor);
    insert("");
}
void TextInput::move(int direction, bool extend)
{
    if (!extend && cursor != anchor) {
        const auto range = selection();
        place(direction < 0 ? range.first : range.second);
    } else
        place(direction < 0 ? previous(cursor) : next(cursor), extend);
}
void TextInput::line_end(bool end, bool extend)
{
    const auto boundary = end      ? text.find('\n', cursor)
                          : cursor ? text.rfind('\n', cursor - 1)
                                   : std::string::npos;
    const auto byte = end ? (boundary == std::string::npos ? text.size() : boundary)
                          : (boundary == std::string::npos ? 0 : boundary + 1);
    place(byte, extend);
}
void TextInput::select_all()
{
    anchor = 0;
    cursor = text.size();
}
