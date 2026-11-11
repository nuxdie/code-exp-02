#include "intent_margin.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

const IntentRun* IntentView::run_at(const std::string& path, int line) const
{
    const auto found = files.find(path);
    if (found == files.end())
        return nullptr;
    for (const auto& run : found->second)
        if (line >= run.first && line <= run.last)
            return &run;
    return nullptr;
}

const std::string& IntentView::intent_of(const IntentRun& run) const
{
    static const std::string none;
    return run.turn >= 0 && run.turn < static_cast<int>(turns.size()) ? turns[run.turn].intent
                                                                      : none;
}

IntentView parse_intent_view(const nlohmann::json& view)
{
    IntentView result;
    if (!view.is_object())
        return result;
    for (const auto& intent : view.value("intents", nlohmann::json::array()))
        result.statements[intent.value("id", "")] = intent.value("statement", "");
    for (const auto& turn : view.value("turns", nlohmann::json::array())) {
        const auto intent = turn.find("intent");
        result.turns.push_back(
            {turn.value("said", ""), turn.value("why", ""),
             intent != turn.end() && intent->is_string() ? intent->get<std::string>() : ""});
    }
    const auto files = view.value("files", nlohmann::json::object());
    for (auto file = files.begin(); file != files.end(); ++file)
        for (const auto& run : file.value())
            if (run.is_array() && run.size() == 3)
                result.files[file.key()].push_back(
                    {run[0].get<int>() - 1, run[1].get<int>() - 1, run[2].get<int>()});
    return result;
}

std::array<float, 3> intent_color(const std::string& intent, double lightness)
{
    // Golden-angle steps keep neighbouring intents apart on the colour wheel.
    const double hue =
        std::fmod(static_cast<double>(std::hash<std::string>{}(intent) % 997) * 137.508, 360.0);
    const double chroma = (1 - std::abs(2 * lightness - 1)) * 0.62;
    const double section = hue / 60;
    const double second = chroma * (1 - std::abs(std::fmod(section, 2.0) - 1));
    const double base = lightness - chroma / 2;
    const std::array<std::array<double, 3>, 6> wheel{{{chroma, second, 0},
                                                      {second, chroma, 0},
                                                      {0, chroma, second},
                                                      {0, second, chroma},
                                                      {second, 0, chroma},
                                                      {chroma, 0, second}}};
    const auto& color = wheel[static_cast<std::size_t>(section) % 6];
    return {static_cast<float>(color[0] + base), static_cast<float>(color[1] + base),
            static_cast<float>(color[2] + base)};
}

namespace {
std::string clipped(std::string text, std::size_t limit)
{
    if (text.size() <= limit)
        return text;
    auto end = text.rfind(' ', limit);
    if (end == std::string::npos || end < limit / 2)
        end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    return text.substr(0, end) + "…";
}
} // namespace

std::string intent_note(const IntentView& view, const IntentRun& run, bool detailed)
{
    if (run.turn < 0 || run.turn >= static_cast<int>(view.turns.size()))
        return "";
    const auto& turn = view.turns[run.turn];
    const auto statement = view.statements.find(turn.intent);
    const bool distilled = statement != view.statements.end() && !statement->second.empty();
    if (!detailed)
        return distilled ? statement->second : clipped(turn.said, 160);
    std::string note = distilled ? "**" + statement->second + "**\n\n" : "";
    std::istringstream said(clipped(turn.said, 600));
    for (std::string line; std::getline(said, line);)
        note += line.empty() ? "\n" : "*" + popup_literal(line) + "*\n";
    if (!turn.why.empty())
        note += "\n" + clipped(turn.why, 900);
    return note;
}

std::string intent_instructions(const IntentView& view)
{
    if (view.statements.empty())
        return "";
    std::string text = "The user has already made these decisions about this project. Follow "
                       "them without being asked again, and say so if a request conflicts with "
                       "one.\n";
    for (const auto& [id, statement] : view.statements)
        text += "- " + id + ": " + statement + "\n";
    return text + "When a turn changes files, call canvas_intent once with the id it served.";
}
