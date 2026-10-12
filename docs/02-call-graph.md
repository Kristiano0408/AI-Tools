# 02 — Call Graph: da `main()` fino in fondo

Questo documento è la mappa di navigazione del codice. Ogni albero mostra **chi chiama chi**, livello per livello, fino alle funzioni foglia (quelle che non chiamano altro codice del progetto). Linee verticali = chiamata diretta. `→ valore` indica cosa ritorna la foglia.

Legenda:
- `ns::funzione()` — chiamata; il namespace dice in che file si trova
- `[foglia]` — non chiama altro codice interno
- `⟲` — chiamata ricorsiva
- `(cond)` — chiamata condizionale

---

## 1. Vista d'insieme (livello 0)

```
main()                                    src/main.cpp
├── server::init()                        src/server/server.cpp        [foglia]
└── server::run()                         src/server/server.cpp
    └── server::handle_request()          (una volta per messaggio in stdin)
        ├── server::make_result()         [foglia]
        ├── server::make_error()          [foglia]
        ├── tools::list_tools()           [foglia: costruisce JSON statico]
        └── tools::call_tool()            ← il vero cuore del sistema
            ├── repository::lookup_repository() / get_or_create_repository()
            ├── (per ogni tool) handler inline in call_tool
            │   ├── indexer::*            (index)
            │   ├── parser::*             (tramite indexer)
            │   ├── symbol_index::*       (search/symbol_search)
            │   ├── dependency_graph::*   (dependency_graph)
            │   ├── summary_store::*      (summary)
            │   ├── cache_manager::*      (edit)
            │   ├── file_utils::* / path_utils::*
            │   └── knowledge_store::KnowledgeStore (query/execute)
            └── tools::tool_result() / tool_error()   [foglia]
```

---

## 2. Il trasporto: `main → run → handle_request`

```
main()
├── server::init()                        [foglia] — hook di inizializzazione (oggi vuoto)
└── server::run()
    ├── std::getline(std::cin, line)      loop: una riga = un messaggio
    ├── (se riga inizia per "Content-Length:")
    │   ├── std::stoi(...)                parsing header
    │   ├── std::getline(...)             scarta header fino a riga vuota
    │   └── std::cin.read(body, N)        legge body esatto
    ├── server::handle_request(request)
    │   ├── json::parse(request)          [foglia]
    │   ├── method == "initialize"
    │   │   └── server::make_result(result, id)      [foglia]
    │   ├── method == "notifications/initialized" → ""   [nessuna risposta]
    │   ├── method == "ping" → make_result({}, id)
    │   ├── method == "tools/list"
    │   │   ├── tools::list_tools()       [foglia] ritorna JSON array di 10 tool
    │   │   └── make_result({tools: [...]}, id)
    │   ├── method == "tools/call"
    │   │   ├── tools::call_tool(name, arguments)     → vedi §4
    │   │   └── make_result(parsed, id)
    │   └── altrimenti → make_error(-32601, ...)
    └── std::cout << response << "\n"     (solo se risposta non vuota)
```

`handle_request` cattura **ogni** `std::exception` e ritorna `make_error(-32600, ...)` — il server non crasha mai su input malformato.

---

## 3. Risoluzione repository (comune a quasi tutti i tool)

```
tools::call_tool()
└── tools::resolve_repo(arguments, store)          [helper anonimo in tools.cpp]
    ├── (se arguments.repo_id)
    │   ├── repository::lookup_repository(repo_id)
    │   │   ├── knowledge_store::KnowledgeStore::open(registry.db)
    │   │   │   └── KnowledgeStore::init_schema()
    │   │   │       └── KnowledgeStore::execute(...) ×N   (CREATE TABLE/INDEX IF NOT EXISTS)
    │   │   ├── KnowledgeStore::query("SELECT ... WHERE repo_id = ?")
    │   │   └── repository::get_db_path(root_path)
    │   │       └── repository::repo_identity(root_path)
    │   │           ├── path_utils::normalize_path()      [foglia]
    │   │           ├── std::filesystem::absolute()        [foglia]
    │   │           └── content_hasher::compute_string_hash()
    │   │               └── content_hasher::to_hex()      [foglia]
    │   └── KnowledgeStore::open(info.db_path)            (apre il DB del repo)
    └── (altrimenti, se arguments.path)
        ├── std::filesystem::is_directory(path)           [foglia]
        ├── repository::get_or_create_repository(root)
        │   ├── path_utils::normalize_path(absolute(root))
        │   ├── repository::repo_identity(canonical)
        │   ├── repository::get_db_path(canonical)
        │   ├── KnowledgeStore::open(db_path)             crea il DB del repo
        │   │   └── init_schema()
        │   ├── (replica identità nel DB del repo: INSERT OR IGNORE repositories)
        │   ├── KnowledgeStore::open(registry.db)         registry globale
        │   ├── registry.query(...) → (esistente? UPDATE updated_at)
        │   └── (nuovo? registry INSERT) → RepoInfo
        └── KnowledgeStore::open(info.db_path)
```

**Invarianti**: dopo `resolve_repo`, `store` è il database **del repository corretto** e `repo_id` è non vuoto. Se la risoluzione fallisce, i tool che richiedono repo rispondono `tool_error`.

---

## 4. Tool `index` → il cammino di indicizzazione completo

```
tools::call_tool("index", args)
├── (con file_path)
│   ├── tools::guarded_path(repo, file_path)
│   │   ├── std::filesystem::weakly_canonical(candidate)  [foglia]
│   │   └── path_utils::is_within_repository(root, abs)   [foglia]
│   └── indexer::index_file(store, abs_path, repo_id)
│       ├── path_utils::get_extension(path)               [foglia]
│       ├── parser::get_language_from_extension(ext)      [foglia]
│       ├── (unknown → return, file saltato)
│       ├── parser::parse_file(path, language)            → vedi §6
│       ├── content_hasher::compute_file_hash(path)
│       │   └── content_hasher::to_hex()                 [foglia]
│       ├── (hash vuoto → return, file illeggibile)
│       ├── std::filesystem::file_size(path, ec)          [foglia]
│       ├── KnowledgeStore::execute("BEGIN")
│       ├── KnowledgeStore::execute(INSERT OR REPLACE files ...)
│       │   └── KnowledgeStore::prepare_cached(sql)       [foglia sqlite]
│       ├── KnowledgeStore::query("SELECT file_id ...")
│       ├── KnowledgeStore::execute("DELETE FROM symbols WHERE file_id = ?")
│       ├── per ogni simbolo: KnowledgeStore::execute(INSERT symbols ...)
│       ├── KnowledgeStore::execute("DELETE FROM dependencies ... source_path = ?")
│       ├── per ogni dipendenza file:
│       │   ├── std::filesystem::weakly_canonical(candidate)   risoluzione a file reale
│       │   ├── std::filesystem::exists / is_regular_file      [foglia]
│       │   └── KnowledgeStore::execute(INSERT dependencies dep_type='include'|'import')
│       ├── KnowledgeStore::execute("DELETE ... dep_type='call'")
│       ├── per ogni SymbolDependency: KnowledgeStore::execute(INSERT dependencies dep_type='call', source_symbol, target_symbol)
│       ├── KnowledgeStore::execute(UPSERT indexing_state ...)
│       └── KnowledgeStore::execute("COMMIT")
└── (senza file_path)
    └── indexer::index_repository(store, repo_id)
        ├── KnowledgeStore::query("SELECT root_path FROM repositories ...")
        ├── std::filesystem::recursive_directory_iterator(root)   [foglia]
        ├── per entry:
        │   ├── entry.is_regular_file()                            [foglia]
        │   ├── path_utils::get_extension(path)                    [foglia]
        │   ├── (skip build/ .git/ node_modules/ third_party/)
        │   ├── indexer::needs_indexing(store, path, repo_id)
        │   │   ├── KnowledgeStore::query("SELECT content_hash FROM files ...")
        │   │   └── content_hasher::compute_file_hash(path) → to_hex()
        │   ├── (fresco → skip)                                    ← incrementale
        │   └── indexer::index_file(store, path, repo_id)          → ramo sopra
        └── KnowledgeStore::execute(UPSERT indexing_state status='full', contatori)
```

---

## 5. Parsing: `parse_file` fino all'AST

```
parser::parse_file(path, language)
├── language "c"|"cpp"  → parser::parse_cpp(path)
│   ├── parser::detail::read_file_content(path, content)          [foglia ifstream]
│   ├── parser::detail::fill_basic_metadata(md, path, content)    [foglia]
│   │   ├── rilevamento binario (NUL nei primi 8 KiB)
│   │   ├── conteggio righe
│   │   └── file_type da estensione (header/source/cuda/other)
│   ├── (binary → ritorna metadati base)
│   ├── ts_parser_new() / ts_parser_set_language(tree_sitter_c|cpp)   [foglia C API]
│   ├── ts_parser_parse_string(...) → TSTree                          [foglia]
│   ├── CppSymbolVisitor::visit(node, scope)                          ⟲ AST
│   │   ├── namespace_definition → push SymbolInfo(namespace) + visit figli con scope esteso
│   │   ├── class|struct|union_specifier → CppSymbolVisitor::emit_type()
│   │   │   └── visit body con scope "Scope::Type"
│   │   ├── enum_specifier → push SymbolInfo(enum)
│   │   ├── function_definition → CppSymbolVisitor::emit_function()
│   │   │   ├── discesa declarator→identifier/qualified_identifier
│   │   │   ├── push SymbolInfo(function, is_definition=true)
│   │   │   └── CppSymbolVisitor::collect_calls(body, caller)        ⟲
│   │   │       ├── (function_definition nidificato → return: la funzione interna possiede i suoi call)
│   │   │       ├── call_expression → CppSymbolVisitor::callee_name(function child)
│   │   │       │   └── identifier|qualified_identifier|field_identifier|field_expression(field)
│   │   │       └── push SymbolDependency{caller, callee, line}
│   │   ├── declaration / field_declaration / prototype → emit_declaration()
│   │   │   ├── prototipo funzione → SymbolInfo(function, is_declaration=true)
│   │   │   ├── typedef/using → SymbolInfo(typedef|using)
│   │   │   └── variabili → collect_identifiers() → SymbolInfo(variable)
│   │   └── template_declaration → visit figli (attraversa)
│   └── extract_includes(root)                                      ⟲
│       └── preproc_include → dipende path field → md.dependencies
├── language "cuda" → parser::parse_cuda(path)
│   └── (stesso schema di parse_cpp, con CudaSymbolVisitor)
│       ├── CudaSymbolVisitor::emit_function()
│       │   ├── cuda_qualifier(prefix testo prima del declarator)
│       │   │   └── "__global__"→kernel, "__device__"→device_function, "__host__"→host_function, "__shared__"→shared_variable
│       │   ├── push SymbolInfo(tipo = kernel/device/host/function)
│       │   └── CudaSymbolVisitor::collect_calls(body, caller)      ⟲
│       └── extract_includes(root)                                   ⟲
├── language "python" → parser::parse_python(path)
│   └── PythonSymbolVisitor::visit(node, scope)                      ⟲
│       ├── class_definition → push SymbolInfo(class) + visit body con scope "Cls"
│       ├── function_definition → push SymbolInfo(function|async_function)
│       │   ├── PythonSymbolVisitor::collect_calls(body, caller)     ⟲
│       │   │   └── call → function child identifier|attribute → SymbolDependency
│       │   └── visit body (def nidificate → scope "outer.inner")
│       ├── decorated_definition → visit(definition)
│       ├── decorator → SymbolInfo(decorator)
│       └── assignment/typed → PythonSymbolVisitor::emit_assignment() → SymbolInfo(variable)
│   + extract_imports(root)                                          ⟲
│       ├── import_statement → dotted_name/aliased_import → dependencies
│       └── import_from_statement → module_name (relative ".utils" → "utils")
└── linguaggio sconosciuto → fallback regex
    ├── parser::detail::read_file_content + fill_basic_metadata
    ├── parser::detail::regex_extract_symbols(content)              [foglia regex]
    │   └── pattern: class|struct|interface|enum / function|def|func|fn
    └── parser::detail::regex_extract_dependencies(content)         [foglia regex]
        └── pattern: import|include|require|use|from
```

---

## 6. Tool `file_content` (inspect)

```
tools::call_tool("file_content", args)
├── tools::resolve_repo(...)                                        → §3
├── tools::guarded_path(repo, file_path)                            → §4
├── path_utils::file_exists(abs)                                    [foglia]
├── KnowledgeStore::query(files: language/file_type/size/lines/hash WHERE path=?)
├── content_hasher::compute_file_hash(abs) → index_status "fresh"|"stale"
├── KnowledgeStore::query(symbols WHERE file_id IN (...) ORDER BY start_line)
├── summary_store::get_file_summary(store, repo_id, abs)
│   └── KnowledgeStore::query(summaries JOIN files ... is_valid=1 AND s.content_hash = f.content_hash LIMIT 1)
├── validazione range (start>=1, end>=start)                        [foglia]
├── file_utils::read_lines(abs, start, end)                         [foglia ifstream]
└── tools::tool_result({path, language, hash, index_status, symbols, summary, content, returned_lines})
```

---

## 7. Tool `edit` (con invalidazione cache)

```
tools::call_tool("edit", args)
├── tools::resolve_repo(...)                                        → §3
├── tools::guarded_path(...)                                        → §4
├── validazione range                                               [foglia]
├── file_utils::read_lines(file, 1, -1)          (tutte le righe)  [foglia]
├── splice in memoria: [1..start) + new_content + (end..fine]      [foglia]
├── file_utils::write_file(file, joined)                            [foglia]
├── cache_manager::CacheManager(store)
│   └── CacheManager::invalidate(file)
│       ├── KnowledgeStore::execute(DELETE symbols WHERE file_id IN (files WHERE path=?))
│       ├── KnowledgeStore::execute(DELETE dependencies WHERE source_path=? OR target_path=?)
│       └── KnowledgeStore::execute(UPDATE summaries SET is_valid=0 ... WHERE file_id IN ...)
├── indexer::index_file(store, file, repo_id)      re-index immediato  → §4
├── (se estensione h/hpp/hxx/cuh)
│   └── CacheManager::invalidate_transitive(file)
│       ├── KnowledgeStore::query(DISTINCT source_path FROM dependencies WHERE target_path=? AND dep_type='include')
│       └── per dipendente: DELETE symbols + UPDATE summaries invalidati (edge conservati → lazy)
└── tool_result({path, replaced_lines, new_line_count, cache_invalidated})
```

---

## 8. Tool `execute`

```
tools::call_tool("execute", args)
├── tools::resolve_repo(...)                                        → §3
├── tools::execute_shell(command, repo.root_path, out, err, ms)
│   ├── tools::shell_quote(cwd)                                     [foglia]
│   ├── std::system("cd <cwd> && ( cmd ) > stdout.log 2> stderr.log")  [foglia]
│   ├── WEXITSTATUS(rc)                                             [foglia]
│   ├── file_utils::read_file(stdout.log)                           [foglia]
│   └── file_utils::read_file(stderr.log)                           [foglia]
└── tool_result({command, working_directory, exit_code, duration_ms, stdout, stderr})
```

Nota: `execute` **non** accetta `path` arbitrario — la working directory è sempre la root del repository (confine).

---

## 9. Tool `repository_search` / `symbol_search`

```
tools::call_tool("repository_search", args)
├── (senza repo_id → cross-repo)
│   ├── KnowledgeStore::open(registry.db)
│   ├── registry.query("SELECT repo_id, root_path FROM repositories")
│   └── per repo:
│       ├── repository::open_repo_store(root, repo_store)
│       │   └── KnowledgeStore::open(get_db_path(root))
│       ├── repo_store.query(files WHERE path LIKE ?)
│       └── symbol_index::search_symbols(repo_store, repo_id, query)
│           └── KnowledgeStore::query(symbols JOIN files WHERE name LIKE ?)
└── (con repo_id)
    ├── store.query(files WHERE repo_id AND path LIKE ? [AND scope][AND file_type])
    └── symbol_index::search_symbols(store, repo_id, query)

tools::call_tool("symbol_search", args)
├── (senza repo_id) → come sopra via registry + open_repo_store
└── (con repo_id)
    ├── symbol_index::get_symbol_details(store, repo_id, name)
    │   └── KnowledgeStore::query(symbols JOIN files WHERE name = ?)
    └── dependency_graph::get_callers(store, repo_id, name)         → §10
```

---

## 10. Tool `dependency_graph` (file e symbol)

```
tools::call_tool("dependency_graph", args)
├── (con "symbol")
│   ├── dependency_graph::get_callees(store, repo_id, symbol)
│   │   └── dependency_graph::symbol_edges(store, repo_id, symbol, outbound=true)
│   │       ├── dependency_graph::symbol_match_sql("source_symbol")  [foglia]
│   │       └── KnowledgeStore::query(SELECT DISTINCT target_symbol, path ...
│   │               WHERE dep_type='call' AND (source_symbol=? OR source_symbol LIKE ?))
│   └── dependency_graph::get_callers(store, repo_id, symbol)
│       └── symbol_edges(..., outbound=false)  → SELECT DISTINCT source_symbol ...
└── (con "file_path")
    ├── tools::guarded_path(...) o weakly_canonical                 → §4
    ├── dependency_graph::get_dependencies(store, repo_id, path)
    │   └── KnowledgeStore::query(target_path WHERE dep_type != 'call')
    ├── dependency_graph::get_all_dependencies(store, repo_id, path)   (depth>1)
    │   ├── ankerl::unordered_dense::set visited
    │   └── ⟲ BFS: get_dependencies per ogni nodo scoperto
    └── dependency_graph::get_dependents(store, repo_id, path)
        └── KnowledgeStore::query(source_path WHERE target_path=? AND dep_type != 'call')
```

---

## 11. Tool `summary`, `file_tree`, `code_analysis`

```
tools::call_tool("summary", args)
├── (file_path + write) → summary_store::store_file_summary(store, repo_id, file, text)
│   ├── KnowledgeStore::query("SELECT file_id, content_hash FROM files ...")
│   ├── KnowledgeStore::execute(UPDATE summaries SET is_valid=0 WHERE file_id=? AND is_valid=1)
│   └── KnowledgeStore::execute(INSERT summaries ... is_valid=1)
├── (file_path, read) → summary_store::get_file_summary(...)        → §6
├── (repo + write)   → summary_store::store_repo_summary(...)  (sentinella file_id=0, content_hash=repo_id)
└── (repo, read)     → summary_store::get_repo_summary(...)

tools::call_tool("file_tree", args)
└── lambda walk ricorsivo ⟲
    ├── std::filesystem::directory_iterator(dir)                    [foglia]
    ├── (skip .git build node_modules __pycache__)
    └── sort entries (directories first, poi nome)

tools::call_tool("code_analysis", args)
├── file_utils::read_lines(file, 1, -1)                             [foglia]
├── classificazione righe code/comment/blank                        [foglia]
└── KnowledgeStore::query(SELECT symbol_type, COUNT(*) FROM symbols WHERE file_id IN (...) GROUP BY symbol_type)
```

---

## 12. Test: come viene esercitato il grafo

```
tests/test_main.cpp::main()
├── run_parser_tests()      tests/test_parsers.cpp
│   ├── parse_cpp / parse_cuda / parse_python su fixture temporanei
│   ├── parser::parse_file (degradazione: file mancante, AST rotta, linguaggio sconosciuto)
│   └── end-to-end: KnowledgeStore.open → indexer::index_file → needs_indexing
│       → symbol_index::get_file_symbols → dependency_graph::get_dependencies
│       → summary_store::store/get → modifica file → needs_indexing==true → re-index
├── run_store_tests()       tests/test_store.cpp   (HOME isolata in /tmp)
│   ├── content_hasher (hash completo anche blocco finale, file grande, mancante)
│   ├── KnowledgeStore (open, params, query errata, binding integrali)
│   ├── repository (identità stabile, isolamento repo, registry)
│   └── cache_manager/indexer (transitive invalidation, stale detection, call edges persistiti)
└── run_tools_tests()       tests/test_tools.cpp
    ├── tools::call_tool per tutti i 10 tool + errori
    └── server::handle_request per initialize/tools/list/tools/call/error/notifiche/malformato
```

---

## 13. Matrice compatta "chi chiama chi" (funzioni interne → callee interni)

| Caller | Callee |
|---|---|
| `main` | `server::init`, `server::run` |
| `server::run` | `server::handle_request` |
| `server::handle_request` | `server::make_result`, `server::make_error`, `tools::list_tools`, `tools::call_tool` |
| `tools::call_tool` | `tools::resolve_repo`, `tools::guarded_path`, `tools::tool_result`, `tools::tool_error`, `tools::execute_shell`, `repository::lookup_repository`, `repository::get_or_create_repository`, `repository::open_repo_store`, `indexer::index_file`, `indexer::index_repository`, `parser::get_language_from_extension`, `symbol_index::*`, `dependency_graph::*`, `summary_store::*`, `cache_manager::CacheManager::*`, `file_utils::read_lines/write_file/read_file`, `path_utils::file_exists`, `content_hasher::compute_file_hash` |
| `tools::resolve_repo` | `repository::lookup_repository`, `repository::get_or_create_repository`, `KnowledgeStore::open` |
| `tools::guarded_path` | `path_utils::is_within_repository` |
| `tools::execute_shell` | `tools::shell_quote`, `file_utils::read_file` |
| `indexer::index_file` | `path_utils::get_extension`, `parser::get_language_from_extension`, `parser::parse_file`, `content_hasher::compute_file_hash`, `KnowledgeStore::execute/query` |
| `indexer::index_repository` | `KnowledgeStore::query`, `path_utils::get_extension`, `indexer::needs_indexing`, `indexer::index_file` |
| `indexer::needs_indexing` | `KnowledgeStore::query`, `content_hasher::compute_file_hash` |
| `parser::parse_file` | `parser::parse_cpp`, `parser::parse_cuda`, `parser::parse_python`, `parser::detail::read_file_content`, `parser::detail::fill_basic_metadata`, `parser::detail::regex_extract_symbols`, `parser::detail::regex_extract_dependencies` |
| `parser::parse_cpp` | `CppSymbolVisitor::visit` → `emit_type`/`emit_function`/`emit_declaration`/`collect_identifiers`/`collect_calls`→`callee_name`, `extract_includes` |
| `parser::parse_cuda` | `CudaSymbolVisitor::visit`→`emit_function`→`cuda_qualifier`/`collect_calls`, `extract_includes` |
| `parser::parse_python` | `PythonSymbolVisitor::visit`→`emit_assignment`/`collect_calls`, `extract_imports` |
| `repository::get_or_create_repository` | `path_utils::normalize_path`, `repository::repo_identity`, `repository::get_db_path`, `KnowledgeStore::open`, `path_utils::get_basename` |
| `repository::repo_identity` | `content_hasher::compute_string_hash` |
| `repository::lookup_repository` | `KnowledgeStore::open`, `KnowledgeStore::query`, `repository::get_db_path` |
| `repository::repository_exists` | `repository::lookup_repository` |
| `KnowledgeStore::open` | `KnowledgeStore::execute` (PRAGMA), `KnowledgeStore::init_schema` |
| `KnowledgeStore::execute` (sovraccarichi) | `KnowledgeStore::execute(vector)` → `prepare_cached` |
| `KnowledgeStore::query` (sovraccarichi) | `KnowledgeStore::query(vector)` → `prepare_cached` |
| `KnowledgeStore::close` | `sqlite3_finalize` (cache stmt), `sqlite3_close` |
| `content_hasher::compute_file_hash` / `compute_string_hash` | `to_hex` |
| `cache_manager::invalidate` | `KnowledgeStore::execute` ×3 |
| `cache_manager::invalidate_transitive` | `KnowledgeStore::query` + `execute` ×2 per dipendente |
| `cache_manager::get_stale_files` | `KnowledgeStore::query`, `content_hasher::compute_file_hash` |
| `dependency_graph::get_all_dependencies` | `dependency_graph::get_dependencies` (BFS) |
| `dependency_graph::get_callees`/`get_callers` | `dependency_graph::symbol_edges` → `symbol_match_sql` |
| `summary_store::store_file_summary` | `KnowledgeStore::query` + `execute` ×2 |
| `symbol_index::*` | `KnowledgeStore::query` |
