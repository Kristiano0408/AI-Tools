# 06 — Build, run, test, troubleshooting

## Prerequisiti

| Dipendenza | Versione | nota |
|---|---|---|
| Compilatore C++23 | gcc 13+ (testato con gcc 15) | servono concepts, `std::filesystem` |
| CMake | 3.16+ | |
| libsqlite3 dev | qualunque recente | via pkg-config |
| libssl dev (OpenSSL) | qualunque recente | per SHA-256 |
| pkg-config | | usato da CMake |

Librerie vendorizzate in `third_party/` (nessun download):
- `third_party/tree-sitter/` — runtime tree-sitter statico + header
- `third_party/grammars/{tree-sitter-c,tree-sitter-cpp,tree-sitter-python}/src/` — `parser.c` (e `scanner.c` dove serve) pregenerati
- `third_party/usr/include/` — nlohmann/json
- `third_party/ankerl-unordered_dense/` — mappe hash header-only

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Target prodotti:
- `build/ai-tools` — il server MCP
- `build/run_tests` — la suite di test aggregata

Target extra: `tree-sitter-c`, `tree-sitter-cpp`, `tree-sitter-python` (librerie statiche delle grammars).

Flag attivi: `-Wall -Wextra -Wpedantic -O2` (Release `-O3`, Debug `-g`).

## Run

```bash
./build/ai-tools        # aspetta JSON-RPC su stdin, risponde su stdout
```

Smoke test (una sessione completa):

```bash
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
 '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"index","arguments":{"path":"/home/franco/Documents/GitHub/AI-Tools"}}}' \
 | ./build/ai-tools
```

Atteso: risposta `initialize`, niente per la notifica, array di 11 tool, poi `{files_indexed:N, files_total:M, repo_id:"<16hex>", status:"indexed"}`.

Configurazione come server MCP in un client (es. VS Code / Claude Desktop): comando `./build/ai-tools`, trasporto stdio.

## Test

```bash
cmake --build build          # build di run_tests
./build/run_tests            # output PASS/FAIL per asserzione
# oppure
ctest --test-dir build --output-on-failure
```

Suite (registrata in CMake come test `unit_tests`):

| Suite | Copre |
|---|---|
| `run_parser_tests()` (tests/test_parsers.cpp) | estrazione simboli C/C++/CUDA/Python, declaration vs definition, scope, include, call edges, degradazione, end-to-end indexer→store→query |
| `run_store_tests()` (tests/test_store.cpp) | hasher (blocco finale, file grande), store (params, errori), identità repository, isolamento, cache invalidation transitiva, stale detection, re-index incrementale; DEP-01/02 (catene di include transitive, safety cap); **DEP-03** (T-17..T-22): propagazione esterna via `index_file`, ciclo mutuo A↔B, stabilizzazione della full scan su dipendenti visitati fuori ordine, call edge esclusi dalla propagazione file-level |
| `run_snapshot_consistency_tests()` (tests/test_snapshot_consistency.cpp) | IDX-03/T-12: `index_snapshot` scrisce hash+simboli+size dallo stesso buffer; re-indicizzo coerente; hash-driven summary revalidation; writer thread che scambia versioni durante `index_file` (nessun accoppiamento hash/simboli di versioni diverse) |
| `run_db_transaction_tests()` (tests/test_db_transactions.cpp) | DB-01/T-08: errori in `index_snapshot` e `COMMIT`, rollback del vecchio indice, invalidazione cache atomica, scansione fallita senza copertura certificata, savepoint annidati, summary atomiche e fault hook |
| `run_symbol_summary_tests()` (tests/test_symbol_summaries.cpp) | SUM-01/T-06: `symbol_summaries` — riassunti indipendenti per simboli dello stesso file, modifica confinata che preserva la sorella, revival hash-driven di un rollback, reindicizzazione invariata che non invalida nulla, ambiguità con candidati (lettura e scrittura), cancellazione di un simbolo, catena header→includer (propagazione DEP-03 + revival + reap di `CacheManager::invalidate`), tombstone IDX-02, hash mai assunto (righe pre-SUM-01 decise dal hash, `body_hash_verified:false`), inventario per-file e repo-wide, rollback DB-01 di una scrittura e di una reindicizzazione (transazione intera, summary intatte), migrazione di un DB pre-SUM-01 (colonne + backfill `qualified_name` per linguaggio, `body_hash` mai backfillato) |
| `run_tools_tests()` (tests/test_tools.cpp) | tutti gli 11 tool via `tools::call_tool`, boundary enforcement, execute, ricerca cross-repo, symbol call graph, interfaccia Resources (`list`/`read`/`templates_list`, errori -32002/-32602, binary reject, traversal, paginazione cursor), server JSON-RPC; **DEP-03 MCP**: modifica esterna di un header → index mono-file riporta `dependents_invalidated` + hint di copertura, self-heal di `file_content` propaga, `summary` propaga, full scan assesta la catena (0 stale, `index_full`), `symbol_search` trova il simbolo nuovo; **DB-01**: envelope MCP di scansione fallita con causa e copertura non completa; **SUM-01 MCP** (`symbol_summary`): inventario per-file, ambiguità con candidati (lettura e scrittura rifiutata), round-trip qualificato con provenienza e body hash, stati onesti (`no_summary` vs `symbol_not_indexed` vs `stale_symbol_body` con righe live), lista repo-wide attraverso il gate IDX-01, limit/offset, confine path, scritture senza symbol rifiutate |

Exit code = numero di asserzioni fallite (0 = tutto verde). Output atteso finale: `ALL TESTS PASSED`.

Isolamento: `run_store_tests`/`run_tools_tests` reimpostano `HOME` su una directory temporanea → non toccano `~/.ai-tools` reale.

## Performance note (misurata su questo repo, 903 file, 37 sorgenti)

| Operazione | Tempo |
|---|---|
| Full index cold | ~2.0 s |
| Re-index incrementale (tutto fresco) | ~0.2 s |
| Suite test completa | < 1 s |

## Troubleshooting

| Sintomo | Causa probabile | Rimedio |
|---|---|---|
| `undefined reference to main` linkando run_tests | `tests/test_main.cpp` non presente al momento della prima configure | `cmake -S . -B build` di nuovo (i test sono GLOB) |
| Tool risponde "Unknown tool" | nome tool errato o typo | `tools/list` mostra i 10 nomi esatti |
| "Path escapes repository boundary" | path fuori dalla root del repo | usare path relativi alla root o assoluti interni |
| `index_status:"stale"` persistente | file modificato ma non re-indicizzato | chiamare `index` con `file_path` o `edit` (re-indicizza da solo) |
| Call graph vuoto per un simbolo | file non ri-indicizzato dopo l'aggiunta della feature call edges | `index` full del repo |
| DB "corrotto"/comportamento strano dopo upgrade | schema legacy con `repo_id` INTEGER | cancellare `~/.ai-tools` e ri-indicizzare |
| ABI mismatch tree-sitter (metadati base, nessun simbolo) | grammars incompatibili col runtime | verificare che `third_party/grammars` siano coerenzi con `libtree-sitter.a` |
| Server non risponde | client usa framing Content-Length con header errati | il server accetta entrambi i framing; verificare `Content-Length` esatto in byte |

## Limitazioni note (vedi anche README root)

1. Call graph per nome (non risolve overload, dispatch virtuale, function pointer).
2. CUDA launch `<<<grid,block>>>` non genera call edge.
3. `#ifdef` indicizzati ma non valutati.
4. Template deduction non eseguita.
5. `is_exported` non popolato.
6. Ricerca non full-text sul contenuto.
7. Python via tree-sitter (non AST stdlib).
