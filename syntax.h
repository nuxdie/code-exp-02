#pragma once
#include <optional>
#include <string>
#include <vector>

enum class Syntax {
    Plain,
    Keyword,
    Type,
    String,
    Comment,
    Number,
    Directive,
    Function,
    Variable,
    Parameter,
    Property,
    Namespace
};

struct Token {
    std::size_t begin;
    std::size_t length;
    Syntax syntax;
};

struct HighlightState {
    bool block_comment = false;
    std::string raw_string_end;
    bool fenced = false; // inside a code fence of a prose file
};

// Words meant to be read: a line that is nothing but a comment, or a line of a document.
// [begin, body) is the marker in front of the words, which is not shown.
struct ProseLine {
    std::size_t begin = 0, body = 0;
    bool heading = false;
};

enum class TextKind {
    Code,     // comments as in C and its many descendants
    HashCode, // "#" starts a comment
    Prose     // a document: every line is prose, except fenced code
};
TextKind text_kind(const std::string& path);

struct StyledLine {
    std::vector<Token> tokens;
    std::optional<ProseLine> prose;
};

Syntax semantic_syntax(const std::string& type);
std::vector<Token> highlight(const std::string& line, HighlightState& state, bool cmake = false);

// Tokens for one line of a file, and whether the line reads as prose.
StyledLine style_line(const std::string& line, TextKind kind, HighlightState& state,
                      bool cmake = false);
