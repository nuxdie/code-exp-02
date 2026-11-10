#pragma once
#include <nlohmann/json.hpp>
#include <string>

std::string intent_document(const nlohmann::json& records, const std::string& selected,
                            const nlohmann::json& evidence, bool overview,
                            const std::string& path = "");
std::string intent_literal(const std::string& text);
std::string intent_summary(const std::string& statement, std::size_t limit = 110);
bool intent_matches_path(const nlohmann::json& record, const std::string& path);
