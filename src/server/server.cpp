#include "server/server.h"
#include "server/resources.h"
#include "server/tools.h"
#include "server/workspace_roots.h"
#include "utils/perf_metrics.h"
#include <deque>
#include <iostream>
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>

using namespace nlohmann;

namespace server {

namespace {

json make_error(int code, const std::string& message, const json& id) {
    json err;
    err["jsonrpc"] = "2.0";
    err["error"] = {{"code", code}, {"message", message}};
    err["id"] = id;
    return err;
}

json make_result(const json& result, const json& id) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["result"] = result;
    resp["id"] = id;
    return resp;
}

// ---- MCP Roots session state ----
// The server polls the client with `roots/list` (a server->client request,
// unlike every other message this server emits). Pending requests are queued
// here and flushed by the stdio transport after each handled message; ids
// this server issued are remembered so the matching client response can be
// routed back into workspace_roots state.
constexpr int kRootsListIdBase = 900001;

std::deque<std::string>& pending_requests() {
    static std::deque<std::string> queue;
    return queue;
}

std::unordered_set<std::string>& pending_roots_ids() {
    static std::unordered_set<std::string> ids;
    return ids;
}

int next_roots_list_id() {
    static int next = kRootsListIdBase;
    return next++;
}

void queue_roots_list_request() {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = next_roots_list_id();
    req["method"] = "roots/list";
    pending_roots_ids().insert(req["id"].dump());
    pending_requests().push_back(req.dump());
}

} // namespace

std::string next_pending_request() {
    auto& queue = pending_requests();
    if (queue.empty()) return "";
    std::string front = queue.front();
    queue.pop_front();
    return front;
}

void init() {
    // Initialize global state
}

std::string handle_request(const std::string& request) {
    PERF_SCOPE("server.handle_request");
    PERF_VALUE("server.handle_request.bytes", request.size());
    json id = nullptr;
    try {
        json req = json::parse(request);

        if (req.contains("id")) id = req["id"];

        if (!req.contains("method")) {
            // ---- MCP Roots: client response to a `roots/list` we issued ----
            // The dispatcher normally only sees client->server requests; a
            // message with no method but carrying an id this server issued and
            // a `result` is the matching response to our poll.
            if (!id.is_null() && req.contains("result")) {
                std::string id_key = id.dump();
                if (pending_roots_ids().erase(id_key) > 0) {
                    const json& result = req["result"];
                    workspace_roots::set_from_roots_array(
                        (result.is_object() && result.contains("roots")) ? result["roots"] : json::array());
                }
            }
            return ""; // notification or response to our own request: no reply
        }

        std::string method = req["method"].get<std::string>();

        if (method == "initialize") {
            // MCP negotiation: if the client requests a protocol version we
            // implement, echo it back; otherwise respond with our latest
            // supported version (per spec — never reject the handshake).
            static const std::vector<std::string> supported = {
                "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
            std::string negotiated = supported.front();
            if (req.contains("params") && req["params"].is_object()) {
                const json& params = req["params"];
                if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
                    std::string requested = params["protocolVersion"].get<std::string>();
                    for (const auto& v : supported) {
                        if (requested == v) { negotiated = v; break; }
                    }
                }
            }

            json result = json::parse(R"json({
                "capabilities": {
                    "tools": {"listChanged": false},
                    "resources": {}
                },
                "serverInfo": {
                    "name": "ai-tools",
                    "version": "1.0.0"
                }
            })json");
            result["protocolVersion"] = negotiated;

            // If the client declared the roots capability, poll it once now
            // (the queue flushes right after this response is written).
            workspace_roots::clear();
            bool client_supports_roots = false;
            if (req.contains("params") && req["params"].is_object()) {
                const json& params = req["params"];
                client_supports_roots = params.contains("capabilities") &&
                                        params["capabilities"].is_object() &&
                                        params["capabilities"].contains("roots");
            }
            if (client_supports_roots) queue_roots_list_request();
            return make_result(result, id).dump();
        }

        if (method == "notifications/initialized" || method == "initialized") {
            return ""; // notification, no response
        }

        if (method == "notifications/roots/list_changed") {
            // The client announced its workspace roots changed: re-poll.
            queue_roots_list_request();
            return ""; // notification, no response
        }

        if (method == "ping") {
            return make_result(json::object(), id).dump();
        }

        if (method == "tools/list") {
            json parsed = json::parse(tools::list_tools());
            json result;
            result["tools"] = parsed;
            return make_result(result, id).dump();
        }

        if (method == "tools/call") {
            if (!req.contains("params") || !req["params"].contains("name")) {
                return make_error(-32602, "Invalid params: missing tool name", id).dump();
            }
            std::string tool_name = req["params"]["name"].get<std::string>();
            json arguments = req["params"].contains("arguments") ? req["params"]["arguments"] : json::object();

            std::string tool_output = tools::call_tool(tool_name, arguments);

            // tools::call_tool returns a full MCP tools/call result payload
            json result = json::parse(tool_output);
            // Add structuredContent so clients can consume the JSON payload
            // directly. To keep the payload on the wire exactly once, bulky
            // data fields (the file body in `content`) live only in
            // structuredContent: the text block carries the metadata plus a
            // `<field>_omitted` marker, not a second copy of the body.
            if (!result.value("isError", false) && result.contains("content") &&
                result["content"].is_array() && !result["content"].empty() &&
                result["content"][0].value("type", "") == "text") {
                try {
                    json payload = json::parse(result["content"][0]["text"].get<std::string>());
                    if (payload.is_object()) {
                        result["structuredContent"] = payload;
                        // Bulk fields travel once, in structuredContent only.
                        static const std::vector<std::string> kBulkFields = {"content"};
                        bool trimmed = false;
                        for (const auto& field : kBulkFields) {
                            if (payload.contains(field)) {
                                payload.erase(field);
                                payload[field + "_omitted"] = true;
                                trimmed = true;
                            }
                        }
                        if (trimmed) result["content"][0]["text"] = payload.dump();
                    }
                } catch (...) {
                    // Non-JSON tool payloads keep only the text content block.
                }
            }
            return make_result(result, id).dump();
        }

        // ---- MCP Resources interface ----
        // Exposes repository context (files, semantic summaries, index
        // overviews) so clients can attach it to a prompt without a tool call.
        if (method == "resources/list" || method == "resources/templates/list" ||
            method == "resources/read") {
            json params = (req.contains("params") && req["params"].is_object()) ? req["params"] : json::object();
            try {
                json result;
                if (method == "resources/list") {
                    result = resources::list(params);
                } else if (method == "resources/templates/list") {
                    result = resources::templates_list(params);
                } else {
                    result = resources::read(params);
                }
                return make_result(result, id).dump();
            } catch (const resources::resource_error& e) {
                json err = make_error(e.code, e.what(), id);
                if (method == "resources/read" && params.contains("uri"))
                    err["error"]["data"] = {{"uri", params["uri"]}};
                return err.dump();
            }
        }

        if (method == "resources/subscribe") {
            // Subscriptions are not supported; per spec an unsubscribable
            // resource surfaces as -32002 (Resource not found).
            json data = json::object();
            if (req.contains("params") && req["params"].is_object() && req["params"].contains("uri"))
                data["uri"] = req["params"]["uri"];
            json err = make_error(-32002, "Resource subscriptions are not supported", id);
            err["error"]["data"] = data;
            return err.dump();
        }

        return make_error(-32601, "Method not found: " + method, id).dump();

    } catch (const std::exception& e) {
        return make_error(-32600, std::string("Invalid request: ") + e.what(), id).dump();
    }
}

void run() {
    // MCP stdio transport: newline-delimited JSON-RPC messages.
    // (Content-Length framing is also accepted for LSP-style clients.)
    init();

    // MCP clients are not guaranteed to send notifications/initialized;
    // treat the server as ready immediately.
    std::string line;
    while (std::getline(std::cin, line)) {
        // Strip trailing CR (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) continue;

        std::string response;

        if (line.rfind("Content-Length:", 0) == 0) {
            // LSP-style framing: read headers until blank line, then body
            int length = 0;
            try { length = std::stoi(line.substr(15)); } catch (...) { continue; }
            std::string header;
            while (std::getline(std::cin, header)) {
                if (!header.empty() && header.back() == '\r') header.pop_back();
                if (header.empty()) break;
                if (header.rfind("Content-Length:", 0) == 0) {
                    try { length = std::stoi(header.substr(15)); } catch (...) {}
                }
            }
            std::string body(static_cast<size_t>(length), '\0');
            std::cin.read(&body[0], length);
            response = handle_request(body);
        } else {
            response = handle_request(line);
        }

        if (!response.empty()) {
            // Newline-delimited JSON (MCP stdio)
            std::cout << response << "\n" << std::flush;
        }

        // Flush any server->client requests generated while handling the
        // message (e.g. the roots/list poll). Written after the response so
        // the client sees request/reply pairs in order on one connection.
        while (true) {
            std::string pending = next_pending_request();
            if (pending.empty()) break;
            std::cout << pending << "\n" << std::flush;
        }
    }
}

}
