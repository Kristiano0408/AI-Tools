#pragma once

#include <string>
#include <cstdint>

namespace knowledge_store {
class KnowledgeStore;
}

namespace repository {

struct RepoInfo {
    std::string repo_id;    // stable identity: hash of canonical root path
    std::string root_path;  // canonical absolute path
    std::string name;
    std::string db_path;    // per-repository knowledge database
    std::string created_at;
    std::string updated_at;
};

// Root of the on-disk data area (databases). Override with the
// AI_TOOLS_DATA_DIR environment variable; defaults to $HOME/.ai-tools.
// Kept separate from the executable so installs can relocate the binary.
std::string data_dir();

// Global registry database (repo_id -> root_path)
std::string registry_db_path();

// Stable identity for a repository root (hash of canonical absolute path)
std::string repo_identity(const std::string& root_path);

// Location of the per-repository knowledge database
std::string get_db_path(const std::string& root_path);

// Get or create repository identity; registers it in the global registry and
// ensures the per-repository knowledge database exists.
RepoInfo get_or_create_repository(const std::string& root_path);

// Look up a repository by identity via the global registry.
// Returns RepoInfo with an empty repo_id when unknown.
RepoInfo lookup_repository(const std::string& repo_id);

// Open the per-repository knowledge store for the given root (creating it).
bool open_repo_store(const std::string& root_path, knowledge_store::KnowledgeStore& store);

// Validate that a repository identity is registered
bool repository_exists(const std::string& repo_id);

}
