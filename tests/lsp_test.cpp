#include "lsp.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using namespace lsp;
void test_protocol(const std::filesystem::path& root)
{
    const Json message = {{"jsonrpc", "2.0"}, {"id", 4}, {"result", "héllo 🌍"}};
    const auto encoded = frame(message);
    Framer framer;
    for (std::size_t i = 0; i + 1 < encoded.size(); ++i) {
        framer.append(std::string_view(encoded).substr(i, 1));
        expect(!framer.pop(), "Partial LSP frames must wait for the complete body");
    }
    framer.append(std::string_view(encoded).substr(encoded.size() - 1));
    framer.append(encoded);
    expect(framer.pop().value() == message && framer.pop().value() == message && !framer.pop(),
           "Fragmented and concatenated frames must decode exactly");
    bool rejected = false;
    try {
        Framer bad;
        bad.append("Content-Length: invalid\r\n\r\n");
        bad.pop();
    } catch (const std::exception&) {
        rejected = true;
    }
    expect(rejected, "Invalid framing must fail cleanly");
    const std::string unicode = "aé🌍z";
    expect(utf16_to_byte(unicode, 4) == 7 && byte_to_utf16(unicode, 7) == 4,
           "UTF-16 positions must handle multibyte characters and surrogate pairs");
    expect(utf16_to_byte(unicode, 3) == 3 && utf16_to_byte(unicode, 100) == unicode.size(),
           "Positions must not split a surrogate or run past a line");
    const auto uri = file_uri((root / "a space #é.cpp").string());
    expect(uri_path(uri) == (root / "a space #é.cpp").string(), "File URIs must round trip");
    const auto decoded = semantic_tokens(Json::array({0, 2, 2, 0, 1, 1, 0, 3, 1, 0}),
                                         {"type", "function"}, {unicode, "foo()"});
    expect(decoded.size() == 2 && decoded[0].begin == 3 && decoded[0].length == 4 &&
               decoded[1].line == 1 && decoded[1].begin == 0,
           "Semantic token deltas and UTF-16 lengths must map to the correct UTF-8 bytes");
}
struct FeatureProbe {
    std::string main_uri, header_uri, bad_uri;
    bool initialized = false, semantic = false, diagnostic = false, hover = false,
         definition = false;
    bool completion = false, references = false, folds = false, symbols = false;
    bool complete() const
    {
        return initialized && semantic && diagnostic && hover && definition && completion &&
               references && folds && symbols;
    }
    void consume(const Message& event)
    {
        expect(event.error.empty(), event.error.c_str());
        if (event.method == "initialized")
            initialized = event.result.contains("semanticTokensProvider");
        if (event.method == "textDocument/semanticTokens/full" && event.uri == main_uri)
            semantic = !event.result.value("data", Json::array()).empty();
        if (event.method == "textDocument/publishDiagnostics" && event.uri == bad_uri)
            diagnostic = !event.result.empty();
        if (event.method == "textDocument/hover") {
            const auto contents = event.result.value("contents", Json());
            const auto text = hover_text(contents);
            hover = contents.value("kind", "") == "markdown" &&
                    text.find("increment") != std::string::npos &&
                    text.find("```cpp") != std::string::npos;
        }
        if (event.method == "textDocument/definition") {
            const auto location = event.result.is_array() ? event.result.at(0) : event.result;
            definition = location.value("uri", location.value("targetUri", "")) == header_uri;
        }
        if (event.method == "textDocument/completion")
            completion = !(event.result.is_array() ? event.result
                                                   : event.result.value("items", Json::array()))
                              .empty();
        if (event.method == "textDocument/references")
            references = !event.result.empty();
        if (event.method == "textDocument/foldingRange" && event.uri == header_uri)
            folds = !event.result.empty();
        if (event.method == "textDocument/documentSymbol" && event.uri == header_uri)
            for (const auto& symbol : event.result)
                if (symbol.value("name", "") == "Toolbox")
                    symbols = symbol.value("children", Json::array()).size() >= 2;
    }
};
void request_features(LanguageServer& server, const std::string& uri)
{
    Json position = {{"textDocument", {{"uri", uri}}},
                     {"position", {{"line", 1}, {"character", 22}}}};
    server.request("textDocument/hover", uri, position);
    server.request("textDocument/definition", uri, position);
    position["context"] = {{"includeDeclaration", true}};
    server.request("textDocument/references", uri, position);
    position.erase("context");
    position["position"]["character"] = 20;
    server.request("textDocument/completion", uri, position);
}
void test_live_server(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    const auto main_uri = file_uri((root / "main.cpp").string());
    const auto header_uri = file_uri((root / "api.h").string());
    const auto bad_uri = file_uri((root / "bad.cpp").string());
    const std::string header = "inline int increment(int value) {\n return value + 1;\n}\nstruct "
                               "Toolbox {\n int value;\n int read() const { return value; }\n};\n";
    const std::string main = "#include \"api.h\"\nint main() { return increment(41); }\n";
    const std::string bad = "int invalid = missing_symbol;\n";
    std::ofstream(root / "api.h") << header;
    std::ofstream(root / "main.cpp") << main;
    std::ofstream(root / "bad.cpp") << bad;
    Json database = Json::array();
    for (const char* name : {"main.cpp", "bad.cpp"})
        database.push_back(
            {{"directory", root.string()},
             {"file", (root / name).string()},
             {"arguments",
              {"clang++", "-std=c++17", "-I" + root.string(), "-c", (root / name).string()}}});
    std::ofstream(root / "compile_commands.json") << database.dump();
    LanguageServer server(TEST_CLANGD, root.string(), root.string(),
                          {{main_uri, main}, {header_uri, header}, {bad_uri, bad}});
    FeatureProbe probe{main_uri, header_uri, bad_uri};
    bool requested = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline && !probe.complete()) {
        pollfd fd{server.event_fd(), POLLIN, 0};
        poll(&fd, 1, 100);
        for (const auto& event : server.take_messages())
            probe.consume(event);
        if (probe.initialized && probe.semantic && !requested) {
            request_features(server, main_uri);
            requested = true;
        }
    }
    expect(probe.complete(),
           "Live clangd must provide semantic tokens, diagnostics, hover, definitions, "
           "completions, references, folding and hierarchical symbols");
}
void test_missing_server(const std::filesystem::path& root)
{
    LanguageServer missing("/nonexistent/clangd", root.string(), root.string(), {});
    pollfd fd{missing.event_fd(), POLLIN, 0};
    expect(poll(&fd, 1, 2000) > 0, "Unavailable clangd must report failure asynchronously");
    const auto events = missing.take_messages();
    expect(!events.empty() && events[0].method == "error",
           "Failure must preserve fallback availability");
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("source-canvas-lsp-test-" + std::to_string(getpid()));
    try {
        test_protocol(root);
        test_live_server(root);
        test_missing_server(root);
        std::filesystem::remove_all(root);
        std::cout << "LSP protocol, Unicode and live clangd feature checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
