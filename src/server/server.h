#pragma once

#include <string>

namespace server {

// Initialize the MCP server
void init();

// Run the server (process JSON-RPC messages)
void run();

// Handle a single JSON-RPC request or notification from the client.
// Handles client->server responses carrying ids this server issued (e.g. the
// `roots/list` poll), so workspace roots stay in sync with the client.
std::string handle_request(const std::string& request);

// Server->client requests generated while handling a message (MCP Roots:
// the server polls the client with `roots/list`). The transport must write
// these to the client; returns "" when none are pending.
std::string next_pending_request();

}
