#pragma once

#include "knowledge_store.h"
#include <string>
#include <vector>

namespace symbol_index {

// One live symbol row with the facts a per-symbol artifact (a summary, the
// future symbol identity of MCP-04) needs in order to key on it and verify
// it (SUM-01). Sourced only from live file records (is_deleted = 0).
struct SymbolRegion {
    long long file_id = 0;
    std::string file_path;
    std::string name;
    std::string qualified_name; // symbols.qualified_name; = name at top level
    std::string symbol_type;
    std::string scope;
    long long start_line = 0;
    long long end_line = 0;
    bool has_body_hash = false; // false: legacy row predating SUM-01
    std::string body_hash;
    std::string file_content_hash; // files.content_hash of the owning record
};

// Live symbols of one file, ordered by their position in the file.
std::vector<SymbolRegion> list_file_symbol_regions(knowledge_store::KnowledgeStore& store,
                                                   const std::string& repo_id,
                                                   const std::string& file_path);

// Resolve one symbol inside one file (SUM-01: never a LIKE guess, PAR-03).
// A query matching a qualified_name exactly ('cache::reset_cache',
// 'Config.init') picks that symbol; otherwise the query is compared with the
// bare name, so the qualified part may be omitted ('reset_cache' resolves
// 'cache::reset_cache' when unambiguous). An empty query lists everything.
// More than one result means ambiguity: the caller must report the candidates
// instead of silently picking one. When symbol_type is non-empty it filters
// the candidates (overload/same-name disambiguation).
std::vector<SymbolRegion> find_symbol_regions(knowledge_store::KnowledgeStore& store,
                                              const std::string& repo_id,
                                              const std::string& file_path,
                                              const std::string& query,
                                              const std::string& symbol_type = "");

// Search symbols by name
std::vector<std::string> search_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& name);

// Get symbols by type
std::vector<std::string> get_symbols_by_type(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_type);

// Get symbols in a file
std::vector<std::string> get_file_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Get symbol details
std::vector<std::string> get_symbol_details(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name);

}
