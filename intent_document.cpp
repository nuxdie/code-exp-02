#include "intent_document.h"
#include <algorithm>
#include <fnmatch.h>

namespace {
using Json = nlohmann::json;
std::string excerpt(const std::string& source, std::size_t limit)
{
    auto length = std::min(source.size(), limit);
    while (length < source.size() && length > 0 &&
           (static_cast<unsigned char>(source[length]) & 0xc0) == 0x80)
        --length;
    return source.substr(0, length);
}
std::string rationale_text(const Json& record)
{
    const auto rationale = record.value("rationale", Json::object());
    const auto summary = rationale.value("summary", "");
    if (summary.empty())
        return "*No explanation recorded yet.*\n\n";
    const auto role = rationale.value("author_role", "agent");
    auto basis = rationale.value("basis", "explicit_record");
    std::replace(basis.begin(), basis.end(), '_', ' ');
    return "**Why**\n" + intent_literal(summary) + "\n\n*" + intent_literal(role) + " · " +
           intent_literal(basis) + "*\n\n";
}
std::string decision_text(const Json& record)
{
    std::string text = "**Request**\n" + intent_literal(record.value("statement", "")) + "\n\n";
    const auto origin = record.value("origin", Json::object());
    if (!origin.value("context", "").empty())
        text += "Context: " + intent_literal(origin.value("context", "")) + "\n\n";
    text += rationale_text(record);
    const auto artifacts = record.value("artifacts", Json::array());
    if (!artifacts.empty()) {
        text += "**Code**\n";
        for (const auto& artifact : artifacts) {
            const auto path = artifact.get<std::string>();
            text += path.find_first_of("*?[") == std::string::npos
                        ? "[" + intent_literal(path) + "](" + path + ")\n"
                        : intent_literal(path) + "\n";
        }
        text += "\n";
    }
    if (origin.value("kind", "") == "conversation")
        text += "[View conversation](chat:" + record.value("id", "") + ")\n\n";
    const auto binding = record.value("binding", Json::object());
    if (binding.contains("commit")) {
        const auto revision = binding.value("commit", "");
        text += "[Attributed commit " + revision.substr(0, 9) + "](commit:" + revision + ")\n\n";
    }
    if (binding.value("kind", "") == "reconstructed_milestone")
        text += "*Reconstructed milestone association; original per-change Git attribution was not "
                "recorded.*\n\n";
    for (const auto& relation : record.value("relations", Json::array()))
        text += "[" + relation.value("kind", "related decision") +
                "](intent:" + relation.value("decision_id", "") + ")\n\n";
    for (const auto& commit : record.value("commits", Json::array())) {
        const auto revision = commit.value("commit", "");
        text += "[" + revision.substr(0, 9) + " · " + intent_literal(commit.value("subject", "")) +
                "](commit:" + revision + ")";
        text += commit.value("reconstructed", false) ? " · reconstructed milestone\n\n" : "\n\n";
    }
    if (record.contains("id") && record.value("commits", Json::array()).empty() &&
        !binding.contains("commit"))
        text += "*No linked checkpoint yet.*\n\n";
    return text;
}
std::string evidence_text(const Json& evidence)
{
    std::string text = "";
    if (evidence.contains("line")) {
        const auto path = evidence.value("path", "");
        text += "[" + intent_literal(path) + ":" + std::to_string(evidence.value("line", 1)) +
                "](" + path + ":" + std::to_string(evidence.value("line", 1)) + ") · " +
                evidence.value("status", "") + "\n\n";
        if (evidence.value("decisions", Json::array()).empty())
            text += "*No recorded human attribution for this line. No intent is inferred.*\n\n";
    }
    if (evidence.contains("token")) {
        text += "**Review frozen diff** · Later working edits will remain uncommitted.\n\n";
        text += decision_text({{"statement", evidence.value("statement", "")},
                               {"rationale", evidence.value("rationale", Json::object())}});
    }
    if (evidence.contains("commit") && evidence["commit"].is_string()) {
        const auto revision = evidence["commit"].get<std::string>();
        text += "**Git commit** [" + revision.substr(0, 9) + "](commit:" + revision + ") · " +
                intent_literal(evidence.value("subject", evidence.value("summary", ""))) + "\n\n";
    }
    for (const auto& decision : evidence.value("decisions", Json::array()))
        text += decision_text(decision);
    for (const auto& change : evidence.value("changes", Json::array())) {
        const auto path = change.value("path", "");
        text += "[" + change.value("status", "") + " " + intent_literal(path) + "](" + path + ")\n";
    }
    const auto patch = evidence.value("patch", "");
    if (!patch.empty()) {
        text += "\n**Diff evidence**\n\n```cpp\n" + excerpt(patch, 80000) + "\n```\n";
        if (patch.size() > 80000)
            text += "\n*Diff display limited to 80,000 bytes; full patch remains in Git.*\n";
    }
    const auto evolution = evidence.value("evolution", "");
    if (!evolution.empty())
        text += "\n**Earlier changes to this line**\n\n```cpp\n" + excerpt(evolution, 40000) +
                "\n```\n";
    return text;
}
std::string overview_text(const Json& records, const std::string& path)
{
    std::string text;
    std::size_t count = 0;
    for (auto record = records.rbegin(); record != records.rend(); ++record) {
        if (!path.empty() && !intent_matches_path(*record, path))
            continue;
        ++count;
        const auto label = intent_summary(record->value("statement", ""));
        text += "[" + intent_literal(label) + "](intent:" + record->value("id", "") + ")\n";
        const auto date = record->value("recorded_at", "").substr(0, 10);
        const auto files = record->value("artifacts", Json::array()).size();
        const bool linked = !record->value("commits", Json::array()).empty() ||
                            record->value("binding", Json::object()).contains("commit");
        text += "*" + (date.empty() ? "" : date + " · ") + std::to_string(files) +
                (files == 1 ? " file" : " files") +
                (linked ? " · checkpoint linked" : " · captured") + "*\n\n";
    }
    if (!count)
        return path.empty() ? "No intents yet. Requests from your conversations will appear here."
                            : "No intents are linked to this file yet.";
    return "**" + std::to_string(count) + (count == 1 ? " intent" : " intents") +
           "** · newest first\n\n" + text;
}
} // namespace

std::string intent_summary(const std::string& statement, std::size_t limit)
{
    auto text = statement;
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return excerpt(text, limit) + (text.size() > limit ? "…" : "");
}

bool intent_matches_path(const Json& record, const std::string& path)
{
    const auto artifacts = record.find("artifacts");
    if (artifacts == record.end())
        return false;
    for (const auto& artifact : *artifacts)
        if (fnmatch(artifact.get_ref<const std::string&>().c_str(), path.c_str(), 0) == 0)
            return true;
    return false;
}

std::string intent_literal(const std::string& text)
{
    std::string escaped;
    for (const char byte : text) {
        if (std::string("&<>*_`[]$\\#~-+").find(byte) != std::string::npos)
            escaped += "&#" + std::to_string(static_cast<unsigned char>(byte)) + ";";
        else
            escaped += byte;
    }
    return escaped;
}

std::string intent_document(const Json& records, const std::string& selected, const Json& evidence,
                            bool overview, const std::string& path)
{
    if (overview)
        return overview_text(records, path);
    if (!evidence.empty())
        return evidence_text(evidence);
    for (const auto& record : records)
        if (record.value("id", "") == selected)
            return decision_text(record);
    return "No decisions recorded yet. Human messages are captured from the conversation; Git "
           "checkpoints bind reviewed changes to them.";
}
