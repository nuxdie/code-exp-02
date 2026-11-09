#include "lsp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace lsp {
std::string file_uri(const std::string& path)
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

std::string uri_path(const std::string& uri)
{
    if (uri.compare(0, 7, "file://") != 0)
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
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
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
        if (line < 0 || line >= static_cast<int>(lines.size()) || type < 0 ||
            type >= static_cast<int>(legend.size()))
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

struct LanguageServer::Impl {
    struct Request {
        int id;
        std::string method;
        std::string uri;
        Json params;
    };
    int results_fd = -1, commands_fd = -1, stream = -1;
    pid_t child = -1;
    std::atomic<bool> stopping{false};
    std::atomic<int> next_id{2};
    std::mutex mutex;
    std::vector<Request> commands;
    std::vector<Message> results;
    std::thread worker;
    std::string executable, root, build;
    std::vector<Document> documents;

    Impl(std::string executable, std::string root, std::string build,
         std::vector<Document> documents)
        : executable(std::move(executable)), root(std::move(root)), build(std::move(build)),
          documents(std::move(documents))
    {
        results_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        commands_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (results_fd < 0 || commands_fd < 0) {
            if (results_fd >= 0)
                close(results_fd);
            if (commands_fd >= 0)
                close(commands_fd);
            throw std::runtime_error("Could not create LSP wakeup descriptors");
        }
        worker = std::thread([this] {
            try {
                run();
            } catch (const std::exception& error) {
                publish({"error", "", 0, {}, error.what()});
            }
            cleanup();
        });
    }

    static void wake(int fd)
    {
        const std::uint64_t value = 1;
        (void)!write(fd, &value, sizeof(value));
    }
    static void drain(int fd)
    {
        std::uint64_t value;
        while (read(fd, &value, sizeof(value)) > 0) {
        }
    }

    void publish(Message message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(message));
        }
        wake(results_fd);
    }

    void enqueue(Request request)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            commands.push_back(std::move(request));
        }
        wake(commands_fd);
    }

    void spawn()
    {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0)
            throw std::runtime_error("Could not create clangd transport");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, sockets[0]);
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
        const auto log = build + "/clangd.log";
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        std::vector<std::string> arguments = {
            executable,           "--compile-commands-dir=" + build,
            "--background-index", "--header-insertion=never",
            "--log=error",        "-j=2"};
        std::vector<char*> argv;
        for (auto& argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        const int error =
            posix_spawnp(&child, executable.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (error) {
            close(sockets[0]);
            child = -1;
            throw std::runtime_error("Could not start clangd: " + std::string(strerror(error)));
        }
        stream = sockets[0];
        fcntl(stream, F_SETFL, fcntl(stream, F_GETFL) | O_NONBLOCK);
    }

    std::string outgoing;
    std::size_t written = 0;
    std::map<int, Request> pending;
    std::set<std::string> semantic_pending;
    bool ready = false;
    bool supports_semantics = false;
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
        send_message({{"id", request.id}, {"method", request.method}, {"params", request.params}});
        pending.emplace(request.id, std::move(request));
    }

    void request_semantics(const std::string& uri)
    {
        if (!supports_semantics || !semantic_pending.insert(uri).second)
            return;
        send_request({next_id++,
                      "textDocument/semanticTokens/full",
                      uri,
                      {{"textDocument", {{"uri", uri}}}}});
    }

    void open_document(const Document& document)
    {
        send_message({{"method", "textDocument/didOpen"},
                      {"params",
                       {{"textDocument",
                         {{"uri", document.uri},
                          {"languageId", "cpp"},
                          {"version", 1},
                          {"text", document.text}}}}}});
        request_semantics(document.uri);
        for (const char* method : {"textDocument/documentSymbol", "textDocument/foldingRange"})
            send_request(
                {next_id++, method, document.uri, {{"textDocument", {{"uri", document.uri}}}}});
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
            publish({method, uri, 0, params.value("diagnostics", Json::array()), {}});
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
                throw std::runtime_error("clangd initialization failed");
            const auto capabilities = value.at("result").at("capabilities");
            supports_semantics = capabilities.contains("semanticTokensProvider");
            ready = true;
            send_message({{"method", "initialized"}, {"params", Json::object()}});
            publish({"initialized", "", id, capabilities, {}});
            for (const auto& document : documents)
                open_document(document);
        } else if (auto found = pending.find(id); found != pending.end()) {
            const auto& request = found->second;
            if (request.method == "textDocument/semanticTokens/full")
                semantic_pending.erase(request.uri);
            publish({request.method, request.uri, id, value.value("result", Json()),
                     value.contains("error")
                         ? value["error"].value("message", "Language request failed")
                         : ""});
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
               {"initializationOptions", {{"fallbackFlags", {"-std=c++17"}}}}}}});
    }

    void collect_commands()
    {
        drain(commands_fd);
        std::vector<Request> incoming;
        {
            std::lock_guard<std::mutex> lock(mutex);
            incoming.swap(commands);
        }
        for (auto& command : incoming)
            deferred.push_back(std::move(command));
    }
    void dispatch_deferred()
    {
        if (!ready || stopping)
            return;
        for (auto& command : deferred) {
            if (command.method == "open") {
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
        const auto sent =
            send(stream, outgoing.data() + written, outgoing.size() - written, MSG_NOSIGNAL);
        if (sent > 0) {
            written += static_cast<std::size_t>(sent);
            if (written == outgoing.size()) {
                outgoing.clear();
                written = 0;
            }
        } else if (sent < 0 && errno != EAGAIN && errno != EINTR)
            throw std::runtime_error("clangd write failed");
    }
    bool read_messages()
    {
        char bytes[65536];
        const auto received = recv(stream, bytes, sizeof(bytes), 0);
        if (received == 0) {
            if (stopping)
                return false;
            throw std::runtime_error("clangd disconnected");
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return true;
            throw std::runtime_error("clangd read failed");
        }
        framer.append({bytes, static_cast<std::size_t>(received)});
        while (auto message = framer.pop())
            handle_rpc(*message);
        return true;
    }
    void run()
    {
        spawn();
        initialize_protocol();
        while (true) {
            if (stopping && !shutdown_id) {
                shutdown_id = next_id++;
                send_message({{"id", shutdown_id}, {"method", "shutdown"}, {"params", nullptr}});
                shutdown_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            }
            if ((exit_sent && outgoing.empty()) ||
                std::chrono::steady_clock::now() >= shutdown_deadline)
                break;
            pollfd fds[] = {
                {stream, static_cast<short>(POLLIN | (outgoing.empty() ? 0 : POLLOUT)), 0},
                {commands_fd, POLLIN, 0}};
            const int count = poll(fds, 2, stopping ? 10 : -1);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("clangd poll failed");
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

    void cleanup()
    {
        if (stream >= 0) {
            close(stream);
            stream = -1;
        }
        if (child > 0) {
            for (int i = 0; i < 30; ++i) {
                if (waitpid(child, nullptr, WNOHANG) != 0) {
                    child = -1;
                    break;
                }
                if (i == 10)
                    kill(child, SIGTERM);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (child > 0) {
                kill(child, SIGKILL);
                waitpid(child, nullptr, 0);
                child = -1;
            }
        }
    }

    ~Impl()
    {
        stopping = true;
        wake(commands_fd);
        worker.join();
        close(commands_fd);
        close(results_fd);
    }
};

LanguageServer::LanguageServer(std::string executable, std::string root, std::string build,
                               std::vector<Document> documents)
    : impl(std::make_unique<Impl>(std::move(executable), std::move(root), std::move(build),
                                  std::move(documents)))
{
}
LanguageServer::~LanguageServer() = default;
int LanguageServer::event_fd() const
{
    return impl->results_fd;
}

std::vector<Message> LanguageServer::take_messages()
{
    Impl::drain(impl->results_fd);
    std::vector<Message> messages;
    std::lock_guard<std::mutex> lock(impl->mutex);
    messages.swap(impl->results);
    return messages;
}

int LanguageServer::request(std::string method, std::string uri, Json params)
{
    const int id = impl->next_id++;
    impl->enqueue({id, std::move(method), std::move(uri), std::move(params)});
    return id;
}

void LanguageServer::open(Document document)
{
    impl->enqueue({0, "open", std::move(document.uri), std::move(document.text)});
}
} // namespace lsp
