#pragma once

#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct ConversationEntry {
    std::string id, turn, role, text, detail;
    int sequence = 0;
    unsigned revision = 0;
    bool activity = false;
    bool expanded = false;
};
struct AgentQuestion {
    nlohmann::json id;
    std::string method, title;
    nlohmann::json params;
};

class Conversation {
  public:
    using Json = nlohmann::json;
    std::string thread_id, active_turn, title = "Conversation", status = "Connecting…", error;
    std::string accepted_text, canvas_context;
    bool fresh = false;
    std::vector<ConversationEntry> entries;
    std::vector<Json> threads;
    std::vector<AgentQuestion> questions;
    bool connected = false, ready = false, sending = false, history_loading = false;
    unsigned revision = 0;

    explicit Conversation(std::string root);
    void receive(const Json& message);
    std::vector<Json> outgoing();
    bool submit(const std::string& text);
    void interrupt();
    void select_thread(const std::string& id);
    void list_threads();
    void answer_questions(std::size_t index, Json answers);
    void answer(std::size_t index, bool accept, const std::string& text = "");

  private:
    std::string root, submitted;
    std::string saved_name, title_thread, title_target, title_result, title_turn, model_name;
    bool title_attempted = false;
    std::map<int, std::string> title_requests;
    std::chrono::steady_clock::time_point title_deadline;
    void start_title(const std::string& prompt);
    void cancel_title();
    bool title_message(const Json& message);
    void title_response(const std::string& stage, const Json& message);
    void start_title_thread(const Json& config);
    void title_notification(const Json& message);
    void title_request(const std::string& stage, const std::string& method, Json params);
    int next_id = 1;
    std::map<int, std::string> pending;
    std::map<std::string, int> sequences;
    std::vector<Json> queued;
    int request(const std::string& method, Json params);
    void response(const Json& message);
    void request_failed(const std::string& method, const Json& failure);
    void thread_ready(const std::string& method, const Json& result);
    void notification(const std::string& method, const Json& params);
    void receive_item(const std::string& method, const Json& params);
    void history_page(const Json& result);
    void put_item(const std::string& turn, const Json& item, int sequence = -1);
    void delta(const Json& params, bool output);
    void hydrate_thread(const Json& thread);
    void send_prompt();
};
