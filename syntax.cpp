#include "syntax.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>

Syntax semantic_syntax(const std::string& type)
{
    if (type == "function" || type == "method")
        return Syntax::Function;
    if (type == "variable")
        return Syntax::Variable;
    if (type == "parameter")
        return Syntax::Parameter;
    if (type == "property")
        return Syntax::Property;
    if (type == "namespace")
        return Syntax::Namespace;
    if (type == "macro")
        return Syntax::Directive;
    if (type == "keyword" || type == "modifier")
        return Syntax::Keyword;
    if (type == "comment")
        return Syntax::Comment;
    if (type == "string")
        return Syntax::String;
    if (type == "number" || type == "enumMember")
        return Syntax::Number;
    if (type == "type" || type == "class" || type == "struct" || type == "enum" ||
        type == "interface" || type == "typeParameter" || type == "concept")
        return Syntax::Type;
    return Syntax::Plain;
}

namespace {
class LineLexer {
  public:
    LineLexer(const std::string& text, HighlightState& context, bool cmake)
        : line(text), state(context), cmake(cmake), first(text.find_first_not_of(" \t"))
    {
    }
    bool done() const { return position >= line.size(); }
    Token next()
    {
        const auto begin = position;
        Syntax kind = Syntax::Plain;
        if (state.block_comment)
            kind = block_comment();
        else if (!state.raw_string_end.empty())
            kind = raw_body(position);
        else if ((cmake && line[position] == '#') || match("//")) {
            position = line.size();
            kind = Syntax::Comment;
        } else if (match("/*")) {
            position += 2;
            kind = block_comment();
        } else if (raw_opening())
            kind = raw_string();
        else if (line[position] == '"' || line[position] == '\'')
            kind = quoted();
        else if (position == first && line[position] == '#')
            kind = directive();
        else if (include_header && line[position] == '<')
            kind = header();
        else if (number_start())
            kind = number();
        else if (identifier(line[position]))
            kind = word();
        else
            ++position;
        return {begin, position - begin, kind};
    }

  private:
    const std::string& line;
    HighlightState& state;
    bool cmake;
    std::size_t first;
    std::size_t position = 0;
    bool include_header = false;
    static bool identifier(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }
    bool match(const char* text) const { return line.compare(position, 2, text) == 0; }
    Syntax block_comment()
    {
        const auto end = line.find("*/", position);
        position = end == std::string::npos ? line.size() : end + 2;
        state.block_comment = end == std::string::npos;
        return Syntax::Comment;
    }
    bool raw_opening() const
    {
        if (!match("R\""))
            return false;
        const auto opening = line.find('(', position + 2);
        return opening != std::string::npos && opening - position - 2 <= 16;
    }
    Syntax raw_body(std::size_t start)
    {
        const auto end = line.find(state.raw_string_end, start);
        position = end == std::string::npos ? line.size() : end + state.raw_string_end.size();
        if (end != std::string::npos)
            state.raw_string_end.clear();
        return Syntax::String;
    }
    Syntax raw_string()
    {
        const auto opening = line.find('(', position + 2);
        state.raw_string_end = ")" + line.substr(position + 2, opening - position - 2) + "\"";
        return raw_body(opening + 1);
    }
    Syntax quoted()
    {
        const char quote = line[position++];
        while (!done()) {
            if (line[position] == '\\')
                position += std::min<std::size_t>(2, line.size() - position);
            else if (line[position++] == quote)
                break;
        }
        return Syntax::String;
    }
    Syntax directive()
    {
        ++position;
        while (!done() && (line[position] == ' ' || line[position] == '\t'))
            ++position;
        const auto begin = position;
        while (!done() && identifier(line[position]))
            ++position;
        include_header = line.substr(begin, position - begin) == "include";
        return Syntax::Directive;
    }
    Syntax header()
    {
        const auto end = line.find('>', position + 1);
        position = end == std::string::npos ? line.size() : end + 1;
        return Syntax::String;
    }
    bool number_start() const
    {
        if (std::isdigit(static_cast<unsigned char>(line[position])))
            return true;
        return line[position] == '.' && position + 1 < line.size() &&
               std::isdigit(static_cast<unsigned char>(line[position + 1]));
    }
    Syntax number()
    {
        ++position;
        while (!done()) {
            const char c = line[position];
            if (identifier(c) || c == '.' || c == '\'')
                ++position;
            else if ((c == '+' || c == '-') &&
                     std::string("eEpP").find(line[position - 1]) != std::string::npos)
                ++position;
            else
                break;
        }
        return Syntax::Number;
    }
    Syntax word()
    {
        static const std::unordered_set<std::string> keywords = {"alignas",
                                                                 "alignof",
                                                                 "and",
                                                                 "and_eq",
                                                                 "asm",
                                                                 "auto",
                                                                 "bitand",
                                                                 "bitor",
                                                                 "break",
                                                                 "case",
                                                                 "catch",
                                                                 "class",
                                                                 "compl",
                                                                 "concept",
                                                                 "const",
                                                                 "consteval",
                                                                 "constexpr",
                                                                 "constinit",
                                                                 "const_cast",
                                                                 "continue",
                                                                 "co_await",
                                                                 "co_return",
                                                                 "co_yield",
                                                                 "decltype",
                                                                 "default",
                                                                 "delete",
                                                                 "do",
                                                                 "dynamic_cast",
                                                                 "else",
                                                                 "enum",
                                                                 "explicit",
                                                                 "export",
                                                                 "extern",
                                                                 "false",
                                                                 "for",
                                                                 "friend",
                                                                 "goto",
                                                                 "if",
                                                                 "inline",
                                                                 "mutable",
                                                                 "namespace",
                                                                 "new",
                                                                 "noexcept",
                                                                 "not",
                                                                 "not_eq",
                                                                 "nullptr",
                                                                 "operator",
                                                                 "or",
                                                                 "or_eq",
                                                                 "private",
                                                                 "protected",
                                                                 "public",
                                                                 "register",
                                                                 "reinterpret_cast",
                                                                 "requires",
                                                                 "return",
                                                                 "sizeof",
                                                                 "static",
                                                                 "static_assert",
                                                                 "static_cast",
                                                                 "struct",
                                                                 "switch",
                                                                 "template",
                                                                 "this",
                                                                 "thread_local",
                                                                 "throw",
                                                                 "true",
                                                                 "try",
                                                                 "typedef",
                                                                 "typeid",
                                                                 "typename",
                                                                 "union",
                                                                 "using",
                                                                 "virtual",
                                                                 "volatile",
                                                                 "while",
                                                                 "xor",
                                                                 "xor_eq"};
        static const std::unordered_set<std::string> types = {
            "bool",           "char",
            "char8_t",        "char16_t",
            "char32_t",       "double",
            "float",          "int",
            "long",           "short",
            "signed",         "unsigned",
            "void",           "wchar_t",
            "size_t",         "string",
            "vector",         "array",
            "unordered_set",  "istringstream",
            "Display",        "Window",
            "Atom",           "XEvent",
            "PangoLayout",    "PangoAttribute",
            "PangoAttrList",  "PangoFontDescription",
            "cairo_t",        "cairo_surface_t",
            "Syntax",         "Token",
            "HighlightState", "Vec2",
            "Viewport",       "Camera",
            "ZoomAnimation",  "SourcePanel",
            "CanvasRect",     "EmbeddedSource"};

        const auto begin = position;
        while (!done() && identifier(line[position]))
            ++position;
        const auto word = line.substr(begin, position - begin);
        const auto next = line.find_first_not_of(" \t", position);
        if (keywords.count(word))
            return Syntax::Keyword;
        if (types.count(word))
            return Syntax::Type;
        if (next != std::string::npos && line[next] == '(')
            return Syntax::Function;
        return Syntax::Plain;
    }
};
} // namespace
std::vector<Token> highlight(const std::string& line, HighlightState& state, bool cmake)
{
    LineLexer lexer(line, state, cmake);
    std::vector<Token> tokens;
    while (!lexer.done()) {
        const auto token = lexer.next();
        if (!tokens.empty() && tokens.back().syntax == token.syntax)
            tokens.back().length += token.length;
        else
            tokens.push_back(token);
    }
    return tokens;
}
