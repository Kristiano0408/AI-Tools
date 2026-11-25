// Tests for IDX-03: hash and parsing must always describe the SAME version of
// a file. index_snapshot() derives content_hash, symbols, dependencies, size
// and line count from one byte buffer; index_file() reads the file exactly
// once and forwards that buffer. A concurrent writer must never be able to
// make the stored hash pair with symbols from a different version.

#include "engine/indexer.h"
#include "engine/cache_manager.h"
#include "engine/content_hasher.h"
#include "engine/knowledge_store.h"
#include "engine/parser.h"
#include "engine/summary_store.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  PASS: " << what << "\n";
    } else {
        std::cout << "  FAIL: " << what << "\n";
        ++failures;
    }
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// Stored record for a file path, as index_snapshot/index_file wrote it.
struct StoredFile {
    bool found = false;
    std::string content_hash;
    long long size_bytes = -1;
    int line_count = -1;
};

static StoredFile stored_file(knowledge_store::KnowledgeStore& store,
                              const std::string& repo_id, const std::string& path) {
    StoredFile s;
    auto rows = store.query(
        "SELECT content_hash, size_bytes, line_count FROM files WHERE repo_id = ? AND path = ?",
        repo_id, path);
    if (rows.empty() || rows[0].size() < 3) return s;
    s.found = true;
    s.content_hash = rows[0][0];
    try { s.size_bytes = std::stoll(rows[0][1]); } catch (...) {}
    try { s.line_count = std::stoi(rows[0][2]); } catch (...) {}
    return s;
}

static std::set<std::string> stored_symbols(knowledge_store::KnowledgeStore& store,
                                            const std::string& repo_id, const std::string& path) {
    std::set<std::string> names;
    auto rows = store.query(
        "SELECT name FROM symbols WHERE file_id IN "
        "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
        repo_id, path);
    for (const auto& r : rows) if (!r.empty()) names.insert(r[0]);
    return names;
}

// Every stored symbol must carry the file hash the snapshot indexing wrote.
static bool all_symbols_hashed_with(knowledge_store::KnowledgeStore& store,
                                    const std::string& repo_id, const std::string& path,
                                    const std::string& hash) {
    auto mismatches = store.query(
        "SELECT COUNT(*) FROM symbols WHERE file_id IN "
        "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) "
        "AND (content_hash_at_index IS NULL OR content_hash_at_index != ?)",
        repo_id, path, hash);
    if (mismatches.empty() || mismatches[0].empty()) return false;
    return mismatches[0][0] == "0";
}

// A and B are distinct C++ versions: one unique symbol each, plus one shared
// symbol. Any A/B mix in the stored knowledge is visible as both uniques
// appearing together, or a unique appearing with the other version's hash.
static const char* kContentA =
    "int alpha_only_A() { return 1; }\n"
    "\n"
    "int common_fn() { return 2; }\n";

static const char* kContentB =
    "int bravo_only_B() { return 3; }\n"
    "\n"
    "int common_fn() { return 4; }\n";

// Count text lines the way fill_basic_metadata does: every '\n' plus one when
// the content has no trailing newline.
static int count_lines(const std::string& content) {
    int n = 0;
    for (char c : content) if (c == '\n') ++n;
    if (!content.empty() && content.back() != '\n') ++n;
    return n;
}

int run_snapshot_consistency_tests() {
    failures = 0;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "ai-tools-snapshot-tests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const std::string repo_id = "repo-snap";

    // ---- Invariant test: the DB snapshot is exactly what index_snapshot was given ----
    std::cout << "[snapshot invariant: hash and symbols derive from one buffer]\n";
    {
        knowledge_store::KnowledgeStore store;
        check(store.open((tmp / "invariant.db").string()), "store opens");

        std::string path = (tmp / "unit.cpp").string();
        const std::string content_a = kContentA;
        const std::string content_b = kContentB;

        // A concurrent version (B) is what the file on disk holds while we
        // index from the snapshot (A). No path in index_snapshot reopens this
        // file, so every stored artifact must describe A.
        write_file(path, content_b);
        indexer::index_snapshot(store, path, repo_id, content_a);

        const std::string hash_a = content_hasher::compute_string_hash(content_a);
        const std::string hash_b = content_hasher::compute_string_hash(content_b);
        check(hash_a != hash_b, "test fixture versions A and B differ");

        StoredFile stored = stored_file(store, repo_id, path);
        check(stored.found, "file record written");
        check(stored.content_hash == hash_a, "files.content_hash is the snapshot's hash (A)");
        check(stored.size_bytes == static_cast<long long>(std::string(kContentA).size()),
              "size_bytes describes the snapshot, not a stat of the file on disk");
        check(stored.line_count == count_lines(content_a),
              "line_count derives from the snapshot");

        std::set<std::string> expected_a = {"alpha_only_A", "common_fn"};
        check(stored_symbols(store, repo_id, path) == expected_a,
              "stored symbols are A's symbols only (B's symbol not resurrected)");
        check(all_symbols_hashed_with(store, repo_id, path, hash_a),
              "every symbols.content_hash_at_index equals the stored file hash");

        // The stored knowledge is A, while the disk is B: the index must
        // expose this as needing re-indexing, and hand back B's hash so a
        // scan loop can reuse it instead of reading the file twice.
        std::string out_hash;
        check(indexer::needs_indexing(store, path, repo_id, &out_hash),
              "snapshot indexing of A while B is on disk is detected as stale");
        check(out_hash == hash_b, "needs_indexing reports the on-disk version's hash");

        // Re-indexing from disk must land on a fully consistent B: hash,
        // symbols and size all describe B (no A/B mix left behind).
        indexer::index_file(store, path, repo_id);
        stored = stored_file(store, repo_id, path);
        check(stored.content_hash == hash_b, "after index_file the stored hash is B");
        std::set<std::string> expected_b = {"bravo_only_B", "common_fn"};
        check(stored_symbols(store, repo_id, path) == expected_b,
              "after index_file the stored symbols are B's symbols only");
        check(all_symbols_hashed_with(store, repo_id, path, hash_b),
              "re-index leaves no symbol tagged with the superseded hash");
        check(stored.size_bytes == static_cast<long long>(std::string(kContentB).size()),
              "size_bytes follows the newly indexed snapshot");
        check(!indexer::needs_indexing(store, path, repo_id), "file fresh after consistent re-index");

        // Summaries attach to the indexed content hash (which came from the
        // snapshot), not to whatever the disk happened to hold.
        indexer::index_snapshot(store, path, repo_id, content_a);
        summary_store::store_file_summary(store, repo_id, path, "summary of A");
        auto sum_rows = store.query(
            "SELECT content_hash FROM summaries WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND is_valid = 1",
            repo_id, path);
        check(!sum_rows.empty() && sum_rows[0][0] == hash_a,
              "summary attaches to the snapshot's hash");
        check(summary_store::get_file_summary(store, repo_id, path).empty(),
              "summary reports stale: disk holds B, summary describes A");

        // With disk and index on the same version, the summary is valid again.
        indexer::index_file(store, path, repo_id); // disk version B
        summary_store::store_file_summary(store, repo_id, path, "summary of B");
        check(summary_store::get_file_summary(store, repo_id, path) == "summary of B",
              "summary valid when index and disk agree on one version");

        // Hash-driven revalidation: index_snapshot revalidates a stored
        // summary when the exact snapshot it was stored against reappears.
        std::string content_c = "int shared_only_C() { return 7; }\n";
        const std::string hash_c = content_hasher::compute_string_hash(content_c);
        indexer::index_snapshot(store, path, repo_id, content_c);
        summary_store::store_file_summary(store, repo_id, path, "summary of C");
        {
            cache_manager::CacheManager cache(store);
            cache.invalidate(path);
        }
        auto count_valid_c = [&]() -> std::string {
            auto rows = store.query(
                "SELECT COUNT(*) FROM summaries WHERE file_id IN "
                "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) "
                "AND content_hash = ? AND is_valid = 1",
                repo_id, path, hash_c);
            return rows.empty() || rows[0].empty() ? "?" : rows[0][0];
        };
        check(count_valid_c() == "0", "invalidation marks the stored summary invalid");
        indexer::index_snapshot(store, path, repo_id, content_c); // same snapshot returns
        check(count_valid_c() == "1",
              "hash-driven revalidation revives the summary of the reindexed snapshot");
    }

    // ---- Concurrency smoke test: an external writer swaps versions during indexing ----
    std::cout << "[concurrent modification during indexing]\n";
    {
        knowledge_store::KnowledgeStore store;
        check(store.open((tmp / "concurrent.db").string()), "store opens");

        std::string path = (tmp / "hot.cpp").string();
        std::string stage = path + ".stage"; // writer stages here, renames atomically
        write_file(path, kContentA);

        const std::string hash_a = content_hasher::compute_string_hash(kContentA);
        const std::string hash_b = content_hasher::compute_string_hash(kContentB);
        const std::set<std::string> expected_a = {"alpha_only_A", "common_fn"};
        const std::set<std::string> expected_b = {"bravo_only_B", "common_fn"};

        // The writer publishes whole versions via rename (atomic within the
        // filesystem), so a reader always observes either A or B in full —
        // exactly the condition the invariant has to survive.
        std::atomic<bool> stop{false};
        std::atomic<unsigned long> published{0};
        std::thread writer([&] {
            bool toggle = false;
            while (!stop.load()) {
                const std::string& next = toggle ? kContentB : kContentA;
                {
                    std::ofstream f(stage);
                    f << next;
                }
                std::error_code ec;
                fs::rename(stage, path, ec);
                if (!ec) published.fetch_add(1);
                toggle = !toggle;
                std::this_thread::yield();
            }
        });

        int mismatches = 0;
        int iterations = 0;
        unsigned long hash_transitions = 0;
        std::string last_hash;
        bool saw_a = false, saw_b = false;

        const int kIterations = 400;
        for (int i = 0; i < kIterations; ++i) {
            indexer::index_file(store, path, repo_id);
            ++iterations;

            StoredFile stored = stored_file(store, repo_id, path);
            std::set<std::string> symbols = stored_symbols(store, repo_id, path);

            bool consistent = false;
            if (stored.content_hash == hash_a) {
                consistent = (symbols == expected_a) &&
                             all_symbols_hashed_with(store, repo_id, path, hash_a) &&
                             stored.size_bytes == static_cast<long long>(std::string(kContentA).size());
                saw_a = true;
            } else if (stored.content_hash == hash_b) {
                consistent = (symbols == expected_b) &&
                             all_symbols_hashed_with(store, repo_id, path, hash_b) &&
                             stored.size_bytes == static_cast<long long>(std::string(kContentB).size());
                saw_b = true;
            }
            if (!consistent) {
                ++mismatches;
                std::cerr << "  mismatch at iteration " << i
                          << ": stored hash=" << stored.content_hash
                          << " symbols=" << symbols.size() << "\n";
            }

            if (!last_hash.empty() && stored.content_hash != last_hash) ++hash_transitions;
            last_hash = stored.content_hash;
        }

        stop.store(true);
        writer.join();

        check(mismatches == 0, "no iteration ever stored a hash paired with another version's symbols");
        check(saw_a && saw_b, "both versions were indexed while the writer kept swapping the file");
        check(hash_transitions > 0, "race window exercised: the indexed version changed across runs");
        check(published.load() > 0, "the writer really republished the file during indexing");
    }

    fs::remove_all(tmp);

    std::cout << (failures == 0 ? "\n[snapshot consistency tests] ALL PASSED\n"
                                : "\n[snapshot consistency tests] " +
                                      std::to_string(failures) + " FAILURES\n");
    return failures;
}
