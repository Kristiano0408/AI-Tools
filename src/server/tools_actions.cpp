// Mutating tool handlers: summary, index, edit, execute.

#include "server/tools_internal.h"

#include "engine/indexer.h"
#include "engine/summary_store.h"
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
        if (indexed.empty())
            return tool_error("File is not indexable (unsupported type or unreadable); cannot attach a summary: " + file_path);

        if (arguments.contains("write")) {
            summary_store::store_file_summary(store, repo_id, file_path, arguments["write"].get<std::string>());
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
        summary_store::store_repo_summary(store, repo_id, arguments["write"].get<std::string>());
        return tool_result(json{{"stored", true}});
    }
    std::string summary = summary_store::get_repo_summary(store, repo_id);
    return tool_result(json{{"repo_id", repo_id}, {"summary", summary}, {"valid", !summary.empty()}});
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
        result["hint"] = "The repository scan did not complete; the index is not trusted for "
                         "repository-level queries. Check the repository root path and retry.";
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
