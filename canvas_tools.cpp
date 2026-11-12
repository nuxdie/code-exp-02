#include "canvas_tools.h"

nlohmann::json canvas_tools()
{
    using Json = nlohmann::json;
    const Json intent{
        {"type", "function"},
        {"name", "canvas_intent"},
        {"description",
         "Record which of the user's standing decisions this turn served, so every line you "
         "wrote stays traceable to it. Call once per turn that changed files, before your final "
         "answer. Pass intent with the id of the listed decision that fits. If none fits and the "
         "user stated something durable, pass statement instead: one short sentence in the "
         "user's voice, close to their wording. Do not call for questions, chores or one-off "
         "fixes."},
        {"inputSchema",
         {{"type", "object"},
          {"additionalProperties", false},
          {"properties",
           {{"intent", {{"type", "string"}, {"maxLength", 80}}},
            {"statement", {{"type", "string"}, {"maxLength", 240}}}}}}}};
    return Json::array({intent});
}
