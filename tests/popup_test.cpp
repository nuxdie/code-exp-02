#include "popup_markup.h"

#include <iostream>
#include <stdexcept>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void test_html()
{
    const auto inline_doc = popup_document(
        "Use <code class=\"cpp\">std::vector&lt;int&gt;</code> &amp; <strong>readability</strong>.",
        true);
    expect(inline_doc.text == "Use std::vector<int> & readability.",
           "HTML code tags and entities must render as content");
    expect(inline_doc.inline_code.size() == 1 &&
               inline_doc.text.substr(inline_doc.inline_code[0].first,
                                      inline_doc.inline_code[0].second) == "std::vector<int>",
           "HTML inline code must retain its exact highlighting range");
    expect(inline_doc.styles.size() == 1 && inline_doc.styles[0].style == PopupStyle::Strong,
           "HTML emphasis must become typography rather than exposed tags");
    const auto block = popup_document("<p>Example</p><pre><code class=\"language-cpp\">const int n "
                                      "= 42;\nreturn n &lt; 50;\n</code></pre>",
                                      true);
    expect(block.text.find("<code") == std::string::npos &&
               block.text.find("return n < 50;") != std::string::npos,
           "HTML code blocks must strip tags and decode C++ operators");
    bool found_code = false;
    for (bool code : block.code_lines)
        found_code = found_code || code;
    expect(found_code, "HTML preformatted examples must be classified as highlighted code");
    const auto literal =
        popup_document("```cpp\nconst char* tag = \"<code>\";\n```\n`<code>`", true);
    expect(literal.text.find("\"<code>\"") != std::string::npos &&
               literal.text.find("\n<code>") != std::string::npos,
           "Literal HTML inside Markdown code must remain visible");
}

void test_typography()
{
    const auto doc =
        popup_document("## Camera\nReadable **prose** and *emphasis* with `const int n = 2;`.\n- "
                       "[Details](https://example.com)\n```cpp\nreturn n;\n```",
                       true);
    expect(doc.text.find("##") == std::string::npos && doc.text.find("**") == std::string::npos &&
               doc.text.find("https://") == std::string::npos &&
               doc.text.find("• Details") != std::string::npos,
           "Markdown markers must become headings, emphasis and readable links");
    expect(doc.styles.size() == 4 && doc.inline_code.size() == 1 && doc.code_lines.back(),
           "Prose typography, inline code and code blocks must coexist");
    const auto plain = popup_document("<code>literal</code> **literal** $x$", false);
    expect(plain.text == "<code>literal</code> **literal** $x$" && plain.math.empty(),
           "Non-documentation popup text must retain literal content");
}

void test_math()
{
    const auto doc = popup_document("Inline $x^2$ and \\(\\frac{a}{b}\\).\n$$\n\\sum_{i=1}^{n} "
                                    "i\n$$\n```cpp\nconst char* price = \"$5$\";\n```",
                                    true);
    expect(doc.math.size() == 3 && !doc.math[0].display && doc.math[2].display &&
               doc.math[1].expression == "\\frac{a}{b}",
           "Inline and multiline display LaTeX must preserve source expressions and modes");
    for (const auto& math : doc.math)
        expect(doc.text.substr(math.begin, 3) == "\xef\xbf\xbc",
               "Math must reserve an inline layout object");
    expect(doc.text.find("$5$") != std::string::npos,
           "Dollar signs inside code must not become equations");
}

void test_clangd_escaped_documentation()
{
    const std::string returned = R"CLANGD(### struct `Camera`  

---
Size: 24 bytes, alignment 8 bytes  
@brief A camera keeps world coordinates independent of window pixels.  
\### A stable coordinate system  
Read the canvas in \*\*world space\*\*; use screen space only for interaction.  
The viewport center is \\(v\\), the camera center is \\(c\\), and zoom is \\(z\\).  
\\[s = v + z(w-c), \\qquad w = c + \\frac{s-v}{z}\\]  
Panning and zooming preserve the world point beneath the pointer:  
\`\`\`cpp camera.pan({12, 0});  
camera.zoom_at(1.15, pointer, viewport);  
\`\`\`  
\<code>screen_to_world\</code> and \<code>world_to_screen\</code> are inverses.  

---
```cpp
struct Camera {}
```)CLANGD";
    const auto doc = popup_document(hover_documentation(returned), true);
    expect(doc.text.find("@brief") == std::string::npos &&
               doc.text.find("\\*") == std::string::npos &&
               doc.text.find("<code>") == std::string::npos &&
               doc.text.find("camera.pan({12, 0});") != std::string::npos,
           "Escaped clangd prose must recover headings, code examples and HTML without visible "
           "escapes");
    expect(doc.math.size() == 4 && doc.math[0].expression == "v" && doc.math.back().display,
           "Clangd's doubled TeX backslashes must become valid original equations");
    int code_lines = 0;
    for (bool code : doc.code_lines)
        code_lines += code;
    expect(code_lines == 3 && doc.styles.size() >= 3,
           "Recovered examples and the server signature must all retain code and prose styling");
}

int main()
{
    try {
        test_html();
        test_typography();
        test_math();
        test_clangd_escaped_documentation();
        std::cout << "Documentation markup, typography and LaTeX parsing checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
