#pragma once

#include "knowledge_store.h"
#include "symbol_index.h"
#include <string>
#include <vector>

namespace summary_store {

// Generate and store summary (or repository summary). Returns false when the
// write failed (file not in the index, or the store rejected/failed the
// insert): callers should check the result so a failed insert is never
// reported to the agent as a stored summary (DB-01).
bool store_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path, const std::string& summary);
bool store_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& summary);

// Get file summary
std::string get_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Get repository summary
std::string get_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

// ---------------------------------------------------------------------------
// SUM-01 — summaries per symbol
//
// The granularity the project exists for: ask about one function and get its
// summary without reading the file. The file-level functions above stay as the
// aggregate level.
//
// Identity: a summary keys on the symbol's qualified_name (symbols.qualified_name)
// plus its symbol_type, NOT on symbols.symbol_id — index_snapshot() deletes and
// recreates every symbol row on each re-index, so symbol_id is not an identity
// that survives one. qualified_name + body_hash are recomputed identically from
// the snapshot, so a summary survives a re-index of unchanged content.
//
// Validity is get_file_summary()'s double check, one level down:
//   1. is_valid — maintained by every invalidation path (re-index supersession,
//      dependent reaps, deleted-file reconciliation);
//   2. body_hash — the stored hash of the symbol's region must equal the
//      body_hash of the LIVE symbol row, i.e. the hash of the bytes the current
//      index was derived from. A match proves the summary describes the bytes on
//      disk today, because symbols.body_hash and files.content_hash always come
//      from one snapshot (IDX-03).
// ---------------------------------------------------------------------------

// Why a per-symbol read returned no text. Every value is a real state, never a
// stand-in for another: 'no summary stored' and 'a summary exists but describes
// different bytes' must stay distinguishable, exactly as an empty search result
// and a blocked query had to be (IDX-04).
namespace symbol_status {
inline constexpr const char* kOk = "ok";                         // text returned
inline constexpr const char* kNone = "no_summary";               // nothing stored for this symbol
inline constexpr const char* kStaleBody = "stale_symbol_body";   // a summary exists; the symbol's body changed
inline constexpr const char* kStaleFile = "stale_file_index";    // the file's index no longer matches disk
inline constexpr const char* kAmbiguous = "ambiguous_symbol";    // several symbols match: candidates filled
inline constexpr const char* kNotFound = "symbol_not_indexed";   // no live symbol matches in this file
inline constexpr const char* kNoFile = "file_not_indexed";       // the file has no live index record
}

struct SymbolSummaryInfo {
    std::string status = symbol_status::kNone;
    std::string file_path;
    std::string symbol_key;     // qualified_name of the matched/summarized symbol
    std::string name;           // bare name, when the language spells it differently
    std::string symbol_type;
    std::string scope;
    long long start_line = 0;
    long long end_line = 0;
    std::string body_hash;          // hash the summary is keyed to live against
    std::string file_content_hash;  // files.content_hash of the owning record
    std::string model_version;
    std::string prompt_version;
    std::string verification_state;
    std::string created_at;
    bool has_summary_row = false;   // a stored row exists for this symbol (any validity)
    // Filled when status == kAmbiguous: the candidates the caller must choose from.
    std::vector<symbol_index::SymbolRegion> candidates;
};

// Read the valid summary of one symbol in one file. `symbol` may be the
// qualified name ('cache::reset_cache', 'Config.init') or the bare name when
// that is unambiguous; `symbol_type` filters an otherwise ambiguous match
// (overload/same-name disambiguation). Returns the text only when the summary is
// valid for the CURRENT bytes; otherwise returns "" and `out` says why.
std::string get_symbol_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                               const std::string& file_path, const std::string& symbol,
                               const std::string& symbol_type, SymbolSummaryInfo& out);

// Read the valid summary of one symbol, without a type filter ('any symbol with
// this name'), the shape most agents use.
inline std::string get_symbol_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                                      const std::string& file_path, const std::string& symbol,
                                      SymbolSummaryInfo& out) {
    return get_symbol_summary(store, repo_id, file_path, symbol, std::string(), out);
}

// Store (or replace) the summary of one symbol. It attaches to the live symbol
// row's body hash, so it can only be read back while those bytes are unchanged.
// Returns false — with `out.status` explaining why — when the symbol is ambiguous
// or unindexed, the file's index is superseded, or the write failed; false means
// nothing was applied (DB-01), so the caller must not report a stored summary.
bool store_symbol_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                          const std::string& file_path, const std::string& symbol,
                          const std::string& symbol_type, const std::string& summary,
                          const std::string& model_version, const std::string& prompt_version,
                          SymbolSummaryInfo& out);

// One entry of the per-file inventory: every live symbol with its summary state,
// so an agent can see which symbols already have knowledge cached and pick one
// instead of reading the file.
struct SymbolSummaryRow {
    symbol_index::SymbolRegion region;
    bool has_valid_summary = false;    // a valid summary is readable for this symbol
    bool has_stale_summary = false;    // a summary exists but describes different bytes
    std::string summary_text;          // valid text only (empty otherwise)
    std::string model_version;
    std::string prompt_version;
    std::string verification_state;
    std::string created_at;
};

// All live symbols of one file with their summary states. An empty result means
// the file has no live symbols — or the read failed: check
// store.last_query_failed() before treating an empty inventory as 'nothing here'.
std::vector<SymbolSummaryRow> list_symbol_summaries(knowledge_store::KnowledgeStore& store,
                                                    const std::string& repo_id,
                                                    const std::string& file_path);
// Repository-wide inventory: every stored symbol summary joined to the LIVE
// symbol row it matches (same identity, same body hash, live file record).
//
// Unlike the per-file inventory this performs no on-disk verification, so the
// caller owns the freshness of the index it asks about: the MCP layer calls it
// only behind ensure_repo_indexed(), which certifies that a completed full scan
// covers the repository as it is on disk (IDX-01/IDX-04). Without that
// certification the answer would describe the index as of the last scan, and a
// summary whose symbol changed since would be reported as matched.
std::vector<SymbolSummaryRow> list_repo_symbol_summaries(knowledge_store::KnowledgeStore& store,
                                                         const std::string& repo_id);
}

