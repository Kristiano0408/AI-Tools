#include "engine/summary_store.h"
#include "engine/content_hasher.h"
#include "utils/file_utils.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace summary_store {

// Store (or replace) a file's semantic summary.
//
// DB-01: the invalidation of the previous summary and the insertion of the new
// one are one transaction. They used to be two independent autocommit writes:
// if the INSERT failed, the file ended up with its old summary invalidated and
// no replacement — the store would then report 'no valid summary' for a file
// that does have knowledge, and the agent would regenerate (or lose) it for a
// failure that was really a write error. Returns false when nothing was
// applied: the caller must not report a stored summary on false.
bool store_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path, const std::string& summary) {
    auto file_rows = store.query(
        "SELECT file_id, content_hash FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );
    if (file_rows.empty()) {
        if (store.last_query_failed()) {
            std::cerr << "ai-tools: cannot look up file for summary (" << file_path
                      << "): " << store.get_error() << "\n";
        }
        return false;
    }
    const std::string& file_id = file_rows[0][0];
    const std::string& content_hash = file_rows[0][1];

    knowledge_store::KnowledgeStore::Transaction tx(store, "store_file_summary");
    if (tx.begin_failed()) return false;

    // Mark any previous summary for this file as invalidated
    tx.exec(
        "UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') WHERE file_id = ? AND is_valid = 1",
        file_id
    );

    tx.exec(
        "INSERT INTO summaries (file_id, content_hash, summary_text, created_at, is_valid) "
        "VALUES (?, ?, ?, datetime('now'), 1)",
        file_id, content_hash, summary
    );

    if (!tx.commit()) {
        std::cerr << "ai-tools: summary write rolled back (" << file_path
                  << "): " << tx.error() << "\n";
        return false;
    }
    return true;
}

std::string get_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path) {
    auto rows = store.query(
        "SELECT s.summary_text, s.content_hash FROM summaries s "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.path = ? AND s.is_valid = 1 "
        "ORDER BY s.summary_id DESC LIMIT 1",
        repo_id, file_path
    );
    if (rows.empty()) {
        return "";
    }
    // A summary is only valid while it matches the current content hash of the file
    std::string current_hash = content_hasher::compute_file_hash(file_path);
    if (current_hash.empty() || current_hash != rows[0][1]) {
        return "";
    }
    return rows[0][0];
}

bool store_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& summary) {
    // Repository-level summaries are stored with file_id = 0 as a sentinel
    // (DB-01: checked — a failed write must not read as a stored summary).
    if (store.execute(
            "INSERT INTO summaries (file_id, content_hash, summary_text, created_at, is_valid) "
            "VALUES (0, ?, ?, datetime('now'), 1)",
            repo_id, summary) != SQLITE_OK) {
        std::cerr << "ai-tools: repository summary write failed (" << repo_id
                  << "): " << store.get_error() << "\n";
        return false;
    }
    return true;
}

std::string get_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    auto rows = store.query(
        "SELECT summary_text FROM summaries WHERE file_id = 0 AND content_hash = ? AND is_valid = 1 "
        "ORDER BY summary_id DESC LIMIT 1",
        repo_id
    );
    if (rows.empty()) {
        return "";
    }
    return rows[0][0];
}

// ===========================================================================
// SUM-01 — summaries per symbol
// ===========================================================================

namespace {

// On-disk probe for one file, owned by a single public call so a per-file
// inventory verifies all of that file's symbols with ONE hash pass instead of
// one per symbol. Deliberately NOT shared across calls: a summary read must
// describe the bytes as they are now, so a probe kept from an earlier call (one
// that ran before an edit) would certify a summary against content that no
// longer exists. Re-reading once per call is the cost of never serving a stale
// verdict from a cached answer.
struct Probe {
    std::string path;         // the file this probe describes
    std::string current_hash; // empty: the file could not be read/hashed
    std::string content;      // its bytes, read at most once per probe
    bool read = false;
};

// Probe the file for THIS call. Deliberately recomputed on every call: caching
// a hash across calls is exactly how a summary would get certified against
// content an edit already replaced (the IDX-04 rule — freshness is proven now,
// not remembered from an earlier answer). Within one call the Probe object is
// passed to each symbol decision, so a per-file inventory still costs ONE hash
// pass (and at most one content read) for all of that file's symbols.
Probe probe_file(const std::string& path) {
    Probe probe;
    probe.path = path;
    probe.current_hash = content_hasher::compute_file_hash(path);
    return probe;
}

// Decide whether a live symbol row still describes the bytes on disk, and what
// its CURRENT body hash is. `disk` is the caller's probe of the symbol's file,
// hashed once for this call — never carried over from an earlier call.
//
// Two facts make a body hash trustworthy:
//   - files.content_hash equals the hash of the bytes currently on disk: then
//     the stored symbols provably derive from those bytes (IDX-03 writes them
//     from one snapshot, and nothing has changed since), so the stored line
//     range is still the symbol's region;
//   - the row's own body_hash. Post-SUM-01 rows carry it, computed from the
//     very snapshot that produced files.content_hash — no disk read needed.
// A legacy row (body_hash NULL) is completed from disk only when the first
// fact holds, because then the on-disk slice IS the indexed body. If the file
// hash no longer matches, nothing is guessed: the state is kStaleFile, and the
// next re-index fills the column (mirrors IDX-04's rule that freshness must be
// proven by the hash, never presumed from a timestamp).
bool live_body_hash(Probe& disk, symbol_index::SymbolRegion& region,
                    std::string& body_hash, std::string& status) {
    if (disk.current_hash.empty()) {
        status = symbol_status::kStaleFile; // unreadable: nothing can be certified
        return false;
    }
    if (!region.file_content_hash.empty() && region.file_content_hash != disk.current_hash) {
        status = symbol_status::kStaleFile; // the file's index is superseded
        return false;
    }
    if (region.has_body_hash) {
        body_hash = region.body_hash;
        return true;
    }
    // Legacy row (no body_hash stored), with the file hash verified against
    // disk: reading the file now is safe precisely because the hash matched,
    // so these bytes ARE the snapshot the stored line range indexes.
    if (!disk.read) {
        disk.read = true;
        disk.content = file_utils::read_file(region.file_path);
    }
    body_hash = content_hasher::compute_string_hash(
        file_utils::slice_lines(disk.content, static_cast<int>(region.start_line),
                                static_cast<int>(region.end_line)));
    region.has_body_hash = true;
    region.body_hash = body_hash;
    return true;
}

// True when the file has a live (not tombstoned) index record. Distinguishes
// 'this file is not in the index' from 'the file is indexed but has no symbol
// with this name' — and, for a deleted file, keeps its symbols unanswerable
// (the row's is_deleted filter already dropped them).
bool file_is_indexed(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                     const std::string& file_path) {
    auto rows = store.query(
        "SELECT 1 FROM files WHERE repo_id = ? AND path = ? AND is_deleted = 0 LIMIT 1",
        repo_id, file_path);
    if (rows.empty() && store.last_query_failed()) {
        // DB-01: a failed read must not answer 'not indexed'.
        std::cerr << "ai-tools: cannot check index record (" << file_path
                  << "): " << store.get_error() << "\n";
        return false;
    }
    return !rows.empty();
}

// Resolve one symbol inside one file into a single candidate, filling `out`
// with everything a read or a write needs, and setting out.status when the
// question cannot be answered unambiguously. Returns false in that case.
bool resolve_symbol(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::string& file_path, const std::string& symbol,
                    const std::string& symbol_type, Probe& disk,
                    symbol_index::SymbolRegion& region, summary_store::SymbolSummaryInfo& out) {
    out.file_path = file_path;

    auto matches = symbol_index::find_symbol_regions(store, repo_id, file_path, symbol, symbol_type);
    bool read_failed = store.last_query_failed();

    if (matches.size() > 1) {
        // Never pick one silently (PAR-02/PAR-03 discipline): overload, same
        // name in two scopes, or a bare name used in several classes must be
        // resolved by the caller, with the candidates spelled out.
        out.status = symbol_status::kAmbiguous;
        out.candidates = matches;
        out.symbol_key = symbol;
        return false;
    }
    if (matches.empty()) {
        if (read_failed) {
            out.status = symbol_status::kStaleFile;
            out.symbol_key = symbol;
            return false;
        }
        out.status = file_is_indexed(store, repo_id, file_path) ? symbol_status::kNotFound
                                                                : symbol_status::kNoFile;
        out.symbol_key = symbol;
        return false;
    }

    region = matches[0];
    out.file_path = region.file_path;
    out.symbol_key = region.qualified_name;
    out.name = region.name;
    out.symbol_type = region.symbol_type;
    out.scope = region.scope;
    out.start_line = region.start_line;
    out.end_line = region.end_line;
    out.file_content_hash = region.file_content_hash;

    if (!live_body_hash(disk, region, out.body_hash, out.status)) return false;
    out.status = symbol_status::kOk;
    return true;
}

} // namespace

std::string get_symbol_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                               const std::string& file_path, const std::string& symbol,
                               const std::string& symbol_type, SymbolSummaryInfo& out) {
    out = SymbolSummaryInfo{};
    symbol_index::SymbolRegion region;
    Probe disk = probe_file(file_path);
    if (!resolve_symbol(store, repo_id, file_path, symbol, symbol_type, disk, region, out)) return "";

    // Read the most recently written row for this symbol identity, whatever its
    // state: that is what separates 'no summary was ever stored' (kNone) from
    // 'a summary exists but describes different bytes' (kStaleBody).
    auto rows = store.query(
        "SELECT summary_text, body_hash, is_valid, model_version, prompt_version, "
        "verification_state, created_at FROM symbol_summaries "
        "WHERE file_id = ? AND symbol_key = ? AND symbol_type = ? "
        "ORDER BY symbol_summary_id DESC LIMIT 1",
        region.file_id, out.symbol_key, out.symbol_type);
    if (rows.empty() && store.last_query_failed()) {
        out.status = symbol_status::kStaleFile;
        return "";
    }
    if (rows.empty()) {
        out.status = symbol_status::kNone;
        return "";
    }

    const auto& row = rows[0];
    out.has_summary_row = true;
    out.model_version = row.size() > 3 ? row[3] : "";
    out.prompt_version = row.size() > 4 ? row[4] : "";
    out.verification_state = row.size() > 5 ? row[5] : "";
    out.created_at = row.size() > 6 ? row[6] : "";

    const bool valid = row.size() > 2 && row[2] == "1";
    const bool hash_matches = row.size() > 1 && row[1] == out.body_hash;
    if (!valid || !hash_matches) {
        // Double check, same shape as get_file_summary(): the flag says nobody
        // superseded it, the hash says it still describes these exact bytes.
        out.status = symbol_status::kStaleBody;
        return "";
    }

    out.status = symbol_status::kOk;
    return row[0];
}

bool store_symbol_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                          const std::string& file_path, const std::string& symbol,
                          const std::string& symbol_type, const std::string& summary,
                          const std::string& model_version, const std::string& prompt_version,
                          SymbolSummaryInfo& out) {
    if (summary.empty()) {
        out = SymbolSummaryInfo{};
        out.file_path = file_path;
        out.symbol_key = symbol;
        out.status = symbol_status::kNotFound;
        std::cerr << "ai-tools: refusing to store an empty symbol summary (" << file_path
                  << " :: " << symbol << ")\n";
        return false;
    }

    out = SymbolSummaryInfo{};
    symbol_index::SymbolRegion region;
    Probe disk = probe_file(file_path);
    if (!resolve_symbol(store, repo_id, file_path, symbol, symbol_type, disk, region, out)) {
        // Nothing was written: the reason (ambiguous symbol, unindexed symbol,
        // superseded file index) is already in out.status for the caller to
        // report (DB-01: false means 'nothing applied', never 'stored').
        return false;
    }

    // Invalidation of the superseded summary and insertion of the new one are
    // ONE transaction (DB-01, same reasoning as store_file_summary): a failing
    // INSERT must not leave the symbol without its previous summary.
    knowledge_store::KnowledgeStore::Transaction tx(store, "store_symbol_summary");
    if (tx.begin_failed()) {
        out.status = symbol_status::kStaleFile;
        std::cerr << "ai-tools: cannot open summary transaction (" << file_path
                  << " :: " << out.symbol_key << "): " << tx.error() << "\n";
        return false;
    }

    tx.exec(
        "UPDATE symbol_summaries SET is_valid = 0, invalidated_at = datetime('now') "
        "WHERE file_id = ? AND symbol_key = ? AND symbol_type = ? AND is_valid = 1",
        region.file_id, out.symbol_key, out.symbol_type);

    // The identity plus body hash is unique, so re-storing the same text for
    // the same bytes updates the row in place instead of stacking an identical
    // duplicate: re-summarizing unchanged code is idempotent, and the summary
    // stays valid through it.
    tx.exec(
        "INSERT INTO symbol_summaries (file_id, symbol_key, symbol_type, body_hash, start_line, "
        "end_line, summary_text, model_version, prompt_version, verification_state, created_at, is_valid) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), 1) "
        "ON CONFLICT(file_id, symbol_key, symbol_type, body_hash) DO UPDATE SET "
        "summary_text = excluded.summary_text, "
        "start_line = excluded.start_line, end_line = excluded.end_line, "
        "model_version = excluded.model_version, prompt_version = excluded.prompt_version, "
        "verification_state = excluded.verification_state, "
        "created_at = excluded.created_at, invalidated_at = NULL, is_valid = 1",
        region.file_id, out.symbol_key, out.symbol_type, out.body_hash,
        region.start_line, region.end_line, summary, model_version, prompt_version,
        "unverified");

    if (!tx.commit()) {
        out.status = symbol_status::kStaleFile;
        std::cerr << "ai-tools: symbol summary write rolled back (" << file_path
                  << " :: " << out.symbol_key << "): " << tx.error() << "\n";
        return false;
    }

    out.model_version = model_version;
    out.prompt_version = prompt_version;
    out.verification_state = "unverified";
    out.has_summary_row = true;
    out.status = symbol_status::kOk;
    return true;
}

std::vector<SymbolSummaryRow> list_symbol_summaries(knowledge_store::KnowledgeStore& store,
                                                    const std::string& repo_id,
                                                    const std::string& file_path) {
    std::vector<SymbolSummaryRow> out;
    auto regions = symbol_index::list_file_symbol_regions(store, repo_id, file_path);
    if (regions.empty()) {
        if (store.last_query_failed()) {
            std::cerr << "ai-tools: cannot list symbols for summary inventory (" << file_path
                      << "): " << store.get_error() << "\n";
        }
        return out; // DB-01: caller distinguishes 'no symbols' from 'read failed'
    }

    // One read of the file's stored summaries, matched in memory afterwards:
    // an inventory must not cost one query per symbol. Rows come newest first,
    // so the first row seen for an identity is its most recent version and
    // later duplicates of the same identity are skipped.
    auto rows = store.query(
        "SELECT ss.symbol_key, ss.symbol_type, ss.body_hash, ss.summary_text, ss.model_version, "
        "ss.prompt_version, ss.verification_state, ss.created_at, ss.is_valid FROM symbol_summaries ss "
        "WHERE ss.file_id = ? ORDER BY ss.symbol_summary_id DESC",
        regions[0].file_id);
    if (rows.empty() && store.last_query_failed()) {
        std::cerr << "ai-tools: cannot read symbol summaries (" << file_path
                  << "): " << store.get_error() << "\n";
        return out;
    }

    struct Entry {
        bool taken = false;
        bool stored_valid = false;
        std::string body_hash;
        std::string summary_text;
        std::string model_version;
        std::string prompt_version;
        std::string verification_state;
        std::string created_at;
    };
    std::unordered_map<std::string, Entry> by_symbol;
    for (const auto& row : rows) {
        if (row.size() < 9) continue;
        std::string key = row[0] + "\x1f" + row[1]; // symbol_key \x1f symbol_type
        Entry& entry = by_symbol[key];
        if (entry.taken) continue; // newest row per identity wins
        entry.taken = true;
        entry.body_hash = row[2];
        entry.summary_text = row[3];
        entry.model_version = row[4];
        entry.prompt_version = row[5];
        entry.verification_state = row[6];
        entry.created_at = row[7];
        entry.stored_valid = (row[8] == "1");
    }

    out.reserve(regions.size());
    Probe disk = probe_file(file_path);
    for (auto& region : regions) {
        SymbolSummaryRow row;
        row.region = region;
        std::string live_hash;
        std::string unused_status;
        if (live_body_hash(disk, region, live_hash, unused_status)) {
            row.region.has_body_hash = true;
            row.region.body_hash = live_hash;
        } else {
            // The symbol's bytes cannot be certified today (its file's index is
            // superseded or unreadable): the inventory reports the symbol with
            // no summary verdict rather than guessing one.
            row.region.has_body_hash = false;
            row.region.body_hash.clear();
        }
        out.push_back(row);
    }

    // Same double check get_symbol_summary() applies: the flag, plus a stored
    // body_hash equal to the live symbol's. A symbol whose body is unchanged by
    // an edit elsewhere in the file keeps its summary (that is SUM-01's whole
    // point); one whose body changed matches nothing and is reported stale.
    for (auto& row : out) {
        auto it = by_symbol.find(row.region.qualified_name + "\x1f" + row.region.symbol_type);
        if (it == by_symbol.end()) continue;
        const Entry& entry = it->second;
        const bool match = entry.stored_valid && row.region.has_body_hash &&
                           entry.body_hash == row.region.body_hash;
        if (match) {
            row.has_valid_summary = true;
            row.summary_text = entry.summary_text;
        } else {
            row.has_stale_summary = true;
        }
        row.model_version = entry.model_version;
        row.prompt_version = entry.prompt_version;
        row.verification_state = entry.verification_state;
        row.created_at = entry.created_at;
    }

    return out;
}

std::vector<SymbolSummaryRow> list_repo_symbol_summaries(knowledge_store::KnowledgeStore& store,
                                                         const std::string& repo_id) {
    std::vector<SymbolSummaryRow> out;

    // One join answers the whole question: a stored summary counts only while a
    // LIVE symbol row carries the same identity AND the same body hash, so a
    // summary of a deleted symbol (its symbol rows are dropped), of a tombstoned
    // file, or of a superseded body never comes back as a match. The newest row
    // per identity wins: that is the one a re-index would have revived.
    auto rows = store.query(
        "SELECT f.path, s.name, s.qualified_name, s.symbol_type, s.scope, s.start_line, s.end_line, "
        "s.body_hash, s.file_id, f.content_hash, "
        "ss.body_hash, ss.summary_text, ss.model_version, ss.prompt_version, "
        "ss.verification_state, ss.created_at, ss.is_valid "
        "FROM symbol_summaries ss "
        "JOIN symbols s ON s.file_id = ss.file_id AND s.qualified_name = ss.symbol_key "
        "AND s.symbol_type = ss.symbol_type AND s.body_hash = ss.body_hash "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.is_deleted = 0 AND s.qualified_name IS NOT NULL "
        "ORDER BY f.path, s.start_line, ss.symbol_summary_id DESC",
        repo_id);
    if (rows.empty()) {
        if (store.last_query_failed()) {
            std::cerr << "ai-tools: cannot read the repository symbol summary inventory (" << repo_id
                      << "): " << store.get_error() << "\n";
        }
        return out; // DB-01: 'nothing cached' and 'the read failed' stay distinct
    }

    std::unordered_set<std::string> seen;
    for (const auto& row : rows) {
        if (row.size() < 17) continue;
        std::string key = row[0] + "\x1f" + row[2] + "\x1f" + row[3]; // path \x1f qualified \x1f type
        if (!seen.insert(key).second) continue;

        SymbolSummaryRow r;
        try {
            r.region.file_id = std::stoll(row[8]);
            r.region.start_line = std::stoll(row[5]);
            r.region.end_line = std::stoll(row[6]);
        } catch (...) {
            continue; // malformed row: report nothing rather than a wrong position
        }
        r.region.file_path = row[0];
        r.region.name = row[1];
        r.region.qualified_name = row[2];
        r.region.symbol_type = row[3];
        r.region.scope = row[4];
        r.region.has_body_hash = !row[7].empty();
        r.region.body_hash = row[7];
        r.region.file_content_hash = row[9];
        // has_stale_summary separates a matching-but-invalidated row (a re-index
        // ran and did not revive it) from a fully usable summary.
        if (row[16] == "1") {
            r.has_valid_summary = true;
            r.summary_text = row[11];
        } else {
            r.has_stale_summary = true;
        }
        r.model_version = row[12];
        r.prompt_version = row[13];
        r.verification_state = row[14];
        r.created_at = row[15];
        out.push_back(r);
    }
    return out;
}

}

