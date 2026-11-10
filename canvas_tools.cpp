#include "canvas_tools.h"

nlohmann::json canvas_tools()
{
    using Json = nlohmann::json;
    const Json text{{"type", "string"}, {"maxLength", 2000}};
    const Json coordinate{{"type", "number"}, {"minimum", 0}, {"maximum", 100000}};
    const Json line{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000000}};
    const Json step{
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"id", "path", "start_line", "end_line", "title", "note"}},
        {"properties",
         {{"id", text},
          {"path", {{"type", "string"}, {"description", "Exact workspace-relative source path"}}},
          {"start_line", line},
          {"end_line", line},
          {"title", text},
          {"note", text},
          {"x", coordinate},
          {"y", coordinate}}}};
    const Json edge{{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"from", "to", "label"}},
                    {"properties", {{"from", text}, {"to", text}, {"label", text}}}};
    const Json schema{
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"action"}},
        {"properties",
         {{"action", {{"type", "string"}, {"enum", {"show", "focus", "clear"}}}},
          {"title", text},
          {"layout", {{"type", "string"}, {"enum", {"grid", "horizontal", "vertical"}}}},
          {"steps", {{"type", "array"}, {"items", step}, {"minItems", 1}, {"maxItems", 12}}},
          {"connections", {{"type", "array"}, {"items", edge}, {"maxItems", 24}}},
          {"focus", {{"type", "boolean"}}}}}};
    const Json tool{
        {"type", "function"},
        {"name", "canvas_walkthrough"},
        {"description",
         "Arrange real code excerpts together on the user's canvas to explain how something "
         "is wired. When asked to show a flow, read the source, then call action=show with "
         "short, ordered steps (inclusive 1-based ranges, at most 40 lines each), notes that "
         "explain why each matters, and labeled directed connections between step IDs. "
         "Multiple excerpts can come from the same file. Layout is grid (default), horizontal "
         "or vertical; optional x/y pairs override individual positions relative to the "
         "walkthrough. This replaces the current walkthrough and focuses it unless focus is "
         "false. action=focus revisits it; action=clear dismisses it. Source files are unchanged."},
        {"inputSchema", schema}};
    return Json::array({tool});
}
