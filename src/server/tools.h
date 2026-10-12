#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace tools {

// List available tools
std::string list_tools();

// Call a specific tool
std::string call_tool(const std::string& tool_name, const nlohmann::json& arguments);

}
