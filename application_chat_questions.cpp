#include "application.h"

void Application::respond_chat_question(const std::string& text)
{
    if (!chat || chat->conversation.questions.empty() || text.empty())
        return;
    const auto& request = chat->conversation.questions.front();
    const auto prompts = request.params.value("questions", lsp::Json::array());
    if (chat->question_index >= prompts.size())
        return;
    chat->question_answers[prompts[chat->question_index].value("id", "")] = {
        {"answers", lsp::Json::array({text})}};
    chat->input.set("");
    ++chat->question_index;
    if (chat->question_index == prompts.size()) {
        chat->conversation.answer_questions(0, std::move(chat->question_answers));
        chat->question_answers = lsp::Json::object();
        chat->question_index = 0;
        chat->question_key.clear();
    }
    flush_agent();
    redraw = true;
    schedule_save();
}

void Application::draw_chat_questions()
{
    if (chat->conversation.questions.empty())
        return;
    const auto& request = chat->conversation.questions.front();
    if (chat->question_key != request.id.dump()) {
        chat->question_key = request.id.dump();
        chat->question_index = 0;
        chat->question_answers = lsp::Json::object();
    }
    const bool user_input = request.method == "item/tool/requestUserInput";
    std::string text = "**Agent needs your response**\n" + request.title;
    lsp::Json options = lsp::Json::array();
    if (user_input) {
        const auto prompts = request.params.value("questions", lsp::Json::array());
        if (chat->question_index < prompts.size()) {
            text = "**" + prompts[chat->question_index].value("question", "") + "**";
            options = prompts[chat->question_index].value("options", lsp::Json::array());
        }
    } else if (request.params.contains("command") && request.params["command"].is_string())
        text += "\n```sh\n" + request.params["command"].get<std::string>() + "\n```";
    auto& document = chat->messages["__question"];
    update_document(document, text, static_cast<int>(chat->body.size.x - 24));
    const double controls = user_input ? options.size() * 30.0 + 8 : 40;
    const Vec2 point{chat->body.origin.x,
                     chat->body.origin.y + chat->body.size.y - document.size.y - controls};
    const auto screen = camera.world_to_screen(point, viewport());
    renderer->rectangle(
        {screen, {chat->body.size.x * camera.zoom, (document.size.y + controls) * camera.zoom}},
        {0.98f, 0.97f, 0.93f, 1});
    renderer->draw(document.mesh, screen, camera.zoom);
    chat->hits.push_back({ChatHit::Kind::Text, {point, document.size}, 0, document.layout.get()});
    if (user_input) {
        for (std::size_t i = 0; i < options.size(); ++i) {
            auto& label = chat->messages["__option_" + std::to_string(i)];
            update_document(label, options[i].value("label", ""),
                            static_cast<int>(chat->body.size.x - 24), false);
            const CanvasRect bounds{{point.x + 12, point.y + document.size.y + i * 30},
                                    {chat->body.size.x - 24, 28}};
            renderer->draw(label.mesh, camera.world_to_screen(bounds.origin, viewport()),
                           camera.zoom);
            chat->hits.push_back({ChatHit::Kind::Option, bounds, i});
        }
    } else {
        const CanvasRect accept{{point.x + 12, point.y + document.size.y + 6}, {120, 28}};
        const CanvasRect decline{{point.x + 150, accept.origin.y}, {120, 28}};
        auto& labels = chat->messages["__answers"];
        update_document(labels, "Allow          Deny", 280, false);
        renderer->draw(labels.mesh, camera.world_to_screen(accept.origin, viewport()), camera.zoom);
        chat->hits.push_back({ChatHit::Kind::Accept, accept});
        chat->hits.push_back({ChatHit::Kind::Decline, decline});
    }
}
