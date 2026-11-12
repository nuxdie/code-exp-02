#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lsp {
using Json = nlohmann::json;

std::string file_uri(std::string_view path);
std::string uri_path(std::string_view uri);
std::size_t utf16_to_byte(std::string_view text, int column);
int byte_to_utf16(std::string_view text, std::size_t byte);
std::string frame(const Json& message);

class Framer {
  public:
    void append(std::string_view data);
    std::optional<Json> pop();

  private:
    std::string buffer;
};

struct SemanticToken {
    int line;
    std::size_t begin;
    std::size_t length;
    std::string type;
    unsigned modifiers;
};
std::vector<SemanticToken> semantic_tokens(const Json& data, const std::vector<std::string>& legend,
                                           const std::vector<std::string>& lines);
std::string hover_text(const Json& contents);

struct Document {
    std::string uri;
    std::string text;
};
struct ServerOptions {
    std::string language = "cpp";
    std::vector<std::string> arguments;
};

struct Message {
    std::string method{};
    std::string uri{};
    int id = 0;
    Json result{};
    std::string error{};
    int version = 0;
    std::string language = "cpp";
};

// All process IO runs on a worker. The UI polls event_fd alongside X11.
class LanguageServer {
  public:
    LanguageServer(std::string executable, std::string root, std::string build,
                   std::vector<Document> documents, ServerOptions options = {});
    ~LanguageServer();
    LanguageServer(const LanguageServer&) = delete;
    LanguageServer& operator=(const LanguageServer&) = delete;
    int event_fd() const;
    std::vector<Message> take_messages();
    int request(std::string method, std::string uri, Json params);
    void open(Document document);
    void change(Document document);
    void close(const std::string& uri);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace lsp
