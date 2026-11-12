#include "syntax.h"

#include <iostream>
#include <stdexcept>

Syntax kind(const std::string& line, const std::string& fragment, HighlightState& state)
{
    const auto position = line.find(fragment);
    if (position == std::string::npos)
        throw std::runtime_error("Missing test fragment");
    const auto tokens = highlight(line, state);
    std::size_t covered = 0;
    Syntax found = Syntax::Plain;
    for (const auto& token : tokens) {
        if (token.begin != covered || token.length == 0)
            throw std::runtime_error("Invalid token coverage");
        covered += token.length;
        if (position >= token.begin && position < covered)
            found = token.syntax;
    }
    if (covered != line.size())
        throw std::runtime_error("Incomplete highlighting");
    return found;
}

void expect(Syntax actual, Syntax expected)
{
    if (actual != expected)
        throw std::runtime_error("Unexpected syntax category");
}

void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check_prose()
{
    HighlightState code;
    const auto note =
        style_line("    // Regions first, so files sit on top.", TextKind::Code, code);
    check(note.prose && note.prose->begin == 4 && note.prose->body == 7,
          "A comment on its own line must be prose, without its marker");
    check(!style_line("int x = 1; // trailing", TextKind::Code, code).prose,
          "A comment after code must stay with the code");
    check(!style_line("// let debug = true;", TextKind::Code, code).prose,
          "Commented-out code must stay in the code face");
    check(!style_line("//   hash      turn", TextKind::Code, code).prose,
          "Columns lined up with spaces must keep their fixed width");
    const auto opened = style_line("/**", TextKind::Code, code);
    const auto inside = style_line(" * Draws the canvas.", TextKind::Code, code);
    const auto closed = style_line(" */", TextKind::Code, code);
    check(opened.prose && opened.prose->body == 3 && inside.prose && inside.prose->body == 3 &&
              closed.prose && closed.prose->body == 3 && !code.block_comment,
          "A block comment must be prose inside, and its bare delimiters must not be shown");
    check(!style_line("#include <vector>", TextKind::Code, code).prose,
          "A preprocessor line is not a comment");
    HighlightState script;
    const auto hash = style_line("# Lines keep their history.", TextKind::HashCode, script);
    check(hash.prose && hash.prose->body == 2 && hash.tokens.size() == 1 &&
              hash.tokens[0].syntax == Syntax::Comment,
          "Where a hash starts a comment, the comment must be prose");
    check(!style_line("#!/usr/bin/env python3", TextKind::HashCode, script).prose,
          "A shebang is not prose");
    HighlightState page;
    const auto heading = style_line("## Install", TextKind::Prose, page);
    check(heading.prose && heading.prose->heading && heading.prose->body == 3,
          "A document heading must be prose without its hashes");
    check(style_line("Plain words.", TextKind::Prose, page).prose.has_value() &&
              !style_line("| key | does |", TextKind::Prose, page).prose,
          "Document text must be prose and tables must keep their columns");
    style_line("```sh", TextKind::Prose, page);
    check(!style_line("make all", TextKind::Prose, page).prose && page.fenced,
          "Fenced code inside a document must stay code");
    style_line("```", TextKind::Prose, page);
    check(!page.fenced && text_kind("docs/notes.md") == TextKind::Prose &&
              text_kind("tools/run.py") == TextKind::HashCode &&
              text_kind("src/main.rs") == TextKind::Code,
          "The kind of text must follow from the file's name");
}

int main()
{
    try {
        HighlightState state;
        expect(kind("const int value = 42;", "const", state), Syntax::Keyword);
        expect(kind("const int value = 42;", "int", state), Syntax::Type);
        expect(kind("const int value = 42;", "42", state), Syntax::Number);
        expect(kind("draw(value);", "draw", state), Syntax::Function);
        expect(kind("#include <X11/Xlib.h>", "#include", state), Syntax::Directive);
        expect(kind("#include <X11/Xlib.h>", "X11", state), Syntax::String);
        expect(kind("1+2", "+", state), Syntax::Plain);
        expect(kind("1.5e-3 + 1'000", "1'000", state), Syntax::Number);
        expect(kind(R"test("escaped \" // string"; // comment)test", "// string", state),
               Syntax::String);
        expect(kind(R"test("escaped \" // string"; // comment)test", "// comment", state),
               Syntax::Comment);
        expect(kind("/* open comment", "open", state), Syntax::Comment);
        expect(kind("still comment */ int x;", "still", state), Syntax::Comment);
        expect(kind("int x;", "int", state), Syntax::Type);
        expect(kind("R\"tag(first line", "first", state), Syntax::String);
        expect(kind("// still string", "still", state), Syntax::String);
        expect(kind(")tag\"; return;", "return", state), Syntax::Keyword);
        if (!state.raw_string_end.empty() || state.block_comment)
            throw std::runtime_error("Leaked lexer state");
        check_prose();
        std::cout << "Syntax, escaped strings and multiline state checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
