#pragma once

#include <string>

namespace server {

// Initialize the MCP server
void init();

// Run the server (process JSON-RPC messages)
void run();

// Handle a single JSON-RPC request
std::string handle_request(const std::string& request);

}
