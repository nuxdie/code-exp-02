#include "lsp.h"

#include "process.h"
#include "wakeup.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace lsp {
std::string file_uri(std::string_view path)
{
    const auto absolute = std::filesystem::absolute(path).lexically_normal().string();
    std::string uri = "file://";
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : absolute) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            uri += c;
        else {
            uri += '%';
            uri += hex[c >> 4];
            uri += hex[c & 15];
        }
    }
    return uri;
}

std::string uri_path(std::string_view uri)
{
    if (!uri.starts_with("file://"))
        return {};
    std::string path;
    for (std::size_t i = 7; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            unsigned byte = 0;
            const auto parsed = std::from_chars(uri.data() + i + 1, uri.data() + i + 3, byte, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != uri.data() + i + 3 || byte == 0)
                return {};
            path += static_cast<char>(byte);
            i += 2;
        } else
            path += uri[i];
    }
    return !path.empty() && path[0] == '/' ? path : std::string{};
}

static std::size_t character_bytes(unsigned char c)
{
    return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
}

std::size_t utf16_to_byte(std::string_view text, int column)
{
    std::size_t byte = 0;
    int units = 0;
    while (byte < text.size() && units < column) {
        const auto length = std::min(character_bytes(text[byte]), text.size() - byte);
        const int count = length == 4 ? 2 : 1;
        if (units + count > column)
            break;
        units += count;
        byte += length;
    }
    return byte;
}

int byte_to_utf16(std::string_view text, std::size_t byte)
{
    int units = 0;
    const auto end = std::min(byte, text.size());
    for (std::size_t i = 0; i < end;) {
        const auto length = std::min(character_bytes(text[i]), text.size() - i);
        if (i + length > end)
            break;
        units += length == 4 ? 2 : 1;
        i += length;
    }
    return units;
}

std::string frame(const Json& message)
{
    const auto body = message.dump();
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

void Framer::append(std::string_view data)
{
    if (buffer.size() + data.size() > 32 * 1024 * 1024)
        throw std::runtime_error("LSP message too large");
    buffer.append(data);
}

std::optional<Json> Framer::pop()
{
    const auto end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buffer.size() > 8192)
            throw std::runtime_error("Invalid LSP header");
        return std::nullopt;
    }
    const auto field = buffer.find("Content-Length: ");
    if (field == std::string::npos || field > end)
        throw std::runtime_error("Missing LSP content length");
    std::size_t length = 0;
    const auto start = field + 16;
    const auto value_end = std::min(end, buffer.find("\r\n", start));
    const auto parsed = std::from_chars(buffer.data() + start, buffer.data() + value_end, length);
    if (parsed.ec != std::errc{} || parsed.ptr != buffer.data() + value_end ||
        length > 32 * 1024 * 1024)
        throw std::runtime_error("Invalid LSP content length");
    if (buffer.size() < end + 4 + length)
        return std::nullopt;
    auto message = Json::parse(buffer.begin() + end + 4, buffer.begin() + end + 4 + length);
    buffer.erase(0, end + 4 + length);
    return message;
}

std::vector<SemanticToken> semantic_tokens(const Json& data, const std::vector<std::string>& legend,
                                           const std::vector<std::string>& lines)
{
    std::vector<SemanticToken> result;
    if (!data.is_array() || data.size() % 5)
        return result;
    int line = 0, column = 0;
    for (std::size_t i = 0; i < data.size(); i += 5) {
        const int delta = data[i].get<int>();
        const int offset = data[i + 1].get<int>();
        const int length = data[i + 2].get<int>();
        const int type = data[i + 3].get<int>();
        if (delta < 0 || offset < 0 || length < 0)
            return {};
        line += delta;
        column = delta ? offset : column + offset;
        if (line < 0 || line >= std::ssize(lines) || type < 0 || type >= std::ssize(legend))
            continue;
        const auto begin = utf16_to_byte(lines[line], column);
        const auto end = utf16_to_byte(lines[line], column + length);
        if (end > begin)
            result.push_back({line, begin, end - begin, legend[type], data[i + 4].get<unsigned>()});
    }
    return result;
}

std::string hover_text(const Json& contents)
{
    if (contents.is_string())
        return contents.get<std::string>();
    if (contents.is_object()) {
        const auto value = contents.value("value", "");
        if (contents.contains("language"))
            return "```" + contents.value("language", "") + "\n" + value + "\n```";
        return value;
    }
    std::string text;
    if (contents.is_array())
        for (const auto& item : contents) {
            if (!text.empty())
                text += "\n\n";
            text += hover_text(item);
        }
    return text;
}

namespace {
std::atomic<int> next_request_id{2};
}

struct LanguageServer::Impl {
    struct Request {
        int id = 0;
        std::string method{};
        std::string uri{};
        Json params{};
        int version = 0;
    };
    Mailbox<Message> results;
    Mailbox<Request> commands;
    ServerOptions options;
    std::string executable, root, build;
    std::vector<Document> documents;
    std::stop_token stopping;
    ChildProcess server;

    Impl(std::string executable, std::string root, std::string build,
         std::vector<Document> documents, ServerOptions options)
        : options(std::move(options)), executable(std::move(executable)), root(std::move(root)),
          build(std::move(build)), documents(std::move(documents))
    {
    }

    void publish(Message message)
    {
        message.language = options.language;
        results.post(std::move(message));
    }

    void spawn()
    {
        std::vector<std::string> arguments{executable};
        if (options.language == "cpp")
            arguments.insert(arguments.end(),
                             {"--compile-commands-dir=" + build, "--background-index",
                              "--header-insertion=never", "--log=error", "-j=2"});
        arguments.insert(arguments.end(), options.arguments.begin(), options.arguments.end());
        server = ChildProcess(arguments, "language server",
                              build + "/" + options.language + "-language-server.log");
    }

    std::string outgoing;
    std::size_t written = 0;
    std::map<int, Request> pending;
    std::set<std::string> semantic_pending;
    std::map<std::string, int> versions;
    bool ready = false;
    bool supports_semantics = false;
    Json capabilities = Json::object();
    Framer framer;
    std::vector<Request> deferred;
    int shutdown_id = 0;
    bool exit_sent = false;
    std::chrono::steady_clock::time_point shutdown_deadline =
        std::chrono::steady_clock::time_point::max();

    void send_message(Json message)
    {
        message["jsonrpc"] = "2.0";
        outgoing += frame(message);
    }

    void send_request(Request request)
    {
        request.version = versions[request.uri];
        send_message({{"id", request.id}, {"method", request.method}, {"params", request.params}});
        pending.emplace(request.id, std::move(request));
    }

    void request_semantics(const std::string& uri)
    {
        if (!supports_semantics || !semantic_pending.insert(uri).second)
            return;
        send_request({next_request_id++,
                      "textDocument/semanticTokens/full",
                      uri,
                      {{"textDocument", {{"uri", uri}}}}});
    }

    void refresh_structure(const std::string& uri)
    {
        request_semantics(uri);
        for (const auto& feature :
             {std::pair{"documentSymbolProvider", "textDocument/documentSymbol"},
              std::pair{"foldingRangeProvider", "textDocument/foldingRange"},
              std::pair{"documentLinkProvider", "textDocument/documentLink"},
              std::pair{"diagnosticProvider", "textDocument/diagnostic"}}) {
            const auto supported = capabilities.value(feature.first, Json(false));
            if (supported.is_object() || (supported.is_boolean() && supported.get<bool>()))
                send_request(
                    {next_request_id++, feature.second, uri, {{"textDocument", {{"uri", uri}}}}});
        }
    }

    void open_document(const Document& document)
    {
        versions[document.uri] = 1;
        send_message({{"method", "textDocument/didOpen"},
                      {"params",
                       {{"textDocument",
                         {{"uri", document.uri},
                          {"languageId", options.language},
                          {"version", 1},
                          {"text", document.text}}}}}});
        refresh_structure(document.uri);
    }
    void change_document(const Document& document)
    {
        const int version = ++versions[document.uri];
        send_message({{"method", "textDocument/didChange"},
                      {"params",
                       {{"textDocument", {{"uri", document.uri}, {"version", version}}},
                        {"contentChanges", Json::array({{{"text", document.text}}})}}}});
        for (auto& existing : documents)
            if (existing.uri == document.uri)
                existing.text = document.text;
        // An older semantic request may still be in flight. Its result is versioned below.
        semantic_pending.erase(document.uri);
        refresh_structure(document.uri);
    }
    void close_document(const std::string& uri)
    {
        send_message(
            {{"method", "textDocument/didClose"}, {"params", {{"textDocument", {{"uri", uri}}}}}});
        std::erase_if(documents, [&](const auto& document) { return document.uri == uri; });
        versions.erase(uri);
        semantic_pending.erase(uri);
    }

    void notification(const Json& value)
    {
        const auto method = value["method"].get<std::string>();
        const auto params = value.value("params", Json::object());
        if (value.contains("id")) {
            send_message(
                {{"id", value["id"]},
                 {"result", method == "workspace/configuration" ? Json::array() : Json(nullptr)}});
            if (method == "workspace/semanticTokens/refresh")
                for (const auto& document : documents)
                    request_semantics(document.uri);
        } else if (method == "textDocument/publishDiagnostics") {
            const auto uri = params.value("uri", "");
            publish({.method = method,
                     .uri = uri,
                     .result = params.value("diagnostics", Json::array()),
                     .version = params.value("version", 0)});
            request_semantics(uri);
        }
    }

    void response(const Json& value)
    {
        const int id = value["id"].get<int>();
        if (id == shutdown_id) {
            send_message({{"method", "exit"}});
            exit_sent = true;
        } else if (id == 1) {
            if (value.contains("error"))
                throw std::runtime_error("Language server initialization failed");
            capabilities = value.at("result").at("capabilities");
            supports_semantics = capabilities.contains("semanticTokensProvider");
            ready = true;
            send_message({{"method", "initialized"}, {"params", Json::object()}});
            publish({.method = "initialized", .id = id, .result = capabilities});
            for (const auto& document : documents)
                open_document(document);
        } else if (auto found = pending.find(id); found != pending.end()) {
            const auto& request = found->second;
            if (request.method == "textDocument/semanticTokens/full")
                semantic_pending.erase(request.uri);
            publish({.method = request.method,
                     .uri = request.uri,
                     .id = id,
                     .result = value.value("result", Json()),
                     .error = value.contains("error")
                                  ? value["error"].value("message", "Language request failed")
                                  : "",
                     .version = request.version});
            pending.erase(found);
        }
    }

    void handle_rpc(const Json& value)
    {
        if (value.contains("method"))
            notification(value);
        else if (value.contains("id") && value["id"].is_number_integer())
            response(value);
    }

    void initialize_protocol()
    {
        send_message(
            {{"id", 1},
             {"method", "initialize"},
             {"params",
              {{"processId", getpid()},
               {"rootUri", file_uri(root)},
               {"clientInfo", {{"name", "Source Canvas"}, {"version", "0.1"}}},
               {"capabilities",
                {{"general", {{"positionEncodings", {"utf-16"}}}},
                 {"textDocument",
                  {{"hover", {{"contentFormat", {"markdown", "plaintext"}}}},
                   {"documentSymbol", {{"hierarchicalDocumentSymbolSupport", true}}},
                   {"foldingRange", {{"lineFoldingOnly", true}}},
                   {"definition", {{"linkSupport", true}}},
                   {"completion", {{"completionItem", {{"snippetSupport", false}}}}},
                   {"semanticTokens",
                    {{"requests", {{"full", true}}},
                     {"formats", {"relative"}},
                     {"tokenTypes",
                      {"namespace",  "type",          "class",     "enum",     "interface",
                       "struct",     "typeParameter", "parameter", "variable", "property",
                       "enumMember", "function",      "method",    "macro",    "keyword",
                       "modifier",   "comment",       "string",    "number",   "operator"}},
                     {"tokenModifiers",
                      {"declaration", "definition", "readonly", "static", "deprecated", "abstract",
                       "async", "modification", "documentation", "defaultLibrary"}},
                     {"overlappingTokenSupport", false},
                     {"multilineTokenSupport", false}}}}}}},
               {"initializationOptions", options.language == "cpp"
                                             ? Json{{"fallbackFlags", {"-std=c++17"}}}
                                             : Json::object()}}}});
    }

    void collect_commands() { std::ranges::move(commands.take(), std::back_inserter(deferred)); }
    void dispatch_deferred()
    {
        if (!ready || stopping.stop_requested())
            return;
        for (auto& command : deferred) {
            if (command.method == "change") {
                change_document({command.uri, command.params.get<std::string>()});
            } else if (command.method == "close") {
                close_document(command.uri);
            } else if (command.method == "open") {
                Document document{command.uri, command.params.get<std::string>()};
                documents.push_back(document);
                open_document(document);
            } else
                send_request(std::move(command));
        }
        deferred.clear();
    }
    void flush_output()
    {
        if (outgoing.empty())
            return;
        const auto sent = send(server.stream(), outgoing.data() + written,
                               outgoing.size() - written, MSG_NOSIGNAL);
        if (sent > 0) {
            written += static_cast<std::size_t>(sent);
            if (written == outgoing.size()) {
                outgoing.clear();
                written = 0;
            }
        } else if (sent < 0 && errno != EAGAIN && errno != EINTR)
            throw std::runtime_error("Language server write failed");
    }
    bool read_messages()
    {
        std::array<char, 65536> bytes;
        const auto received = recv(server.stream(), bytes.data(), bytes.size(), 0);
        if (received == 0) {
            if (stopping.stop_requested())
                return false;
            throw std::runtime_error("Language server disconnected");
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return true;
            throw std::runtime_error("Language server read failed");
        }
        framer.append({bytes.data(), static_cast<std::size_t>(received)});
        while (auto message = framer.pop())
            handle_rpc(*message);
        return true;
    }
    void run()
    {
        spawn();
        initialize_protocol();
        while (true) {
            const bool leaving = stopping.stop_requested();
            if (leaving && !shutdown_id) {
                shutdown_id = next_request_id++;
                send_message({{"id", shutdown_id}, {"method", "shutdown"}, {"params", nullptr}});
                shutdown_deadline = std::chrono::steady_clock::now() + 300ms;
            }
            if ((exit_sent && outgoing.empty()) ||
                std::chrono::steady_clock::now() >= shutdown_deadline)
                break;
            std::array<pollfd, 2> fds{
                {{server.stream(), static_cast<short>(POLLIN | (outgoing.empty() ? 0 : POLLOUT)),
                  0},
                 {commands.fd(), POLLIN, 0}}};
            const int count = poll(fds.data(), fds.size(), leaving ? 10 : -1);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("Language server poll failed");
            }
            if (fds[1].revents & POLLIN)
                collect_commands();
            dispatch_deferred();
            if (fds[0].revents & POLLOUT)
                flush_output();
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) && !read_messages())
                break;
        }
    }

    // Last, so the worker stops before anything it uses goes away.
    std::jthread worker{[this](std::stop_token token) {
        stopping = std::move(token);
        const std::stop_callback wake(stopping, [this] { commands.wake(); });
        try {
            run();
        } catch (const std::exception& error) {
            publish({.method = "error", .error = error.what()});
        }
        server.stop(100ms);
    }};
};

LanguageServer::LanguageServer(std::string executable, std::string root, std::string build,
                               std::vector<Document> documents, ServerOptions options)
    : impl(std::make_unique<Impl>(std::move(executable), std::move(root), std::move(build),
                                  std::move(documents), std::move(options)))
{
}
LanguageServer::~LanguageServer() = default;
int LanguageServer::event_fd() const
{
    return impl->results.fd();
}

std::vector<Message> LanguageServer::take_messages()
{
    return impl->results.take();
}

int LanguageServer::request(std::string method, std::string uri, Json params)
{
    const int id = next_request_id++;
    impl->commands.post({.id = id,
                         .method = std::move(method),
                         .uri = std::move(uri),
                         .params = std::move(params)});
    return id;
}

void LanguageServer::open(Document document)
{
    impl->commands.post(
        {.method = "open", .uri = std::move(document.uri), .params = std::move(document.text)});
}
} // namespace lsp

namespace lsp {
void LanguageServer::change(Document document)
{
    impl->commands.post(
        {.method = "change", .uri = std::move(document.uri), .params = std::move(document.text)});
}
void LanguageServer::close(const std::string& uri)
{
    impl->commands.post({.method = "close", .uri = uri});
}
} // namespace lsp
