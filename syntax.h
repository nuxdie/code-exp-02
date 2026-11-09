#pragma once
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
};

Syntax semantic_syntax(const std::string& type);
std::vector<Token> highlight(const std::string& line, HighlightState& state, bool cmake = false);
