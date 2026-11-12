#include "syntax.h"
#include "text.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <unordered_set>

Syntax semantic_syntax(std::string_view type)
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
    LineLexer(std::string_view text, HighlightState& context, bool cmake)
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
    std::string_view line;
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
        state.raw_string_end =
            std::format("){}\"", line.substr(position + 2, opening - position - 2));
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
                     std::string_view("eEpP").contains(line[position - 1]))
                ++position;
            else
                break;
        }
        return Syntax::Number;
    }
    Syntax word()
    {
        static const std::unordered_set<std::string_view> keywords = {"alignas",
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
        static const std::unordered_set<std::string_view> types = {
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
        if (keywords.contains(word))
            return Syntax::Keyword;
        if (types.contains(word))
            return Syntax::Type;
        if (next != std::string::npos && line[next] == '(')
            return Syntax::Function;
        return Syntax::Plain;
    }
};
} // namespace
std::vector<Token> highlight(std::string_view line, HighlightState& state, bool cmake)
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

TextKind text_kind(std::string_view path)
{
    const auto slash = path.find_last_of('/');
    const auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = name.find_last_of('.');
    const auto extension = dot == std::string::npos ? std::string_view{} : name.substr(dot);
    static const std::unordered_set<std::string_view> prose{".md", ".markdown", ".txt", ".rst"};
    static const std::unordered_set<std::string_view> hash{
        ".py", ".sh", ".bash", ".zsh",  ".rb",   ".pl",  ".yml",  ".yaml",
        ".r",  ".jl", ".toml", ".conf", ".cfg",  ".ini", ".nix",  ".tcl",
        ".mk", ".ex", ".exs",  ".ps1",  ".fish", ".awk", ".cmake"};
    static const std::unordered_set<std::string_view> hash_names{"Makefile", "Dockerfile",
                                                                 ".gitignore", "CMakeLists.txt"};
    if (prose.contains(extension))
        return TextKind::Prose;
    return hash.contains(extension) || hash_names.contains(name) ? TextKind::HashCode
                                                                 : TextKind::Code;
}

namespace {
bool reads_as_prose(std::string_view line, std::size_t body)
{
    const auto last = line.find_last_not_of(" \t");
    if (body > last || std::string_view(";{}\\").contains(line[last]))
        return false;
    if (line.find("  ", body) < last)
        return false;
    return std::any_of(line.begin() + static_cast<std::ptrdiff_t>(body), line.end(),
                       [](unsigned char c) { return std::isalpha(c); });
}

std::optional<ProseLine> comment_prose(std::string_view line, const std::vector<Token>& tokens)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
        return std::nullopt;
    const auto whole = std::ranges::find_if(tokens, [&](const Token& token) {
        return token.syntax == Syntax::Comment && token.begin <= first &&
               token.begin + token.length == line.size();
    });
    if (whole == tokens.end())
        return std::nullopt;
    std::size_t body = first;
    for (const char* marker : {"///", "//!", "//", "/**", "/*", "#", "*"}) {
        const std::string text(marker);
        if (line.compare(first, text.size(), text) != 0)
            continue;
        const auto after = first + text.size();
        if (text == "*" && after < line.size() && line[after] != ' ')
            break;
        body = after;
        break;
    }
    if (body < line.size() && line[body] == ' ')
        ++body;
    const auto rest = line.find_first_not_of(" \t", body);
    if ((body > first && rest == std::string::npos) || starts_at(line, first, "*/"))
        return ProseLine{first, line.size(), false};
    if (!reads_as_prose(line, body))
        return std::nullopt;
    return ProseLine{first, body, false};
}

std::optional<ProseLine> document_prose(std::string_view line)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '|')
        return std::nullopt;
    const auto level = line.find_first_not_of('#');
    if (level != std::string::npos && level > 0 && level <= 6 && line[level] == ' ')
        return ProseLine{0, level + 1, true};
    return ProseLine{first, first, false};
}
} // namespace

StyledLine style_line(std::string_view line, TextKind kind, HighlightState& state, bool cmake)
{
    if (kind == TextKind::Prose) {
        if (line.starts_with("```")) {
            state.fenced = !state.fenced;
            return {};
        }
        if (!state.fenced)
            return {{}, document_prose(line)};
    }
    StyledLine result{highlight(line, state, cmake), std::nullopt};
    const auto first = line.find_first_not_of(" \t");
    const bool hash = kind == TextKind::HashCode && first != std::string::npos &&
                      line[first] == '#' && !starts_at(line, first, "#!");
    if (hash)
        result.tokens = {{first, line.size() - first, Syntax::Comment}};
    if (kind != TextKind::Prose)
        result.prose = comment_prose(line, result.tokens);
    return result;
}
