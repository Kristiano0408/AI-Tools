#include "repository.h"
#include "knowledge_store.h"
#include "../utils/path_utils.h"
#include "../utils/perf_metrics.h"
#include "../engine/content_hasher.h"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace repository {

static std::string format_timestamp(time_t t) {
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

static std::string registry_path() {
    return data_dir() + "/registry.db";
}

std::string data_dir() {
    const char* override_dir = std::getenv("AI_TOOLS_DATA_DIR");
    if (override_dir && *override_dir) return std::string(override_dir);
#ifdef AI_TOOLS_DEFAULT_DATA_DIR
    // Compiled-in default chosen by the installer (CMake -DAI_TOOLS_DATA_DIR=...)
    return AI_TOOLS_DEFAULT_DATA_DIR;
#endif
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.ai-tools" : "/tmp/ai-tools";
}

std::string registry_db_path() {
    return data_dir() + "/registry.db";
}

std::string repo_identity(const std::string& root_path) {
    PERF_SCOPE("repository.repo_identity");
    std::string canonical = path_utils::normalize_path(std::filesystem::absolute(root_path));
    return content_hasher::compute_string_hash(canonical).substr(0, 16);
}

std::string get_db_path(const std::string& root_path) {
    return data_dir() + "/repos/" + repo_identity(root_path) + "/knowledge.db";
}

RepoInfo get_or_create_repository(const std::string& root_path) {
    PERF_SCOPE("repository.get_or_create_repository");
    RepoInfo info;
    std::string canonical = path_utils::normalize_path(std::filesystem::absolute(root_path));
    std::string identity = repo_identity(canonical);
    std::string db_path = get_db_path(canonical);

    // Ensure the per-repository database exists (opens + initializes schema)
    std::filesystem::create_directories(std::filesystem::path(db_path).parent_path());
    {
        knowledge_store::KnowledgeStore repo_store;
        if (!repo_store.open(db_path)) return info;
        // Mirror the identity into the per-repo store so engine components
        // (indexer, etc.) can resolve repo_id -> root_path locally.
        repo_store.execute(
            "INSERT OR IGNORE INTO repositories (repo_id, root_path, name, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
            repo_identity(canonical), canonical, path_utils::get_basename(canonical),
            format_timestamp(time(nullptr)), format_timestamp(time(nullptr)));
    }

    // Register in the global registry
    knowledge_store::KnowledgeStore registry;
    if (!registry.open(registry_path())) return info;

    std::string now = format_timestamp(time(nullptr));

    auto rows = registry.query(
        "SELECT repo_id, root_path, name, created_at, updated_at FROM repositories WHERE repo_id = ?", identity);
    if (!rows.empty()) {
        info.repo_id = rows[0][0];
        info.root_path = rows[0][1];
        info.name = rows[0][2];
        info.db_path = db_path;
        info.created_at = rows[0][3];
        info.updated_at = now;
        registry.execute("UPDATE repositories SET updated_at = ? WHERE repo_id = ?", now, identity);
        return info;
    }

    registry.execute(
        "INSERT INTO repositories (repo_id, root_path, name, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
        identity, canonical, path_utils::get_basename(canonical), now, now);

    info.repo_id = identity;
    info.root_path = canonical;
    info.name = path_utils::get_basename(canonical);
    info.db_path = db_path;
    info.created_at = now;
    info.updated_at = now;
    return info;
}

RepoInfo lookup_repository(const std::string& repo_id) {
    PERF_SCOPE("repository.lookup_repository");
    RepoInfo info;
    knowledge_store::KnowledgeStore registry;
    if (!registry.open(registry_path())) return info;

    auto rows = registry.query(
        "SELECT repo_id, root_path, name, created_at, updated_at FROM repositories WHERE repo_id = ?", repo_id);
    if (rows.empty()) return info;
    info.repo_id = rows[0][0];
    info.root_path = rows[0][1];
    info.name = rows[0][2];
    info.db_path = get_db_path(info.root_path);
    info.created_at = rows[0][3];
    info.updated_at = rows[0][4];
    return info;
}

bool open_repo_store(const std::string& root_path, knowledge_store::KnowledgeStore& store) {
    PERF_SCOPE("repository.open_repo_store");
    std::string db_path = get_db_path(root_path);
    std::filesystem::create_directories(std::filesystem::path(db_path).parent_path());
    return store.open(db_path);
}

bool repository_exists(const std::string& repo_id) {
    return !lookup_repository(repo_id).repo_id.empty();
}

}
