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
   - `"resources/list"` / `"resources/templates/list"` / `"resources/read"` → delegates to `resources::list/templates_list/read` (see the Resources layer below); `resource_error` becomes a JSON-RPC error (`-32602` params, `-32002` not found, `-32603` internal). In `resources/read` the error `data` carries the requested `uri`.
   - `"resources/subscribe"` → `make_error(-32002, "Resource subscriptions are not supported")` (per spec: a non-subscribable resource surfaces as not found).
   - otherwise → `make_error(-32601, "Method not found: ...")`.
- Nota: ogni eccezione interna diventa `-32600`; il server non propaga mai eccezioni al loop (`resource_error` viene catturato prima del fallback generico).

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

## Layer server resources — namespace `resources`

`src/server/resources.h` + `src/server/resources.cpp` implementano l'interfaccia MCP **Resources**: il contesto del repository (file, riassunti semantici, overview dell'indice) viene esposto come risorsa di protocollo, così i client possono agganciarlo al contesto **senza consumare chiamate di tool**.

### `struct resource_error : std::runtime_error` — `code`
Fallimento del layer risorse con codice JSON-RPC già conforme alla spec: `-32602` (parametri), `-32002` (risorsa non trovata), `-32603` (errore interno). `server.cpp` lo traduce in risposta di errore.

### `json list(const json& params)` → `{resources:[...], nextCursor?}`
1. Risolve il repository_target: `repo_id` esplicito → `lookup_repository`; `path` → repository registrato contenente il path, altrimenti auto-registrazione; altrimenti repository registrato unico; altrimenti modalità "tutti".
2. Enumera (bound a 5.000 risorse, walk con `skip_permission_denied` che salta `.git`, `build*`, `node_modules`, `__pycache__`, `third_party`, `.ai-tools`, `dist`, `venv`):
   - `repo://<repo_id>/index`, `repo://<repo_id>/summary` (solo se un riassunto esiste), un `repo://<repo_id>/file/<rel>` per ogni file regular testuale, un `repo://<repo_id>/summary/<rel>` per ogni file con riassunto valido.
   - in modalità "tutti": anche `repo://registry/index` + enumerazione di ogni repository registrato.
3. Ogni entry è una `Resource` MCP: `uri`, `name`, `title`, `description`, `mimeType`, `size`, `annotations{audience,priority,lastModified}`.
4. Paginazione cursor-based offset numerico (`cursor`, `limit` default 200) → `nextCursor`.

### `json templates_list(const json& params)` → `{resourceTemplates:[...]}`
Sei URI template statici: `repo://{repo_id}/file/{path}`, `repo://{repo_id}/summary/{path}`, `repo://{repo_id}/summary`, `repo://{repo_id}/index`, `repo://registry/index`, `file:///{path}`.

### `json read(const json& params)` → `{contents:[content]}`
1. `uri` obbligatorio (stringa) → altrimenti `-32602`.
2. `parse_uri`: scheme `repo` (authority = `repo_id` o `registry`) o `file` (path assoluto); percent-decoding del path; scheme sconosciuto → `-32002`.
3. `repo://registry/index` → overview dei repository dal registry globale.
4. `repo://<id>/...`: `lookup_repository` (sconosciuto → `-32002`), apre lo store del repo (fallito → `-32603`); path dopo l'authority: vuoto o `index` → overview (file con path relativo + `uri` della risorsa, conteggi simboli/dipendenze, `index_status`); `summary` → riassunto repository; `summary/<rel>` → riassunto del file (non valido → `-32002`); `file/<rel>` → contenuto testo.
5. `read_repo_file`: `guarded_abs` (fuga dal root → `-32002`), file mancante → `-32002`, `is_binary` → `-32002`, testo con cap 1 MiB e marcatore di truncation; `mimeType` via estensione (`text/x-c`, `text/x-c++`, `text/x-python`, `text/markdown`, …).
6. `file:///<abs>`: `weakly_canonical`, poi `owner_of` (repository registrato contenente il path) → nessun owner → `-32002` (con hint); altrimenti stesso handler di `repo://<id>/file/<rel>`.
- Guardie: ogni path relativo URI passa da `guarded_abs` (niente traversal fuori dal root); binary mai esposti; letture limitate a 1 MiB.

### Helper anonimi
- `skipped_dir`, `encode_uri_path`/`decode_uri_path`, `parse_uri`, `iso8601`, `registered_ids`, `resolve`, `owner_of`, `guarded_abs`, `to_rel`, `mime_for`, `make_resource`, `enumerate_repo`, `enumerate_registry`, `page`, `read_repo_file`, `read_repo_summary`, `read_repo_index`, `read_registry_index`.

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

### `FileMetadata parse_file(path, language[, content])` *(IDX-03)*
Overload con byte-snapshot: `parse_file(path, language, content)` non riapre il file — parser e hash del chiamante descrivono quindi **la stessa versione**. La variante a 2 argomenti legge il file una volta (`detail::read_file_content`; fallito → metadati minimali, nessun crash) e delega all'overload con contenuto.
Dispatch: `c|cpp`→`parse_cpp(path, content)`, `cuda`→`parse_cuda(path, content)`, `python`→`parse_python(path, content)` (anch'essi con overload content e wrapper da-disco); altrimenti percorso fallback:
1. `detail::fill_basic_metadata`.
2. `detail::regex_extract_symbols` + `regex_extract_dependencies` (pattern basilari, priorità 3).

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

### `FileMetadata parse_cpp(file_path[, content])`
1. Lettura + `fill_basic_metadata`; estensione `.c` → grammar tree-sitter-c, altrimenti tree-sitter-cpp. Overload con `content` (IDX-03): nessuna lettura, il parser consuma il snapshot del chiamante; la variante da-disco e' un wrapper read+delega (fallimento lettura → metadati minimali).
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

Overload `parse_cuda(file_path[, content])` come `parse_cpp` (snapshot → nessun riaccesso al file; variante da-disco = wrapper read+delega). Identico a `parse_cpp` con grammar cpp e `CudaSymbolVisitor`:
- `cuda_qualifier(prefix)` — cerca nel testo prima del declarator: `__global__`→`kernel`, `__device__`→`device_function`, `__host__`→`host_function`, `__shared__`→`shared_variable`.
- `emit_function` — come cpp ma con tipo simbolo dal qualificatore (kernel/device/host/function); poi `collect_calls`.
- `declaration` con qualificatore CUDA (parser a volte produce declaration per kernel) → SymbolInfo con tipo dal qualificatore.
- Launch `<<<grid,block>>>` non è un `call_expression` tree-sitter → non genera edge (limitazione documentata).

---

## `src/engine/parser/python_parser.cpp` — `parse_python`

Overload `parse_python(file_path[, content])` come `parse_cpp` (snapshot → nessun riaccesso al file; variante da-disco = wrapper read+delega).
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

### `bool needs_indexing(store, file_path, repo_id[, out_hash])`
1. `SELECT content_hash, stale, is_deleted FROM files WHERE repo_id=? AND path=?` → vuoto = **true** (mai indicizzato).
2. Record `is_deleted=1` (tombstone IDX-02) → **true**: il file e' tornato su disco, va ri-indicizzato (e il record viene resuscitato).
3. Record `stale=1` → true.
4. Hash corrente (`compute_file_hash`) ≠ hash salvato → true. L'overload con `std::string* out_hash` restituisce l'hash letto da disco ("" se file illeggibile), così il loop di scansione può riusarlo senza rileggere il file (IDX-03).
- Nota: file cancellato da disco → hash vuoto ≠ salvato → true (stale).

### `UpdateReport index_snapshot(store, file_path, repo_id, content, force_propagate=false)` *(IDX-03, DEP-03)*
**Unico punto che scrive la conoscenza derivata di un file e propaga ai dipendenti.** `content` e' il byte-snapshot fornito dal chiamante: `content_hash` (`compute_string_hash`), parsing (`parse_file(path, language, content)`), simboli (`content_hash_at_index`), dipendenze, `size_bytes` (=`metadata.size_bytes`, non `filesystem::file_size`), `line_count` e `is_binary` derivano **tutti dallo stesso buffer**; il file non viene mai riaperto. `last_indexed_at` e `content_verified_at` ricevono lo stesso timestamp di scrittura: il primo registra quando e' stata scritta la conoscenza derivata, il secondo quando il contenuto del buffer e' stato verificato. Gate di estensione/linguaggio prima di usare il contenuto (`unknown` → skip, `skipped_language=true`).
**Gate di supersessione (DEP-03)**: `SELECT content_hash, is_deleted FROM files` — il file e' `changed` se aveva un record e l'hash dello snapshot differisce da quello memorizzato **o** il record era un tombstone (`is_deleted=1` → resurrezione). Se `changed || force_propagate`: `cache_manager::collect_dependents` (BFS transitiva sul grafo inverso, **prima** di qualunque cancellazione). Altrimenti nessuna traversata (counter `propagation_skipped_unchanged`): un re-indicizzo a contenuto invariato non propaga, altrimenti ogni self-heal di lettura pagherebbe una BFS e una catena di due include andrebbe in ping-pong.
**Transazione** `BEGIN` ... `COMMIT` con:
- `cache_manager::reap_dependent` per ogni dipendente raccolto (DELETE `symbols` del dipendente, `summaries is_valid=0`, `files stale=1`, **edge conservati** — lazy); ogni statement verificato, **ROLLBACK** di tutta l'invalidazione se una scrittura fallisce (`report.error`).
- `UPDATE files ...` (hash, language, file_type, size, line_count, `last_indexed_at=datetime('now')`, is_binary, `stale=0`) + `INSERT ...` se il record non esiste (`INSERT ... WHERE changes()=0`), mantenendo `file_id` stabile tra re-indicizzi e resuscitando i tombstones.
- re-validazione hash-driven delle summary precedentemente invalidate (hash dello snapshot = hash della summary → `is_valid=1`).
- `DELETE symbols WHERE file_id` + INSERT per ogni simbolo (con `content_hash_at_index`).
- `DELETE dependencies WHERE source_path` + INSERT `include|import`; il target viene **risolto a file reale** se esiste su disco (relative `./x`, `../x` dal parent del file; altrimenti path assoluto o parent) → abilita lookup inversi e invalidazione transitiva. Se il target e' un path risolto che il database conosce come **tombstone** (`is_deleted=1`), l'edge viene **droppato** (non si crea un arco verso un path inesistente).
- `DELETE dependencies WHERE dep_type='call'` + INSERT per ogni `SymbolDependency` con `source_symbol=caller`, `target_symbol=callee`.
- UPSERT `indexing_state` (`last_incremental_at`, status `partial` se non c'e' uno snapshot di copertura, altrimenti preservato; `files_indexed` = conteggio riga live).
Ritorna `UpdateReport{ indexed, changed, propagated, truncated, dependents, content_hash, error }`.

### `UpdateReport index_file(store, file_path, repo_id)` *(wrapper, IDX-03, DEP-03)*
1. Estensione → linguaggio; `unknown` → skip (**prima** di leggere: i file non indicizzabili non vengono letti da disco) → `skipped_language=true`.
2. `detail::read_file_content` — **una sola apertura**; fallito → skip (illeggibile, `unreadable=true`).
3. Delega a `index_snapshot(store, file_path, repo_id, content)`: hash, parsing **e propagazione** consumano lo stesso buffer letto qui. La propagazione ai dipendenti e' quindi garantita **per costruzione** in ogni call site (DEP-03), non per disciplina del chiamante.

### `UpdateReport index_dependent_chain(...)` *(DEP-03)*
Chiama `index_snapshot(..., force_propagate=true)`: riconcilia i dipendenti anche quando il contenuto e' invariato (una catena puo' essere gia' stale a meta') e ritorna `report.dependents`.

### `bool has_dependents(...)` *(diagnostico)*
Wrapper sottile: `!cache_manager::collect_dependents(...).empty()`. La BFS duplicata e' stata eliminata (DEP-03).

### `disappeared_files(store, repo_id, files_on_disk)` *(anonima)*
Confronta i record live (`is_deleted=0`) con il walk del filesystem (path canonici): un record la cui forma canonica non compare nel walk **e** il cui path canonico non esiste piu' su disco (guardia: un file non piu' indicizzabile per estensione non e' un file eliminato) → insieme di riconciliazione (IDX-02). Chiave: la **forma memorizzata** (quella riferita da `file_id`, summary ed edge), non quella canonica del walk.

### `mark_dependents_of_deleted(store, repo_id, gone)` *(anonima)*
Per ogni file in `gone`, trova i dipendenti `dep_type != 'call'` ancora presenti nel grafo e li marca `stale=1`, **prima** che gli edge vengano cancellati: se un dipendenteincludeva un file cancellato, deve essere ri-parserizzato invece di conservare all'infinito simboli e edge derivati da un contenuto inesistente. Restituisce il numero di dipendenti marcati (o -1 su errore di scrittura).

### `reconcile_deleted_files(store, repo_id, gone)` *(anonima, IDX-02)*
In **un'unica transazione** `BEGIN ... COMMIT` con ogni statement verificato e **ROLLBACK all-or-nothing** se uno fallisce o il commit fallisce:
1. `UPDATE files SET is_deleted=1, stale=0, last_indexed_at=datetime('now')` (tombstone, mantenuto per audit);
2. `DELETE FROM symbols WHERE file_id IN (...)`;
3. `UPDATE summaries SET is_valid=0, invalidated_at=datetime('now') WHERE file_id IN (...)`;
4. `DELETE FROM dependencies WHERE source_path=? OR target_path=?`.
Limitato al `repo_id` corrente; in caso di rollback i record restano live e `repo_coverage()` li segnala come copertura non verificata (mai una falsa claim `full`).

### `FullScanResult index_repository(store, repo_id)`
Restituisce: `ok`, `files_total`, `files_refreshed`, `files_verified` (file invariati verificati per hash e ri-stampati), `files_reconciled` (tombstone scritti), `dependents_invalidated` (dipendenti marcati stale dalla propagazione dei file cambiati o cancellati), `dependents_refreshed` e `stabilization_passes` (DEP-03).
1. `SELECT root_path FROM repositories WHERE repo_id=?` (locale al DB del repo).
2. `scan_indexable_files(root, -1)`: walk ricorsivo illimitato, `skip_permission_denied`, solo estensioni supportate (`supported_source_extensions()`), skip path contenenti `/build/`, `/.git/`, `/node_modules/`, `/third_party/`. Errore walk → `status='failed'` + snapshot di copertura azzerato (`fail()`).
3. `disappeared_files` calcolato **prima** del loop incrementale.
4. Se l'insieme non e' vuoto: `mark_dependents_of_deleted` (invalidazione) e poi `reconcile_deleted_files` (tombstone transazionale).
5. Per ogni file su disco: `needs_indexing(..., &disk_hash)` → se invariato, aggiornare `content_verified_at` sul record senza ri-parsare (0 letture); altrimenti **una sola** `read_file_content` e `index_snapshot` (budget: 1 lettura per file cambiato, 0 per file invariato — IDX-03). Se l'hash del buffer letto ≠ `disk_hash` il file e' cambiato tra confronto e lettura: **retry bounded di una sola rilettura**; in ogni caso hash e simboli indicizzati derivano dallo stesso snapshot. Lettura fallita → skip (la riconciliazione del punto 4 gestisce il tombstone al prossimo scan). Ogni `index_snapshot` che sostituisce la conoscenza propaga ai dipendenti transitivi (DEP-03): i dipendenti marcati stale al punto 4 (e quelli marcati dalla propagazione dei file cambiati) vengono raccolti in `pending_dependents`. I dipendenti marcati stale al punto 4 vengono ri-parsed qui, ora che il tombstone esiste → i loro edge non puntano piu' al path cancellato.
6. **Passata di stabilizzazione bounded (DEP-03)**: un worklist `std::deque` re-indicizza i dipendenti marcati stale dalla propagazione della scansione stessa, quando il loop principale li ha visitati prima dei loro includer (l'ordine del walk non e' specificato) o quando il walk non li raggiunge. Limitata a `kMaxStabilizationPasses = 4` round e al safety cap sui dipendenti; `dependents_refreshed` conta i ri-parser, `stabilization_passes` i round consumati. Se il bound viene raggiunto, i record restano `stale=1` e la copertura li segnala (mai una falsa claim `full`).
7. UPSERT `indexing_state` con `status='full'`, `files_indexed`, `files_total`, `full_index_covered`, `last_error=NULL`: la copertura certificata da `repo_coverage()`.

### `repo_coverage()` / gate `ensure_repo_indexed()` (IDX-04)
Il walk confronta l'insieme dei file live indicizzati con quello su disco. Per i file presenti, `mtime <= content_verified_at` certifica il fast-path; se mtime e' piu' recente o il marcatore manca, `touched_records()` confronta l'hash corrente con `content_hash`. L'hash uguale e' freshness per contenuto; l'hash differente o non leggibile e' stale. Il controllo hash e' limitato a 2.000 file per verifica. Se una scansione completata resta non fresca, il gate blocca la query con `index_status="index_stale_after_scan"`, un `hint` e i campi diagnostici `index_coverage.touched_files` / `touched_paths`, anziche' restituire un envelope vuoto ambiguo. `coverage_certified_at` espone l'istante della certificazione della copertura.

> Un tombstone non e' conoscenza live: `repo_coverage()` lo conteggia in `tombstoned_records` (separato dai `files_indexed`), e le query symbol/dependency lo escludono. Se il file ritorna su disco, il prossimo re-indicizzo lo resuscita (vedi `index_file`).

---

## `src/engine/cache_manager.cpp` — class `CacheManager`

### `bool is_valid(file_path, current_hash)`
Confronta hash salvato (per path) con quello corrente.

### `InvalidationResult invalidate(file_path, repo_id="")` *(DEP-01/DEP-02)*
- `collect_dependents(store, file_path, repo_id, truncated)`: BFS transitiva **sola lettura** sul grafo inverso (edge `include`/`import`, mai `call`), collected **prima** di ogni cancellazione (DEP-01), visited-guard contro i cicli (DEP-02), safety cap `kMaxDependents=1000` (`truncated`).
- `reap_dependent` sul file modificato e su ogni dipendente: DELETE `symbols`, `summaries SET is_valid=0, invalidated_at=now`, `files SET stale=1` — **senza toccare gli edge** (invalidazione lazy: il grafo resta per scoprire i dipendenti al prossimo accesso).
- Ritorna `InvalidationResult{ dependents, truncated }`; un `repo_id` vuoto mantiene la legacy path-only scope.

### `std::vector<std::string> collect_dependents(store, file_path, repo_id, truncated)` *(free function, DEP-03)*
BFS condivisa della propagazione: usata da `CacheManager::invalidate()`, `indexer::index_snapshot()` e `indexer::has_dependents()`. Nessuna implementazione duplicata della traversata (DEP-03).

### `bool reap_dependent(store, repo_id, path)` *(free function, DEP-03)*
`Reap` condiviso della invalidazione (vedi `invalidate`). Ritorna `false` se una scrittura fallisce, così un chiamante dentro una transazione (es. `index_snapshot`) puo' fare ROLLBACK dell'intera invalidazione invece di lasciarne metà applicata.

### `void invalidate_transitive(file_path)` *(deprecato)*
Wrapper di compatibilità: `invalidate()` esegue gia' la raccolta transitiva, quindi nessun ordine di chiamata attorno ad esso conta.

### `std::vector<std::string> get_stale_files()`
Per ogni file `is_deleted=0`: hash a disco ≠ hash DB (o file sparito) → stale. I record tombstonati (`is_deleted=1`) non sono piu' files live da invalidare: la loro conoscenza derivata e' gia' stata rimossa dalla riconciliazione IDX-02 di `index_repository()`.

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
