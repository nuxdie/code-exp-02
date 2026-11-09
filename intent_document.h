#pragma once
#include <nlohmann/json.hpp>
#include <string>

std::string intent_document(const nlohmann::json& records, const std::string& selected,
                            const nlohmann::json& evidence, bool overview);
std::string intent_literal(const std::string& text);
