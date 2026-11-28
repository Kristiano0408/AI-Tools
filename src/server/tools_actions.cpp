// Mutating tool handlers: summary, symbol summary, index, edit, execute.

#include "server/tools_internal.h"

#include "engine/indexer.h"
#include "engine/summary_store.h"
#include "engine/symbol_index.h"
#include "utils/file_utils.h"
#include "utils/path_utils.h"
#include "utils/perf_metrics.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace tools {

namespace {

// Single-quote a string for safe use in a shell command
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Create a unique, unpredictable temp file (mode 0600) via mkstemp.
// Returns the path, or "" on failure. The caller owns and must unlink it.
std::string make_temp_file(const std::string& prefix) {
    std::string tmpl = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) return "";
    close(fd); // we only need the path; the shell will write into it
    return std::string(buf.data());
}

int execute_shell(const std::string& command, const std::string& cwd, std::string& out, std::string& err, int& duration_ms) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    // Unique per-invocation temp files: no fixed names, so concurrent calls
    // (VS Code + CLI, or parallel tool calls) never clobber each other, and
    // mkstemp avoids the symlink/race exposure of predictable /tmp paths.
    std::string outfile = make_temp_file("ai-tools-out-");
    std::string errfile = make_temp_file("ai-tools-err-");
    if (outfile.empty() || errfile.empty()) {
        if (!outfile.empty()) std::remove(outfile.c_str());
        if (!errfile.empty()) std::remove(errfile.c_str());
        duration_ms = 0;
        return -1;
    }

    std::string full = "cd " + shell_quote(cwd) + " && ( " + command + " ) > " +
                       shell_quote(outfile) + " 2> " + shell_quote(errfile);

    int rc = std::system(full.c_str());
    int exit_code = (rc == -1) ? -1 : WEXITSTATUS(rc);

    auto end = clock::now();
    duration_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    out = file_utils::read_file(outfile);
    err = file_utils::read_file(errfile);

    std::remove(outfile.c_str());
    std::remove(errfile.c_str());
    return exit_code;
}

} // namespace

std::string handle_summary(const ToolContext& ctx) {
    PERF_SCOPE("handler.summary");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (repo_id.empty()) return tool_error("summary requires repo_id");

    if (arguments.contains("file_path")) {
        std::string file_path = guarded_path(repo, arguments["file_path"].get<std::string>());
        if (file_path.empty()) return tool_error("Invalid file path");

        // A summary is attached to the file's indexed content hash, so the
        // file must exist in the index. Self-heal a stale/missing entry; if
        // the file type is not indexable, report it instead of silently
        // claiming the summary was stored. The self-heal goes through the one
        // update entry point, so a file whose content changed here also
        // invalidates what was derived from the superseded version (DEP-03).
        indexer::UpdateReport refresh;
        if (indexer::needs_indexing(store, file_path, repo_id)) {
            refresh = indexer::index_file(store, file_path, repo_id);
        }
        auto indexed = store.query("SELECT file_id FROM files WHERE repo_id = ? AND path = ?", repo_id, file_path);
        if (indexed.empty()) {
            // DB-01: distinguish 'not indexable' (skip: unsupported type or
            // unreadable) from 'indexing failed' (a storage failure rolled the
            // write back): the recovery step is different (re-scan, not give-up).
            if (!refresh.indexed && !refresh.skipped_language && !refresh.unreadable)
                return tool_error("Index update failed for " + file_path + " (" +
                                  (refresh.error.empty() ? "storage failure" : refresh.error) +
                                  "): cannot attach a summary to knowledge that was not written. "
                                  "Call the index tool with file_path and retry.");
            return tool_error("File is not indexable (unsupported type or unreadable); cannot attach a summary: " + file_path);
        }

        if (arguments.contains("write")) {
            // DB-01: store_file_summary is transactional and reports failure.
            // On false nothing was applied — reporting {"stored":true} would
            // make the agent believe a summary exists when it does not.
            if (!summary_store::store_file_summary(store, repo_id, file_path, arguments["write"].get<std::string>()))
                return tool_error("Failed to store summary for " + file_path +
                                  " (storage failure): the previous summary was not replaced.");
            json written = {{"stored", true}, {"path", file_path}};
            attach_dependents_report(written, refresh);
            return tool_result(written);
        }
        std::string summary = summary_store::get_file_summary(store, repo_id, file_path);
        json read_out = {{"path", file_path}, {"summary", summary}, {"valid", !summary.empty()}};
        attach_dependents_report(read_out, refresh);
        return tool_result(read_out);
    }

    if (arguments.contains("write")) {
        // DB-01: the repo summary write is checked: on failure nothing was
        // stored, and {"stored":true} would be a false claim.
        if (!summary_store::store_repo_summary(store, repo_id, arguments["write"].get<std::string>()))
            return tool_error("Failed to store repository summary for " + repo_id +
                              " (storage failure): the summary was not saved.");
        return tool_result(json{{"stored", true}});
    }
    std::string summary = summary_store::get_repo_summary(store, repo_id);
    return tool_result(json{{"repo_id", repo_id}, {"summary", summary}, {"valid", !summary.empty()}});
}

// SUM-01 — summaries per symbol.
//
// Three modes, chosen by which arguments are present:
//   write  : file_path + symbol + write
//   read   : file_path + symbol
//   list   : file_path (every symbol of that file, with its state)
//            no file_path (every indexed symbol of the repository with a summary)
//
// The point of the tool is that a symbol's knowledge is answerable without
// reading its body, and that a missing/outdated summary is stated as such —
// never an empty result indistinguishable from 'no such symbol' (the IDX-04
// lesson applied to knowledge instead of coverage).
std::string handle_symbol_summary(const ToolContext& ctx) {
    PERF_SCOPE("handler.symbol_summary");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (repo_id.empty()) return tool_error("symbol_summary requires repo_id");

    const bool has_file = arguments.contains("file_path");
    const bool has_symbol = arguments.contains("symbol") &&
                            !arguments["symbol"].get<std::string>().empty();
    const bool has_write = arguments.contains("write");
    const std::string symbol_type =
        arguments.contains("symbol_type") ? arguments["symbol_type"].get<std::string>() : std::string();

    // ---- repo-wide listing: every stored symbol summary that still matches a
    // live symbol. Reads the persisted index across the whole repository, so it
    // goes through the coverage gate like the other repo-level queries (IDX-01):
    // without a certified full scan, 'this summary still matches' could not be
    // claimed, and an envelope of matches from a partial index would be exactly
    // the silent-partial-result shape that gate exists to prevent. ----
    if (!has_file) {
        if (has_write)
            return tool_error("symbol_summary write requires file_path and symbol");
        if (has_symbol)
            return tool_error("symbol_summary read requires file_path with a symbol "
                              "(omit symbol to list the repository's symbol summaries)");

        json result = {{"repo_id", repo_id}};
        if (!ensure_repo_indexed(store, repo_id, repo, result)) return tool_result(result);

        auto rows = summary_store::list_repo_symbol_summaries(store, repo_id);
        if (rows.empty() && store.last_query_failed()) {
            result["error"] = "cannot read the symbol summary inventory: " + store.get_error();
            result["hint"] = "Retry after an index scan of the repository.";
            return tool_result(result);
        }
        json items = json::array();
        for (const auto& r : rows) {
            items.push_back(json{
                {"file_path", r.region.file_path},
                {"symbol", r.region.qualified_name},
                {"name", r.region.name},
                {"symbol_type", r.region.symbol_type},
                {"start_line", r.region.start_line},
                {"end_line", r.region.end_line},
                {"body_hash", r.region.body_hash},
                {"state", r.has_valid_summary ? "valid" : "stale"},
                {"summary", r.summary_text},
                {"model_version", r.model_version},
                {"prompt_version", r.prompt_version},
                {"verification_state", r.verification_state},
                {"created_at", r.created_at}});
        }
        result["symbol_summaries"] = items;
        paginate_array(result["symbol_summaries"], arguments, result, "symbol_summaries", 50);
        result["hint"] = "state 'valid' means the stored text still matches the live symbol body; "
                         "'stale' means the symbol's bytes changed since it was written. Omit nothing "
                         "to get a summary: file_path + symbol reads one, file_path alone lists that "
                         "file's symbols with each symbol's state.";
        return tool_result(result);
    }

    std::string file_path = guarded_path(repo, arguments["file_path"].get<std::string>());
    if (file_path.empty()) return tool_error("Invalid file path");
    if (!path_utils::file_exists(file_path)) return tool_error("File not found: " + file_path);

    // Same self-heal as the file-level summary tool: a summary can only attach
    // to indexed knowledge, so a stale or never-indexed file is re-parsed first,
    // through the ONE update entry point (which propagates to dependents, DEP-03)
    // and, at the same time, fills the identity/body-hash columns the whole
    // feature reads and writes against.
    indexer::UpdateReport refresh;
    if (indexer::needs_indexing(store, file_path, repo_id)) {
        refresh = indexer::index_file(store, file_path, repo_id);
    }

    auto report_refresh = [&](json& out) { attach_dependents_report(out, refresh); };

    // ---- list mode: every symbol of this file, with whether its summary is
    // cached, stale, or absent. This is what lets an agent pick a symbol it
    // already has knowledge about instead of reading the file. ----
    if (!has_symbol && !has_write) {
        auto rows = summary_store::list_symbol_summaries(store, repo_id, file_path);
        if (rows.empty() && store.last_query_failed()) {
            json out = {{"path", file_path},
                        {"error", "cannot read the symbol inventory: " + store.get_error()}};
            report_refresh(out);
            return tool_result(out);
        }
        json items = json::array();
        for (const auto& r : rows) {
            json item{{"symbol", r.region.qualified_name},
                      {"name", r.region.name},
                      {"symbol_type", r.region.symbol_type},
                      {"start_line", r.region.start_line},
                      {"end_line", r.region.end_line},
                      {"state", r.has_valid_summary ? "valid"
                                                    : (r.has_stale_summary ? "stale" : "none")}};
            if (!r.region.body_hash.empty()) item["body_hash"] = r.region.body_hash;
            else item["body_hash_verified"] = false; // pre-SUM-01 row: hash not computed yet
            if (r.has_valid_summary) {
                item["summary"] = r.summary_text;
                item["model_version"] = r.model_version;
                item["prompt_version"] = r.prompt_version;
                item["verification_state"] = r.verification_state;
                item["created_at"] = r.created_at;
            }
            items.push_back(item);
        }
        json out = {{"path", file_path}, {"symbols", items}};
        paginate_array(out["symbols"], arguments, out, "symbols", 50);
        out["has_symbols"] = !items.empty();
        out["hint"] = "state 'valid' = a summary is readable for that symbol (read it with file_path + "
                      "symbol); 'stale' = a summary exists but the symbol's body changed; 'none' = write "
                      "one with file_path + symbol + write.";
        report_refresh(out);
        return tool_result(out);
    }

    if (!has_symbol) return tool_error("symbol_summary write requires symbol");
    const std::string symbol = arguments["symbol"].get<std::string>();

    summary_store::SymbolSummaryInfo info;
    if (has_write) {
        bool stored = summary_store::store_symbol_summary(
            store, repo_id, file_path, symbol, symbol_type,
            arguments["write"].get<std::string>(),
            arguments.contains("model_version") ? arguments["model_version"].get<std::string>() : std::string(),
            arguments.contains("prompt_version") ? arguments["prompt_version"].get<std::string>() : std::string(),
            info);
        if (!stored) {
            json err_payload{{"stored", false},
                             {"path", file_path},
                             {"symbol", symbol},
                             {"status", info.status},
                             {"error", "cannot store the symbol summary"}};
            // DB-01: 'stored: false' plus the reason, never a bare failure —
            // the agent must know whether to disambiguate, re-index the file,
            // or pick another symbol.
            if (info.status == summary_store::symbol_status::kAmbiguous) {
                json cands = json::array();
                for (const auto& c : info.candidates) {
                    cands.push_back(json{{"symbol", c.qualified_name},
                                         {"symbol_type", c.symbol_type},
                                         {"start_line", c.start_line},
                                         {"end_line", c.end_line}});
                }
                err_payload["candidates"] = cands;
                err_payload["hint"] = "Several symbols match this name (overload, same name in "
                                      "different scopes). Repeat with one qualified symbol from "
                                      "candidates, or add symbol_type to filter.";
            } else if (info.status == summary_store::symbol_status::kNotFound) {
                auto symbols = symbol_index::list_file_symbol_regions(store, repo_id, file_path);
                json cands = json::array();
                for (size_t i = 0; i < symbols.size() && i < 25; ++i)
                    cands.push_back(json{{"symbol", symbols[i].qualified_name},
                                         {"symbol_type", symbols[i].symbol_type},
                                         {"start_line", symbols[i].start_line}});
                err_payload["available_symbols"] = cands;
                err_payload["hint"] = "No indexed symbol with this name in this file. Write a summary "
                                      "for one of available_symbols (qualified names).";
            } else if (info.status == summary_store::symbol_status::kNoFile) {
                err_payload["hint"] = "This file has no live index record. Re-index it with the index "
                                      "tool (file_path) and retry.";
            } else {
                err_payload["hint"] = "The file's index does not match its content or the write failed. "
                                      "Re-index the file with the index tool (file_path) and retry.";
            }
            report_refresh(err_payload);
            return tool_error(err_payload.dump());
        }
        json written = {{"stored", true},
                        {"path", file_path},
                        {"symbol", info.symbol_key},
                        {"name", info.name},
                        {"symbol_type", info.symbol_type},
                        {"start_line", info.start_line},
                        {"end_line", info.end_line},
                        {"body_hash", info.body_hash},
                        {"model_version", info.model_version},
                        {"prompt_version", info.prompt_version},
                        {"verification_state", info.verification_state},
                        {"valid", true}};
        report_refresh(written);
        return tool_result(written);
    }

    std::string text = summary_store::get_symbol_summary(store, repo_id, file_path, symbol, symbol_type, info);
    json out = {{"path", file_path},
                {"symbol", info.symbol_key.empty() ? symbol : info.symbol_key},
                {"valid", !text.empty()},
                {"status", info.status}};
    if (!text.empty()) {
        out["summary"] = text;
        out["name"] = info.name;
        out["symbol_type"] = info.symbol_type;
        out["start_line"] = info.start_line;
        out["end_line"] = info.end_line;
        out["body_hash"] = info.body_hash;
        out["model_version"] = info.model_version;
        out["prompt_version"] = info.prompt_version;
        out["verification_state"] = info.verification_state;
        out["created_at"] = info.created_at;
    } else if (info.status == summary_store::symbol_status::kStaleBody) {
        // 'A summary exists but describes a different version of this symbol'
        // must never read like 'nothing was ever stored': the recovery step is
        // to re-read the symbol's region (its live line range is here) and write
        // a new summary.
        auto live = symbol_index::find_symbol_regions(store, repo_id, file_path, symbol, symbol_type);
        if (live.size() == 1) {
            out["symbol"] = live[0].qualified_name;
            out["start_line"] = live[0].start_line;
            out["end_line"] = live[0].end_line;
            if (!info.body_hash.empty()) out["body_hash"] = info.body_hash;
        }
        out["hint"] = "A summary is stored for this symbol but its body changed since. Read lines "
                      "start_line..end_line of this file and write an updated summary (file_path + "
                      "symbol + write).";
    } else if (info.status == summary_store::symbol_status::kAmbiguous) {
        json cands = json::array();
        for (const auto& c : info.candidates) {
            cands.push_back(json{{"symbol", c.qualified_name},
                                 {"symbol_type", c.symbol_type},
                                 {"start_line", c.start_line},
                                 {"end_line", c.end_line}});
        }
        out["candidates"] = cands;
        out["hint"] = "Several symbols match this name. Repeat with a qualified symbol from candidates "
                      "or add symbol_type.";
    } else if (info.status == summary_store::symbol_status::kStaleFile) {
        out["hint"] = "The file is not in the persisted index or its bytes are newer than its index "
                      "record: re-index it (index tool with file_path) and read again.";
    } else if (info.status == summary_store::symbol_status::kNone) {
        out["hint"] = "No summary is stored for this symbol. Read its region (file_content with "
                      "start_line/end_line) and store one (file_path + symbol + write).";
    } else {
        out["hint"] = "This file has no indexed symbol with that name.";
    }
    report_refresh(out);
    return tool_result(out);
}

std::string handle_index(const ToolContext& ctx) {
    PERF_SCOPE("handler.index");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (repo_id.empty()) return tool_error("index requires repo_id or path");

    if (arguments.contains("file_path")) {
        std::string file_path = guarded_path(repo, arguments["file_path"].get<std::string>());
        if (file_path.empty()) return tool_error("Invalid file path");
        // One update entry point (DEP-03): index_snapshot writes this file's
        // knowledge and invalidates everything derived from the version it
        // replaced, in the same transaction. Indexing a changed header through
        // this tool never leaves its includers with stale symbols/summaries.
        auto report = indexer::index_file(store, file_path, repo_id);
        json single = {{"indexed", file_path}};
        if (!report.indexed && (report.skipped_language || report.unreadable))
            single["skipped"] = report.skipped_language ? "unsupported file type" : "file unreadable";
        // DB-01: a storage failure is NOT a skip and not a success. The file's
        // knowledge was not written (its transaction rolled back), so saying
        // 'indexed' would be a lie: report the failure with its cause and do
        // not leave the indexed claim in the envelope.
        if (!report.indexed && !report.skipped_language && !report.unreadable) {
            single.erase("indexed");
            single["status"] = "index_failed";
            single["hint"] = "The file's knowledge could not be stored: " +
                             (report.error.empty() ? "storage failure" : report.error) +
                             ". Nothing was written (the update was rolled back); call index again.";
        }
        attach_dependents_report(single, report);
        // Indexing one file alone never makes the repository index complete:
        // report the coverage the repository index currently has (IDX-01).
        report_index_coverage(store, repo_id, single);
        single["note"] = "Single-file indexing is partial; run the index tool without file_path "
                         "for a complete repository scan.";
        return tool_result(single);
    }

    indexer::FullScanResult scan = indexer::index_repository(store, repo_id);
    json result = {{"repo_id", repo_id}, {"root_path", repo.root_path}};
    if (!scan.ok) {
        result["status"] = "index_failed";
        // DB-01: the real cause of the stopped scan, not just the fact that it
        // stopped. Without it the agent cannot distinguish 'the repository root
        // is wrong' from 'a write failed halfway through the re-index'.
        result["hint"] = "The repository scan did not complete"
                        + (scan.error.empty() ? "" : ": " + scan.error) +
                        "; the index is not trusted for repository-level queries. "
                        "Check the repository root path and retry the index tool.";
        report_index_coverage(store, repo_id, result);
        return tool_result(result);
    }
    result["status"] = "indexed";
    result["files_refreshed"] = scan.files_refreshed; // files actually re-parsed (unchanged skipped)
    // IDX-04: unchanged files are not just skipped — their content was compared
    // and the verification stamp was refreshed in place, which is what makes the
    // coverage revalidate as full_fresh after this scan instead of as stale.
    result["files_verified"] = scan.files_verified;
    // Records the scan found gone from disk and reconciled out of the index
    // (tombstoned, with their symbols/summaries/edges dropped) (IDX-02).
    if (scan.files_reconciled > 0)
        result["files_reconciled_deleted"] = scan.files_reconciled;
    // DEP-03: dependents the scan invalidated (through deleted files or
    // through a re-parsed include/import) and the dependents it then re-parsed
    // to settle the chain before certifying coverage.
    if (scan.dependents_invalidated > 0)
        result["dependents_invalidated"] = scan.dependents_invalidated;
    if (scan.dependents_refreshed > 0)
        result["dependents_refreshed"] = scan.dependents_refreshed;
    if (scan.stabilization_passes > 0)
        result["stabilization_passes"] = scan.stabilization_passes;
    report_index_coverage(store, repo_id, result, nullptr);
    // Flat mirror of the coverage the just-completed scan certified, so
    // existing clients keep working while the authoritative data lives in
    // index_coverage.
    if (result.contains("index_coverage")) {
        const json& c = result["index_coverage"];
        result["files_indexed"] = c.value("files_indexed", 0);
        result["files_total"] = c.value("files_total", scan.files_total);
    } else {
        result["files_total"] = scan.files_total;
    }
    return tool_result(result);
}

std::string handle_edit(const ToolContext& ctx) {
    PERF_SCOPE("handler.edit");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    for (const char* key : {"file_path", "start_line", "end_line", "new_content"}) {
        if (arguments.find(key) == arguments.end()) return tool_error(std::string("Missing required argument: ") + key);
    }
    if (repo_id.empty()) return tool_error("edit requires repo_id or path");

    std::string file_path = guarded_path(repo, arguments["file_path"].get<std::string>());
    if (file_path.empty()) return tool_error("Path escapes repository boundary");

    int start_line = arguments["start_line"].get<int>();
    int end_line = arguments["end_line"].get<int>();
    std::string new_content = arguments["new_content"].get<std::string>();

    if (start_line < 1) return tool_error("start_line must be >= 1");
    if (end_line < start_line) return tool_error("end_line must be >= start_line");

    auto lines = file_utils::read_lines(file_path, 1, -1);
    if (lines.empty() && !path_utils::file_exists(file_path)) return tool_error("File not found: " + file_path);
    if (start_line > static_cast<int>(lines.size()) + 1) return tool_error("start_line beyond end of file");
    if (end_line > static_cast<int>(lines.size())) end_line = static_cast<int>(lines.size());

    // Splice: keep [1, start_line), insert new content, keep (end_line, ...]
    std::vector<std::string> replacement;
    std::istringstream stream(new_content);
    std::string line;
    while (std::getline(stream, line)) replacement.push_back(line);
    if (new_content.empty()) replacement.clear();

    std::vector<std::string> out;
    out.insert(out.end(), lines.begin(), lines.begin() + (start_line - 1));
    out.insert(out.end(), replacement.begin(), replacement.end());
    if (end_line < static_cast<int>(lines.size()))
        out.insert(out.end(), lines.begin() + end_line, lines.end());

    std::string joined;
    for (size_t i = 0; i < out.size(); ++i) {
        joined += out[i];
        joined += "\n";
    }

    if (!file_utils::write_file(file_path, joined)) return tool_error("Failed to write file: " + file_path);

    // Re-index straight from the content just written, held in memory: the
    // snapshot is hashed and parsed from the same buffer (IDX-03), and its
    // fresh edges rebuild what the invalidation dropped for this file.
    //
    // DEP-01/DEP-02/DEP-03: this single call is the whole invalidation flow.
    // index_snapshot collects the transitive dependent set (header includes
    // and Python imports) while the edges still exist, drops their symbols
    // and summaries and marks those records stale, and only then writes this
    // file's new knowledge — all inside one transaction. The handler no
    // longer sequences a separate cache pass, so the correct order is a
    // property of the module instead of a rule this function must remember.
    // force_propagate keeps the documented edit guarantee (an edit always
    // reconciles its dependents) even when the new bytes happen to match what
    // was already stored.
    auto invalidated = indexer::index_snapshot(store, file_path, repo_id, joined,
                                               /*force_propagate=*/true);

    // DB-01: the write of the edited file's knowledge succeeded or was rolled
    // back as one unit. If it failed, the file ON DISK is already changed (the
    // edit tool wrote it before re-indexing), so the index is now behind the
    // content: say so explicitly instead of returning the normal edit envelope,
    // and give the agent the recovery step (index the file or run a full scan).
    if (!invalidated.indexed) {
        json failed = {
            {"path", file_path},
            {"replaced_lines", {start_line, end_line}},
            {"new_line_count", static_cast<int>(out.size())},
            {"cache_invalidated", false},
            {"status", "index_failed"},
            {"hint", "The file was edited on disk but its index update failed (" +
                     (invalidated.error.empty() ? "storage failure" : invalidated.error) +
                     "): the knowledge update was rolled back, so the stored index no longer "
                     "describes this file. Call the index tool with file_path to re-index it."}};
        return tool_result(failed);
    }

    json data = {
        {"path", file_path},
        {"replaced_lines", {start_line, end_line}},
        {"new_line_count", static_cast<int>(out.size())},
        {"cache_invalidated", true}};
    attach_dependents_report(data, invalidated, /*always=*/true);
    return tool_result(data);
}

std::string handle_execute(const ToolContext& ctx) {
    PERF_SCOPE("handler.execute");
    const json& arguments = ctx.arguments;
    const repository::RepoInfo& repo = ctx.repo;

    if (arguments.find("command") == arguments.end()) return tool_error("Missing required argument: command");
    // execute runs arbitrary shell commands: require an explicit repo_id
    // so the repository boundary is always intentional (no auto-detect).
    if (!arguments.contains("repo_id") || arguments["repo_id"].is_null() || repo.repo_id.empty())
        return tool_error("execute requires repo_id (commands run in the repository root)");

    std::string command = arguments["command"].get<std::string>();
    std::string out, err;
    int duration_ms = 0;
    int exit_code = execute_shell(command, repo.root_path, out, err, duration_ms);

    json result = {
        {"command", command},
        {"working_directory", repo.root_path},
        {"exit_code", exit_code},
        {"duration_ms", duration_ms},
        {"stdout", out},
        {"stderr", err}};
    return tool_result(result);
}

}
