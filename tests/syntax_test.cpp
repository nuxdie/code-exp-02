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
        std::cout << "Syntax, escaped strings and multiline state checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
