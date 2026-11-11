#pragma once

#include "document_view.h"

#include <nlohmann/json.hpp>

#include <array>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A run of source lines introduced by one thing the human said.
struct IntentRun {
    int first = 0, last = 0; // zero-based, inclusive
    int turn = -1;
};

struct IntentTurn {
    std::string said, why, intent;
};

// What the intent store knows about the working tree, ready to draw.
struct IntentView {
    std::map<std::string, std::string> statements;
    std::vector<IntentTurn> turns;
    std::map<std::string, std::vector<IntentRun>> files;

    const IntentRun* run_at(const std::string& path, int line) const;
    const std::string& intent_of(const IntentRun& run) const;
};

IntentView parse_intent_view(const nlohmann::json& view);

// A stable colour per intent, expressed for the light theme.
std::array<float, 3> intent_color(const std::string& intent, double lightness);

// The sidenote for a run: the decision, and when asked, the words and reasoning behind it.
std::string intent_note(const IntentView& view, const IntentRun& run, bool detailed);

struct IntentMargin {
    IntentView view;
    std::future<std::string> loading;
    bool stale = false;
    std::string selected; // the intent lit up across the canvas
    std::string pinned_path, hover_path;
    std::optional<IntentRun> pinned;
    int pinned_line = 0, hover_line = 0, hover_first = -1; // the line a note sits beside
    DocumentView note;
    CanvasRect note_bounds{};
};
