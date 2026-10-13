#include "server/server.h"
#include "server/tools.h"
#include "utils/perf_metrics.h"
#include <iostream>
#include <string>
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

} // namespace

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
            // Probably a JSON-RPC notification/response from the client — ignore.
            return "";
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
                    "tools": {"listChanged": false}
                },
                "serverInfo": {
                    "name": "ai-tools",
                    "version": "1.0.0"
                }
            })json");
            result["protocolVersion"] = negotiated;
            return make_result(result, id).dump();
        }

        if (method == "notifications/initialized" || method == "initialized") {
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
    }
}

}
