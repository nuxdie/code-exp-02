#include "popup_markup.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <string_view>

namespace {
std::size_t web_url_length(const std::string& text, std::size_t begin)
{
    const auto prefix = text.compare(begin, 8, "https://") == 0 ? 8 : 7;
    if (text.compare(begin, 7, "http://") != 0 && prefix != 8)
        return 0;
    auto end = begin + prefix;
    int parentheses = 0;
    while (end < text.size()) {
        const auto c = static_cast<unsigned char>(text[end]);
        if (std::isspace(c) || c < 32 || std::string("<>\"'`[]{}").find(c) != std::string::npos)
            break;
        if (c == ')' && parentheses == 0)
            break;
        parentheses += c == '(' ? 1 : c == ')' ? -1 : 0;
        ++end;
    }
    while (end > begin + prefix && std::string(".,;:!?").find(text[end - 1]) != std::string::npos)
        --end;
    return end > begin + prefix ? end - begin : 0;
}

std::string decode_entities(std::string text)
{
    const std::pair<std::string_view, std::string_view> entities[] = {
        {"&lt;", "<"},  {"&gt;", ">"},   {"&quot;", "\""}, {"&apos;", "'"},
        {"&#39;", "'"}, {"&#x27;", "'"}, {"&nbsp;", " "},  {"&amp;", "&"}};
    for (const auto& [entity, replacement] : entities) {
        std::size_t position = 0;
        while ((position = text.find(entity, position)) != std::string::npos) {
            text.replace(position, entity.size(), replacement);
            position += replacement.size();
        }
    }
    return text;
}

std::string tag_name(std::string tag)
{
    if (tag.empty())
        return {};
    std::transform(tag.begin(), tag.end(), tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tag.substr(0, tag.find_first_of(" \t/", tag[0] == '/' ? 1 : 0));
}

class HtmlDocumentation {
  public:
    std::string convert(const std::string& text)
    {
        std::istringstream input(text);
        std::string line, output;
        bool fenced = false;
        while (std::getline(input, line)) {
            const auto first = line.find_first_not_of(" \t");
            const bool fence = first != std::string::npos && (line.compare(first, 3, "```") == 0 ||
                                                              line.compare(first, 3, "~~~") == 0);
            if (!pre && fence)
                fenced = !fenced;
            output += !pre && (fenced || fence) ? line : convert_line(line);
            output += '\n';
        }
        return output;
    }

  private:
    bool pre = false;
    bool code = false;

    std::string convert_tag(const std::string& name, const std::string& literal)
    {
        if (name == "pre") {
            pre = true;
            return "\n```cpp\n";
        }
        if (name == "/pre") {
            pre = false;
            return "\n```\n";
        }
        if (name == "code" || name == "/code") {
            code = name == "code";
            return pre ? "" : "`";
        }
        if (!pre && !code && (name == "strong" || name == "/strong" || name == "b" || name == "/b"))
            return "**";
        if (!pre && !code && (name == "em" || name == "/em" || name == "i" || name == "/i"))
            return "*";
        if (!pre && !code && name.size() == 2 && name[0] == 'h' && name[1] >= '1' &&
            name[1] <= '6') {
            return "\n### ";
        }
        if (!pre && !code && name.size() == 3 && name[0] == '/' && name[1] == 'h' &&
            name[2] >= '1' && name[2] <= '6')
            return "\n";
        if (pre || code)
            return literal;
        if (name == "br" || name == "p" || name == "/p" || name == "div" || name == "/div")
            return "\n";
        if (name == "li")
            return "\n• ";
        const std::vector<std::string> formatting{"em", "/em", "strong", "/strong", "b",    "/b",
                                                  "i",  "/i",  "a",      "/a",      "span", "/span",
                                                  "ul", "/ul", "ol",     "/ol",     "/li"};
        return std::find(formatting.begin(), formatting.end(), name) != formatting.end() ? ""
                                                                                         : literal;
    }

    std::string convert_line(const std::string& line)
    {
        std::string result;
        std::size_t position = 0;
        while (position < line.size()) {
            if (!pre && !code && line[position] == '`') {
                const auto end = line.find('`', position + 1);
                if (end != std::string::npos) {
                    result += line.substr(position, end - position + 1);
                    position = end + 1;
                    continue;
                }
            }
            if (line[position] != '<') {
                result += line[position++];
                continue;
            }
            const auto end = line.find('>', position + 1);
            if (end == std::string::npos) {
                result += line.substr(position);
                break;
            }
            const auto literal = line.substr(position, end - position + 1);
            result += convert_tag(tag_name(line.substr(position + 1, end - position - 1)), literal);
            position = end + 1;
        }
        return decode_entities(result);
    }
};

std::string inline_literals(const std::string& source)
{
    std::string result;
    for (std::size_t position = 0; position < source.size(); ++position) {
        if (source.compare(position, 2, "&#") == 0) {
            const auto end = source.find(';', position + 2);
            unsigned value = 0;
            if (end != std::string::npos && end - position < 8) {
                const auto converted =
                    std::from_chars(source.data() + position + 2, source.data() + end, value);
                if (converted.ec == std::errc{} && converted.ptr == source.data() + end &&
                    value >= 32 && value <= 126) {
                    result += static_cast<char>(value);
                    position = end;
                    continue;
                }
            }
        }
        result += source[position];
    }
    return result;
}

class InlineDocumentation {
  public:
    InlineDocumentation(PopupDocument& target, std::size_t start) : document(target), offset(start)
    {
    }

    std::string parse(const std::string& line)
    {
        std::size_t position = 0;
        while (position < line.size()) {
            if (web_url(line, position) || entity(line, position) || math(line, position) ||
                code(line, position) || emphasis(line, position) || link(line, position))
                continue;
            result += line[position++];
        }
        return result;
    }

  private:
    PopupDocument& document;
    std::size_t offset;
    std::string result;

    bool web_url(const std::string& line, std::size_t& position)
    {
        const bool angle = line[position] == '<';
        const auto begin = position + (angle ? 1 : 0);
        const auto length = web_url_length(line, begin);
        if (!length)
            return false;
        result += line.substr(begin, length);
        position = begin + length;
        if (angle && position < line.size() && line[position] == '>')
            ++position;
        return true;
    }

    bool entity(const std::string& line, std::size_t& position)
    {
        if (line.compare(position, 2, "&#") != 0)
            return false;
        const auto end = line.find(';', position + 2);
        if (end == std::string::npos)
            return false;
        const auto encoded = line.substr(position, end - position + 1);
        const auto decoded = inline_literals(encoded);
        if (decoded == encoded)
            return false;
        result += decoded;
        position = end + 1;
        return true;
    }

    bool math(const std::string& line, std::size_t& position)
    {
        std::string open, close;
        for (const auto& pair : {std::pair<std::string, std::string>{"$$", "$$"},
                                 {"$", "$"},
                                 {"\\(", "\\)"},
                                 {"\\[", "\\]"}}) {
            if (line.compare(position, pair.first.size(), pair.first) == 0) {
                open = pair.first;
                close = pair.second;
                break;
            }
        }
        if (open.empty())
            return false;
        const auto end = line.find(close, position + open.size());
        if (end == std::string::npos || end == position + open.size())
            return false;
        document.math.push_back({offset + result.size(),
                                 line.substr(position + open.size(), end - position - open.size()),
                                 open == "$$" || open == "\\["});
        result += "\xef\xbf\xbc";
        position = end + close.size();
        return true;
    }

    bool code(const std::string& line, std::size_t& position)
    {
        if (line[position] != '`')
            return false;
        const auto content = line.find_first_not_of('`', position);
        if (content == std::string::npos)
            return false;
        const auto delimiter = line.substr(position, content - position);
        const auto end = line.find(delimiter, content);
        if (end == std::string::npos)
            return false;
        document.inline_code.emplace_back(offset + result.size(), end - content);
        result += line.substr(content, end - content);
        position = end + delimiter.size();
        return true;
    }

    bool emphasis(const std::string& line, std::size_t& position)
    {
        const char marker = line[position];
        if (marker != '*' && marker != '_')
            return false;
        const bool strong = position + 1 < line.size() && line[position + 1] == marker;
        const std::string delimiter(strong ? 2 : 1, marker);
        const auto content = position + delimiter.size();
        const auto end = line.find(delimiter, content);
        if (end == std::string::npos || end == content)
            return false;
        // Escaped characters inside emphasis are still literal text, not markup.
        const auto text = inline_literals(line.substr(content, end - content));
        document.styles.push_back({offset + result.size(), text.size(),
                                   strong ? PopupStyle::Strong : PopupStyle::Emphasis});
        result += text;
        position = end + delimiter.size();
        return true;
    }

    bool link(const std::string& line, std::size_t& position)
    {
        if (line[position] != '[')
            return false;
        const auto middle = line.find("](", position + 1);
        if (middle == std::string::npos)
            return false;
        auto end = middle + 2;
        int depth = 1;
        for (; end < line.size(); ++end) {
            depth += line[end] == '(' ? 1 : line[end] == ')' ? -1 : 0;
            if (depth == 0)
                break;
        }
        if (end == line.size())
            return false;
        const auto label = inline_literals(line.substr(position + 1, middle - position - 1));
        const auto length = label.size();
        auto destination = line.substr(middle + 2, end - middle - 2);
        if (destination.size() >= 2 && destination.front() == '<' && destination.back() == '>')
            destination = destination.substr(1, destination.size() - 2);
        document.styles.push_back({offset + result.size(), length, PopupStyle::Link, destination});
        result += label;
        position = end + 1;
        return true;
    }
};

bool format_prose(std::string& line)
{
    bool heading = false;
    if (!line.empty() && line[0] == '#') {
        const auto end = line.find_first_not_of('#');
        if (end != std::string::npos && end <= 6 && line[end] == ' ') {
            line.erase(0, end + 1);
            heading = true;
        }
    }
    if (line.compare(0, 2, "- ") == 0 || line.compare(0, 2, "* ") == 0)
        line.replace(0, 2, "• ");
    if (line == "---")
        line.clear();
    return heading;
}

void join_display_math(std::istream& input, std::string& line)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
        return;
    const auto marker = line.substr(first);
    if (marker != "$$" && marker != "\\[")
        return;
    const std::string close = marker == "$$" ? "$$" : "\\]";
    std::string next, body;
    while (std::getline(input, next)) {
        const auto begin = next.find_first_not_of(" \t");
        if (begin != std::string::npos && next.substr(begin) == close) {
            line = marker + body + close;
            return;
        }
        body += next + " ";
    }
    line += " " + body;
}

std::string unescape_documentation(const std::string& line)
{
    std::string result;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '`') {
            const auto end = line.find('`', i + 1);
            if (end != std::string::npos) {
                result += line.substr(i, end - i + 1);
                i = end;
                continue;
            }
        }
        if (line[i] == '\\' && i + 1 < line.size() &&
            std::string("\\`*_{}[]()#+-.!<>|$").find(line[i + 1]) != std::string::npos)
            ++i;
        result += line[i];
    }
    return result;
}

void add_web_links(PopupDocument& document)
{
    for (const auto& link : web_links(document.text)) {
        const bool covered =
            std::any_of(document.styles.begin(), document.styles.end(), [&](const auto& span) {
                return span.style == PopupStyle::Link && link.begin < span.begin + span.length &&
                       span.begin < link.begin + link.length;
            });
        if (!covered)
            document.styles.push_back(link);
    }
}

} // namespace

std::vector<PopupSpan> web_links(const std::string& text)
{
    std::vector<PopupSpan> links;
    auto begin = text.find("http");
    while (begin != std::string::npos) {
        const auto length = web_url_length(text, begin);
        if (length)
            links.push_back({begin, length, PopupStyle::Link, text.substr(begin, length)});
        begin = text.find("http", begin + std::max(std::size_t{1}, length));
    }
    return links;
}

PopupDocument popup_document(const std::string& text, bool markup)
{
    PopupDocument document;
    HtmlDocumentation html;
    std::istringstream input(markup ? html.convert(text) : text);
    std::string line;
    std::string fence;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        const auto prefix = first == std::string::npos ? "" : line.substr(first, 3);
        if (markup && (prefix == "```" || prefix == "~~~")) {
            if (fence.empty())
                fence = prefix;
            else if (fence == prefix)
                fence.clear();
            continue;
        }
        if (markup && fence.empty())
            join_display_math(input, line);
        const bool code = markup && !fence.empty();
        const bool heading = markup && fence.empty() && format_prose(line);
        if (!document.code_lines.empty())
            document.text += '\n';
        if (markup && fence.empty()) {
            InlineDocumentation inline_text(document, document.text.size());
            line = inline_text.parse(line);
        }
        if (heading)
            document.styles.push_back({document.text.size(), line.size(), PopupStyle::Heading});
        document.text += line;
        document.code_lines.push_back(code);
    }
    add_web_links(document);
    return document;
}

std::string hover_documentation(const std::string& markdown)
{
    std::istringstream input(markdown);
    std::string line, result;
    bool literal_fence = false;
    bool recovered_fence = false;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        const bool fence = first != std::string::npos && line.compare(first, 3, "```") == 0;
        if (fence && !recovered_fence)
            literal_fence = !literal_fence;
        else if (!literal_fence) {
            line = unescape_documentation(line);
            const auto marker = line.find_first_not_of(" \t");
            if (marker != std::string::npos && line.compare(marker, 3, "```") == 0) {
                recovered_fence = !recovered_fence;
                const auto body = line.find(' ', marker + 3);
                if (recovered_fence && body != std::string::npos)
                    line[body] = '\n';
            }
            if (!recovered_fence && line.compare(0, 7, "@brief ") == 0)
                line.erase(0, 7);
        }
        if (!result.empty())
            result += '\n';
        result += line;
    }
    return result;
}

std::string popup_literal(const std::string& text)
{
    std::string escaped;
    for (const char byte : text) {
        if (std::string("&<>*_`[]$\\#~-+").find(byte) != std::string::npos)
            escaped += "&#" + std::to_string(static_cast<unsigned char>(byte)) + ";";
        else
            escaped += byte;
    }
    return escaped;
}
