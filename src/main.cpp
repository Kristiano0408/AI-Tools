#include "server/server.h"
#include "engine/repository.h"
#include "engine/indexer.h"
#include "engine/knowledge_store.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cout << "Usage: ai-tools [options] [repo_root ...]\n"
              << "MCP server over stdio (JSON-RPC 2.0). Repository roots passed as\n"
              << "arguments are registered at startup; tools also accept 'path' to\n"
              << "auto-register a repository on first use.\n\n"
              << "Options:\n"
              << "  --init-db        Create the data directory and the global registry\n"
              << "                   database, then exit (used by the installer).\n"
              << "  --register PATH  Register (and index) a repository without starting\n"
              << "                   the server; repeatable.\n"
              << "  -h, --help       Show this help.\n\n"
              << "Data directory: $AI_TOOLS_DATA_DIR, else the compiled-in default,\n"
              << "else $HOME/.ai-tools.\n";
}

int main(int argc, char** argv) {
    std::vector<std::string> repos;
    bool init_db = false;
    bool register_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--init-db") {
            init_db = true;
            continue;
        }
        if (arg == "--register") {
            if (i + 1 >= argc) {
                std::cerr << "ai-tools: --register requires a path\n";
                return 1;
            }
            register_only = true;
            repos.push_back(argv[++i]);
            continue;
        }
        repos.push_back(arg);
    }

    // Optional one-shot database bootstrap (installer entry point).
    if (init_db) {
        std::filesystem::create_directories(repository::data_dir());
        knowledge_store::KnowledgeStore registry;
        if (!registry.open(repository::registry_db_path())) {
            std::cerr << "ai-tools: failed to create registry database\n";
            return 1;
        }
        std::cout << "data directory: " << repository::data_dir() << "\n"
                  << "registry database: " << repository::registry_db_path() << "\n";
    }

    // Repository roots: registered at startup (server mode) or via --register.
    for (const std::string& arg : repos) {
        repository::RepoInfo info = repository::get_or_create_repository(arg);
        if (info.repo_id.empty()) {
            std::cerr << "ai-tools: failed to register repository: " << arg << "\n";
            continue;
        }
        knowledge_store::KnowledgeStore store;
        if (!repository::open_repo_store(info.root_path, store)) {
            std::cerr << "ai-tools: failed to open store for: " << arg << "\n";
            continue;
        }
        indexer::index_repository(store, info.repo_id);
        std::cerr << "ai-tools: registered repository " << info.repo_id
                  << " (" << info.root_path << ")\n";
    }

    if (init_db || register_only) return 0;

    server::run();
    return 0;
}
