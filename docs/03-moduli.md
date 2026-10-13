# 03 — Reference moduli: funzione per funzione

Formato per ogni funzione: **firma** → cosa fa, passo per passo; **ritorna**; **effetti collaterali**; **note**.

---

## `src/main.cpp`

### `int main()`
1. `server::init()` — hook di inizializzazione.
2. `server::run()` — loop del trasporto; ritorna solo quando stdin è chiuso (EOF).
- Ritorna: `0`.

---

## `src/server/server.cpp` — namespace `server`

### `void init()`
Hook vuoto riservato a inizializzazioni globali future. Nessun effetto.

### `std::string handle_request(const std::string& request)`
Processa **un** messaggio JSON-RPC e ritorna la risposta serializzata (o stringa vuota se non serve risposta).
1. `json::parse(request)`; se fallisce → `make_error(-32600, "Invalid request: ...")`.
2. Cattura `id` se presente (usato in ogni risposta).
3. Se manca `method` → stringa vuota (è una notifica/risposta del client).
4. Dispatch per `method`:
   - `"initialize"` → result con `protocolVersion: "2024-11-05"`, `capabilities.tools.listChanged:false`, `serverInfo{name,version}`.
   - `"notifications/initialized"` / `"initialized"` → `""` (notifica).
   - `"ping"` → result `{}`.
   - `"tools/list"` → result `{tools: [10 tool con inputSchema]}` via `tools::list_tools()`.
   - `"tools/call"` → richiede `params.name`; passa `params.arguments` (default `{}`) a `tools::call_tool`; il ritorno del tool (già in forma MCP `{content,isError}`) diventa `result`. Il server aggiunge `structuredContent` (payload JSON completo) e, per non duplicare i byte, sposta i campi bulk (`content`) solo in `structuredContent`, lasciando nel text block i metadati con un marcatore `<field>_omitted`.
   - altrimenti → `make_error(-32601, "Method not found: ...")`.
- Nota: ogni eccezione interna diventa `-32600`; il server non propaga mai eccezioni al loop.

### `void run()`
Loop del trasporto stdio:
1. `std::getline` su stdin; rimuove `\r` finale (CRLF); righe vuote ignorate.
2. Se la riga inizia per `Content-Length:` → framing LSP: estrae N, consuma le righe di header fino alla vuota, legge N byte esatti di body.
3. Altrimenti la riga intera è il messaggio (newline-delimited JSON, MCP stdio).
4. `handle_request(body)`; se la risposta non è vuota: `std::cout << risposta << "\n" << flush`.
- Effetti: legge stdin, scrive stdout. stdout è **solo** JSON-RPC (mai log: corromperebbe il canale).

### Helper anonimi
- `json make_error(int code, const std::string& message, const json& id)` → `{jsonrpc, error:{code,message}, id}`.
- `json make_result(const json& result, const json& id)` → `{jsonrpc, result, id}`.

---

## Layer server tool — namespace `tools`

Il layer tool è separato in quattro file:

### `src/server/tools.cpp` — router
- `std::string list_tools()` → ritorna l'array JSON dei 10 tool con `name`, `description`, `inputSchema` (JSON Schema). Il contenuto è statico e deve restare allineato con il dispatch.
- `std::string call_tool(tool_name, arguments)`:
  1. Costruisce `KnowledgeStore store` e risolve il repository con `resolve_repo(arguments, store)` → `repo_id` (vuoto se non risolvibile).
  2. Se `repo_id` non vuoto: `repository::lookup_repository(repo_id)` → `RepoInfo repo`.
  3. Costruisce `ToolContext{store, repo_id, repo, arguments}` e fa dispatch su un solo `if` per tool verso l'handler in `tools_query.cpp` / `tools_actions.cpp`.
  4. Ritorna sempre un payload MCP: `tool_result(json)` o `tool_error(string)`.

### `src/server/tools_internal.h` — contratto condiviso
- `struct ToolContext { KnowledgeStore& store; const std::string& repo_id; const RepoInfo& repo; const json& arguments; }`.
- Dichiarazioni degli helper condivisi e degli 10 handler (`handle_<tool>(const ToolContext&)`).

### `src/server/tools_support.cpp` — helper condivisi
- `tool_result(json)` → `{"content":[{"type":"text","text":payload.dump()}],"isError":false}`; `tool_error(msg)` → stessa forma, `isError:true`.
- `resolve_repo(arguments, store)`: con `repo_id` usa `lookup_repository` e apre `info.db_path`; altrimenti con `path` (directory o parent del file) usa `get_or_create_repository`; poi fallback cwd; poi repository registrato unico; altrimenti `""`.
- `guarded_path(repo, file_path)`: assoluto diretto / relativo a `repo.root_path`; `weakly_canonical` (risolve `..`, non richiede esistenza); poi `path_utils::is_within_repository`; ritorna `""` se fuori confine.
- `ensure_repo_indexed(store, repo_id, repo, gate_reason)`: self-healing repo-level (walk limitato, soglia 10.000 file; auto-index sotto soglia, hint `index` sopra).
- `paginate_array(arr, arguments, result, prefix, default_limit)`: aggiunge `<prefix>_total/_returned/_truncated/_next_offset` solo quando il paging si applica.

### `src/server/tools_query.cpp` — handler di sola lettura
`handle_repository_search`, `handle_symbol_search`, `handle_file_content`, `handle_dependency_graph`, `handle_file_tree`, `handle_code_analysis` (vedi [05-tools.md](05-tools.md) per input/output).

### `src/server/tools_actions.cpp` — handler con effetti
`handle_summary`, `handle_index`, `handle_edit`, `handle_execute`.
- `shell_quote(s)` → racchiude in `'...'` escapando i `'` (`'\''`).
- `make_temp_file(pattern)` → `mkstemp` (file univoci 0600, niente race su path fissi).
- `execute_shell(command, cwd, out, err, duration_ms)`: `cd <cwd quoted> && ( <command> ) > <tmp-out> 2> <tmp-err>` via `std::system`; durata con `steady_clock`; legge i file con `file_utils::read_file`; rimuove i temp.
  - Ritorna exit code (`WEXITSTATUS`; `-1` se system fallisce).

---

## `src/engine/repository.cpp` — namespace `repository`

### `std::string repo_identity(const std::string& root_path)`
`SHA-256(normalize(absolute(root_path)))` → primi 16 hex char. Deterministica: stesso path → stesso id, sempre.

### `std::string get_db_path(const std::string& root_path)`
`$HOME/.ai-tools/repos/<repo_identity>/knowledge.db` (fallback `/tmp/ai-tools/...` se `HOME` non impostata).

### `RepoInfo get_or_create_repository(const std::string& root_path)`
1. Canonicalizza root; calcola identità e db path.
2. Crea la directory e apre il DB del repo (inizializza lo schema) e vi replica l'identità (`INSERT OR IGNORE` in `repositories`) — così l'indexer può risolvere `repo_id→root_path` dal DB locale.
3. Apre il registry globale `~/.ai-tools/registry.db`.
4. Se il repo esiste → aggiorna `updated_at`, ritorna `RepoInfo` preservando `created_at`.
5. Altrimenti INSERT nuovo.
- `RepoInfo{repo_id, root_path, name, db_path, created_at, updated_at}`; `repo_id` vuoto = fallito.

### `RepoInfo lookup_repository(const std::string& repo_id)`
Apre il registry, `SELECT` per id; se non trovato ritorna `RepoInfo` con `repo_id` vuoto. Calcola `db_path` dal `root_path` memorizzato.

### `bool open_repo_store(const std::string& root_path, KnowledgeStore& store)`
Assicura la directory e apre il DB del repo. Usata dalla ricerca cross-repo.

### `bool repository_exists(const std::string& repo_id)`
`!lookup_repository(repo_id).repo_id.empty()`.

### Static
- `format_timestamp(time_t)` → `"YYYY-MM-DDTHH:MM:SSZ"` (UTC).
- `registry_path()` → `$HOME/.ai-tools/registry.db`.

---

## `src/engine/knowledge_store.cpp` — class `KnowledgeStore`

### `bool open(const std::string& db_path)`
1. No-op se già aperto.
2. `sqlite3_open`; errore → `error_` settato, `false`.
3. PRAGMA: `journal_mode=WAL`, `busy_timeout=5000`, `foreign_keys=ON`.
4. `init_schema()` → crea 6 tabelle + indici `IF NOT EXISTS` (vedi [04-data-model.md](04-data-model.md)).

### `void close()`
`sqlite3_finalize` su ogni statement in cache, `clear` della mappa, `sqlite3_close`. Chiamata anche dal distruttore.

### `sqlite3_stmt* prepare_cached(const std::string& sql)` *(private)*
Cache prepared statement: hit → riuso; miss → `sqlite3_prepare_v2` e memorizzazione. In ogni caso `sqlite3_reset` + `clear_bindings` prima dell'uso. `nullptr` su prepare fallito (con `error_`).

### `int execute(sql, params...)`
Prepara (cache), lega i parametri come testo, `sqlite3_step`, `reset`. Ritorna `SQLITE_OK`/`SQLITE_ROW` o il codice d'errore. Overloads: vector, 1–4 stringhe, e variadic template (`StringConvertible`: stringhe o integrali → convertiti con `to_string`).

### `std::vector<std::vector<std::string>> query(sql, params...)`
Prepara (cache), lega parametri, itera `SQLITE_ROW`; NULL → stringa vuota. `reset` finale. Su errore ritorna risultati parziali con `error_` settato (nessuna eccezione).

### `std::string get_error() const` / `bool is_open() const`
Ultimo messaggio sqlite; stato apertura.

---

## `src/engine/content_hasher.cpp` — namespace `content_hasher`

### `std::string compute_file_hash(const std::string& file_path)`
SHA-256 streaming in blocchi da 4096 byte. **Nota critica**: il ciclo è `while (read(...) || gcount()>0)` per hashare anche l'ultimo blocco parziale. File illeggibile → stringa vuota.

### `std::string compute_string_hash(const std::string& content)`
SHA-256 della stringa. Usata per l'identità dei repository.

### static `to_hex(hash, len)` → 64 hex char minuscoli.

---

## `src/engine/parser.cpp` — namespace `parser`

### `FileMetadata parse_file(path, language)`
Dispatch: `c|cpp`→`parse_cpp`, `cuda`→`parse_cuda`, `python`→`parse_python`; altrimenti percorso fallback:
1. `detail::read_file_content` (fallito → metadati minimali, nessun crash).
2. `detail::fill_basic_metadata`.
3. `detail::regex_extract_symbols` + `regex_extract_dependencies` (pattern basilari, priorità 3).

### `std::string get_language_from_extension(ext)`
`c`→c; `cpp|cc|cxx|c++|h|hpp|hxx`→cpp; `cu|cuh`→cuda; `py`→python; altro→`unknown`.

### `parser::detail::read_file_content(path, content)`
Lettura binaria completa del file. `false` se non apribile.

### `parser::detail::fill_basic_metadata(md, path, content)`
- `is_binary`: presenza di `\0` nei primi 8192 byte.
- `line_count`: numero di `\n` (+1 se il file non termina con newline).
- `file_type`: `header` (h/hpp/hxx/h++/hh), `cuda` (cu/cuh), `source` (c/cpp/cc/cxx/c++/py), altrimenti `other`.
- `size_bytes` = dimensione contenuto.

### `parser::detail::regex_extract_symbols(content)`
Per riga, due regex: `(class|struct|interface|enum) Nome` e `(function|def|func|fn) Nome` → `SymbolInfo{name, type, signature=riga, start=end=riga, is_declaration=true}`.

### `parser::detail::regex_extract_dependencies(content)`
Regex `(import|include|require|use|from) <target>` → vettore di target (quote/angle rimosse).

---

## `src/engine/parser/cpp_parser.cpp` — `parse_cpp` + visitor anonimi

### `FileMetadata parse_cpp(file_path)`
1. Lettura + `fill_basic_metadata`; estensione `.c` → grammar tree-sitter-c, altrimenti tree-sitter-cpp.
2. `is_binary` → ritorna metadati base.
3. Parser/tree RAII (`TsParser`/`TsTree`); ABI mismatch o parse null → metadati base (fallimento isolato).
4. Radice AST → `CppSymbolVisitor::visit(child, "")` per ogni figlio named.
5. `extract_includes(root)`.

### `CppSymbolVisitor`
- `visit(node, scope)` — dispatch per tipo nodo:
  - `namespace_definition` → SymbolInfo(namespace, definition) + visita body con scope `ns` o `parent::ns`.
  - `class|struct|union_specifier` → `emit_type` + visita body con scope `Scope::Type`.
  - `enum_specifier` → SymbolInfo(enum).
  - `function_definition` → `emit_function`.
  - `declaration`, `field_declaration`, `function_declaration`, `prototype_function_declaration` → `emit_declaration`.
  - `template_declaration` → attraversa (visita figli).
  - default → visita figli named.
- `emit_type(node, ts_type, scope)` — nome dal field `name` (anonimi saltati); tipo `class|struct|union`; `is_definition` se ha body.
- `emit_function(node, scope)` — scende la catena di declarator fino a `identifier|field_identifier|operator_name|destructor_name|qualified_identifier|template_function_id`; SymbolInfo(function, definition, signature = testo da inizio a inizio-body); poi `collect_calls(body, caller_qualificato)`.
- `collect_calls(node, caller)` — ricorsione sul body; salta `function_definition` nidificati (la funzione interna possiede i suoi call); su `call_expression` estrae callee via `callee_name` e pusha `SymbolDependency{caller, callee, line}`; continua a scendere (call annidati negli argomenti).
- `callee_name(fn)` — `identifier|qualified_identifier|field_identifier|destructor_name|operator_name|template_function_id` → testo; `field_expression` (`obj.m()` / `obj->m()`) → field `field` (solo il nome metodo).
- `emit_declaration(node, scope)` — testo normalizzato:
  - prototipo funzione (contiene `(` e finisce `;`) → SymbolInfo(function, is_declaration=true, nome prima della parentesi).
  - `typedef`/`using` → SymbolInfo(typedef|using, nome best-effort prima di `=`/`;`).
  - altrimenti → `collect_identifiers` sui declarator → SymbolInfo(variable).

### `extract_includes(node, source, metadata)`
Ricorsione; nodo `preproc_include` → field `path`, rimuove `"`/`<` e `"`/`>` → `metadata.dependencies`.

---

## `src/engine/parser/cuda_parser.cpp` — `parse_cuda`

Identico a `parse_cpp` con grammar cpp e `CudaSymbolVisitor`:
- `cuda_qualifier(prefix)` — cerca nel testo prima del declarator: `__global__`→`kernel`, `__device__`→`device_function`, `__host__`→`host_function`, `__shared__`→`shared_variable`.
- `emit_function` — come cpp ma con tipo simbolo dal qualificatore (kernel/device/host/function); poi `collect_calls`.
- `declaration` con qualificatore CUDA (parser a volte produce declaration per kernel) → SymbolInfo con tipo dal qualificatore.
- Launch `<<<grid,block>>>` non è un `call_expression` tree-sitter → non genera edge (limitazione documentata).

---

## `src/engine/parser/python_parser.cpp` — `parse_python`

- `class_definition` → SymbolInfo(class, signature da header a body) + visita body con scope `Cls` (o `outer.Cls`).
- `function_definition` → SymbolInfo(function o **async_function** se la signature inizia per `async `); scope children `outer.func`; `collect_calls(body, caller_qualificato)`; poi visita body per def nidificate.
- `decorated_definition` → visita la `definition` sottostante.
- `decorator` → SymbolInfo(decorator, nome con `@`).
- `expression_statement` con `assignment|augmented_assignment|...` → `emit_assignment` (solo target `identifier`; tuple/attribute saltati) → SymbolInfo(variable).
- `extract_imports`:
  - `import_statement` → figli `dotted_name` o `aliased_import` (field `name`) → dependency.
  - `import_from_statement` → field `module_name`; relative `.utils`/`..pkg` → normalizzate a `utils`/`pkg`.
- `collect_calls(node, caller)` — salta `function_definition`/`class_definition` nidificati; su nodo `call` → field `function`: `identifier` o `attribute` (nome dopo l'ultimo `.`) → `SymbolDependency`.

---

## `src/engine/indexer.cpp` — namespace `indexer`

### `bool needs_indexing(store, file_path, repo_id)`
1. `SELECT content_hash FROM files WHERE repo_id=? AND path=?` → vuoto = **true** (mai indicizzato).
2. Hash corrente (`compute_file_hash`) ≠ hash salvato → true.
- Nota: file cancellato da disco → hash vuoto ≠ salvato → true (stale).

### `void index_file(store, file_path, repo_id)`
1. Estensione → linguaggio; `unknown` → skip.
2. `parse_file` → `FileMetadata`.
3. `compute_file_hash`; vuoto → skip (illeggibile).
4. `file_size` (errore → 0).
5. **Transazione** `BEGIN` ... `COMMIT` con:
   - `INSERT OR REPLACE files` (hash, language, file_type, size, line_count, `last_indexed_at=datetime('now')`, is_binary).
   - `DELETE symbols WHERE file_id` + INSERT per ogni simbolo (con `content_hash_at_index`).
   - `DELETE dependencies WHERE source_path` + INSERT `include|import`; il target viene **risolto a file reale** se esiste su disco (relative `./x`, `../x` dal parent del file; altrimenti path assoluto o parent) → abilita lookup inversi e invalidazione transitiva.
   - `DELETE dependencies WHERE dep_type='call'` + INSERT per ogni `SymbolDependency` con `source_symbol=caller`, `target_symbol=callee`.
   - UPSERT `indexing_state` (`last_incremental_at`, status `incremental`, `files_indexed+1`).

### `void index_repository(store, repo_id)`
1. `SELECT root_path FROM repositories WHERE repo_id=?` (locale al DB del repo).
2. `recursive_directory_iterator` con `skip_permission_denied`; errore walk → messaggio e ritorno.
3. Per ogni file regular: estensione supportata; skip path contenenti `/build/`, `/.git/`, `/node_modules/`, `/third_party/`.
4. `needs_indexing` → **skip se fresco** (incrementale).
5. `index_file` per i file cambiati; contatori.
6. UPSERT `indexing_state` con `status='full'`, `files_indexed`, `files_total`.

---

## `src/engine/cache_manager.cpp` — class `CacheManager`

### `bool is_valid(file_path, current_hash)`
Confronta hash salvato (per path) con quello corrente.

### `void invalidate(file_path)`
- DELETE `symbols` del file.
- DELETE `dependencies` dove il file è source **o** target.
- UPDATE `summaries SET is_valid=0, invalidated_at=now` per il file.

### `void invalidate_transitive(file_path)`
Per i file che **includono** il target (`dep_type='include'`): invalida simboli e riassunti dei dipendenti **senza toccare gli edge** (invalidazione lazy: il grafo resta per scoprire i dipendenti al prossimo accesso).

### `std::vector<std::string> get_stale_files()`
Per ogni file `is_deleted=0`: hash a disco ≠ hash DB (o file sparito) → stale.

---

## `src/engine/dependency_graph.cpp` — namespace `dependency_graph`

### `std::vector<std::string> get_dependencies(store, repo_id, file_path)`
Edge file-level `out`: `target_path` con `dep_type != 'call'`.

### `std::vector<std::string> get_dependents(store, repo_id, file_path)`
Edge file-level `in`: `source_path` con `dep_type != 'call'`.

### `std::vector<std::string> get_all_dependencies(store, repo_id, file_path)`
BFS transitivo con `ankerl::unordered_dense::set` visited e `std::queue`; ritorna tutti i nodi raggiungibili (escluso il root).

### `symbol_edges(store, repo_id, symbol_name, outbound)` *(anonima)*
Query sul call graph: seleziona il lato opposto (`target_symbol` per callees, `source_symbol` per callers) con match `(col = ? OR col LIKE %?%)` — accetta sia nome bare (`helper`) sia qualificato (`indexer::helper`). Join su `files` per allegare il path del file sorgente dell'edge.

### `get_callees` / `get_callers`
Wrapper di `symbol_edges` outbound/inbound. Formato risultato: `"symbol (file_path)"`.

### `build_adjacency_list(store, repo_id)`
Mappa `source → [targets]` su tutti gli edge file-level del repo (`ankerl::unordered_dense::map`).

---

## `src/engine/symbol_index.cpp` — namespace `symbol_index`

### `search_symbols(store, repo_id, name)`
LIKE `%name%` su `symbols.name` JOIN `files` filtrando `repo_id` → `"name (type) in path:line"`.

### `get_symbols_by_type(store, repo_id, symbol_type)`
Match esatto su tipo → `"name in path:line"`.

### `get_file_symbols(store, repo_id, file_path)`
Tutti i simboli di un file → `"type name at line N"`.

### `get_symbol_details(store, repo_id, symbol_name)`
Match **esatto** nome → testo multilinea: nome/tipo, signature, `file:start-end`, scope.

---

## `src/engine/summary_store.cpp` — namespace `summary_store`

### `store_file_summary(store, repo_id, file_path, summary)`
1. Recupera `file_id` e `content_hash` del file (se non indicizzato → no-op).
2. UPDATE: riassunti validi precedenti → `is_valid=0, invalidated_at=now`.
3. INSERT nuovo riassunto associato all'hash corrente, `is_valid=1`.

### `get_file_summary(store, repo_id, file_path)`
Solo riassunti con `is_valid=1` **E** `s.content_hash = f.content_hash` (validità hash-driven automatica), più recente. Vuoto = nessuno o stale.

### `store_repo_summary` / `get_repo_summary`
Riassunti repository-level con sentinella `file_id=0` e `content_hash=repo_id`.

---

## `src/utils/path_utils.cpp` — namespace `path_utils`

### `bool is_within_repository(repo_root, path)`
`weakly_canonical` di entrambi (no eccezioni, usa `error_code`); verifica `path` inizia per `root` **e** il carattere successivo è `/` (oppure uguaglianza) → `/repo2/x` non è dentro `/repo`.

### `resolve_path`, `get_absolute_path`, `file_exists`, `get_extension`, `get_basename`, `normalize_path`
Utility path: join con root, `std::filesystem::absolute`, ifstream `.good()`, estensione dopo l'ultimo `.` (senza punto), nome dopo l'ultimo `/`, path normalizzato.

---

## `src/utils/file_utils.cpp` — namespace `file_utils`

### `read_file(path)`
Lettura binaria completa (seek end → size → read).

### `write_file(path, content)`
Scrittura binaria completa; `false` su errore.

### `get_file_size`, `get_modification_time`
`filesystem::file_size`; conversione `file_time` → `system_clock` (C++20).

### `read_lines(path, start_line=1, end_line=-1)`
Vettore di righe 1-based; `end_line=-1` = fino alla fine; si ferma oltre `end_line` (early break).

### `is_binary(path)`
Byte `\0` nei primi 1024.

### `walk_directory(dir, follow_symlinks=false)`
Ricorsione manuale: file raccolti; directory saltate se hidden (`.git`, `.venv`, ...) o note (`build`, `node_modules`, `__pycache__`, `venv`, `.tox`).

---

## `tests/` — struttura dei test

- `test_main.cpp::main()` — esegue `run_parser_tests()`, `run_store_tests()`, `run_tools_tests()`; exit code = numero di asserzioni fallite.
- `test_parsers.cpp` — fixture temporanei; verifica extraction simboli (namespace/class/struct/enum/function, declaration vs definition, scope), include, call edges C++/Python, CUDA kernel/device, degradazione (file mancante, AST rotta, linguaggio sconosciuto), end-to-end indexer→store→query→modifica→stale→re-index.
- `test_store.cpp` — hasher (blocco finale parziale, file grande, mancante), KnowledgeStore (params, binding integrali, query errata), repository (identità stabile, isolamento, registry), cache/indexer (transitive invalidation, stale via hash, call edges persistiti, skip `build/`, re-index incrementale "solo il file cambiato").
- `test_tools.cpp` — ogni tool via `tools::call_tool` con unwrap del payload MCP; boundary rejection; range invalidi; execute con exit code; ricerca cross-repo; symbol callers/callees; server JSON-RPC (initialize, tools/list, tools/call, -32601, notifiche, -32600).
