# 05 — I 10 tool MCP: specifiche e esempi reali

## Formato del canale

Il server legge JSON-RPC 2.0 da **stdin** (una riga = un messaggio) e scrive su **stdout**. Ogni risposta ecoa l'`id`.

Chiamare un tool:
```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"<tool>","arguments":{...}}}
```

Risposta (payload MCP standard):
```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"<JSON stringificato>"}],"isError":false}}
```
`isError:true` = errore del tool (il testo è il messaggio). Errori di trasporto: `-32600` (richiesta malformata), `-32601` (metodo sconosciuto), `-32602` (parametri mancanti).

Convenzione repository: quasi tutti i tool accettano `repo_id`; se manca, molti accettano `path` da cui deducono la root. Se serve un repository e non è risolvibile → errore del tool.

---

## 1. `index` — indicizzazione full o incrementale

**Input**: `repo_id` **oppure** `path` (root del repo; crea il repository se nuovo). Opzionale: `file_path` (file specifico).
**Output**: `{repo_id, status:"indexed", files_indexed, files_total}` oppure `{indexed:"<path>"}`.

```bash
# Prima indicizzazione di un repository (deduce repo_id dal path)
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"index","arguments":{"path":"/home/me/progetto"}}}
# → {"files_indexed":37,"files_total":903,"repo_id":"a386c17ff522c568","status":"indexed"}

# Re-run: riprocessa solo i file cambiati
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"index","arguments":{"repo_id":"a386c17ff522c568"}}}
# → {"files_indexed":0,...}   ← nessun file riprocessato (tutti freschi)

# Solo un file
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"index","arguments":{"repo_id":"a386c17ff522c568","file_path":"src/engine/indexer.cpp"}}}
```

**Note**: estensioni supportate `.c .cpp .cc .cxx .h .hpp .cu .cuh .py`; skip `build/`, `.git/`, `node_modules/`, `third_party/`. Un file non supportato non è indicizzato ma è contato in `files_total`.

---

## 2. `file_content` — inspect di un file

**Input**: `file_path` (assoluto o relativo alla root), `repo_id`, `start_line?`, `end_line?` (1-based, inclusivo).
**Output**: `path, language, file_type, size_bytes, line_count, content_hash, index_status ("fresh"|"stale"), symbols[], summary?, content, returned_lines`.

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"file_content","arguments":
  {"repo_id":"a386c17ff522c568","file_path":"src/engine/indexer.h","start_line":1,"end_line":10}}}
```
Risposta reale:
```json
{"path":"/home/franco/Documents/GitHub/AI-Tools/src/engine/indexer.h",
 "content":"#pragma once\n\n#include \"parser.h\"\n...","returned_lines":10,
 "language":"cpp","file_type":"header","size_bytes":544,"line_count":19,
 "content_hash":"d868b85c...","index_status":"fresh",
 "symbols":[{"name":"indexer","type":"namespace","start_line":8,"end_line":19,"scope":""},
            {"name":"index_file","type":"function","start_line":11,"end_line":11,"scope":"indexer"},
            {"name":"index_repository","type":"function","start_line":14,"end_line":14,"scope":"indexer"},
            {"name":"needs_indexing","type":"function","start_line":17,"end_line":17,"scope":"indexer"}]}
```
**Errori**: `start_line < 1`, `end_line < start_line`, path fuori confine, file inesistente.

---

## 3. `repository_search` — ricerca file + simboli

**Input**: `query` (obbligatorio), `repo_id?`, `scope?` (sottotree directory), `file_type?`.
**Output**: `{files:[{path,language,line_count}], symbols:["name (type) in path:line", ...]}`.
Senza `repo_id` cerca in **tutti** i repository registrati.

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"repository_search","arguments":{"query":"indexer"}}}
```
Risposta reale (cross-repo):
```json
{"files":[{"path":".../src/engine/indexer.cpp","language":"cpp","line_count":189},
          {"path":".../src/engine/indexer.h","language":"cpp","line_count":19}],
 "symbols":["indexer (namespace) in .../src/engine/indexer.cpp:10",
            "indexer (namespace) in .../src/engine/indexer.h:8"]}
```
**Nota**: la ricerca è su path e simboli indicizzati, **non** sul contenuto dei file (limitazione nota).

---

## 4. `symbol_search` — dettagli simbolo + callers

**Input**: `name` (obbligatorio), `repo_id?`.
**Output**: `{symbols:[blocco testo con signature/file/scope], callers:["symbol (file)", ...]}`.

```json
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"symbol_search","arguments":
  {"repo_id":"a386c17ff522c568","name":"index_repository"}}}
```
Risposta reale:
```json
{"symbols":["index_repository (function)\n  Signature: void index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id)\n  File: .../src/engine/indexer.cpp:127-187\n  Scope: indexer",
            "index_repository (function)\n  Signature: void index_repository(...);\n  File: .../src/engine/indexer.h:14-14\n  Scope: indexer"],
 "callers":[...]}
```

---

## 5. `dependency_graph` — grafo file o call graph simboli

**Input**: `file_path` **oppure** `symbol`; `repo_id`; `direction` (`in`|`out`|`both`, default `both`); `depth` (default 1; >1 = BFS transitivo, solo file-level).
**Output file**: `{dependencies:[...], dependents:[...]}`. **Output symbol**: `{symbol, callees:[...], callers:[...]}`.

```json
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"dependency_graph","arguments":
  {"repo_id":"a386c17ff522c568","file_path":"src/engine/indexer.cpp","direction":"out"}}}
```
→ dipendenze include del file (target risolti a path reali quando esistono).

```json
{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"dependency_graph","arguments":
  {"repo_id":"a386c17ff522c568","symbol":"compute_file_hash","direction":"in"}}}
```
Risposta reale:
```json
{"symbol":"compute_file_hash",
 "callers":["indexer::needs_indexing (.../src/engine/indexer.cpp)",
            "indexer::index_file (.../src/engine/indexer.cpp)",
            "tools::call_tool (.../src/server/tools.cpp)",
            "run_store_tests (.../tests/test_store.cpp)"]}
```
**Note**: i path relativi sono normalizzati ai path canonici assoluti usati nel DB; i match dei simboli accettano nome bare (`helper`) o qualificato (`indexer::helper`).

---

## 6. `edit` — modifica lineare sicura

**Input**: `file_path`, `repo_id`, `start_line`, `end_line`, `new_content` (tutti obbligatori).
**Effetti**: sostituisce esattamente le righe `[start,end]`; scrive il file; invalida cache; re-indicizza il file; se header invalida transitivamente i dipendenti.
**Output**: `{path, replaced_lines:[start,end], new_line_count, cache_invalidated:true}`.

```json
{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"edit","arguments":
  {"repo_id":"a386c17ff522c568","file_path":"src/engine/indexer.h","start_line":11,"end_line":11,
   "new_content":"void index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);"}}}
```
**Errori**: `start_line<1`, `end_line<start_line`, `start_line` oltre EOF, path fuori confine.

---

## 7. `execute` — build/test nella root del repo

**Input**: `command` (obbligatorio), `repo_id` (**obbligatorio** — la cwd è la root del repo, mai un path arbitrario).
**Output**: `{command, working_directory, exit_code, duration_ms, stdout, stderr}`.

```json
{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"execute","arguments":
  {"repo_id":"a386c17ff522c568","command":"cmake --build build -j"}}}
```
**Note**: stdout/stderr separati; exit code non-zero è un **risultato**, non un errore del tool (gli errori non sono mai inghiottiti).

---

## 8. `summary` — riassunti semantici

**Input**: `repo_id` (obbligatorio), `file_path?`, `write?` (testo da memorizzare).
**Output lettura**: `{path|repo_id, summary, valid}` — il riassunto è restituito solo se l'hash del file coincide con quello della scrittura.
**Output scrittura**: `{stored:true}`.

```json
{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"summary","arguments":
  {"repo_id":"a386c17ff522c568","file_path":"src/engine/indexer.cpp","write":"Orchestratore di indicizzazione full e incrementale"}}}
```
Dopo una modifica al file, la stessa lettura ritorna `{"summary":"","valid":false}`.

---

## 9. `file_tree` — struttura directory

**Input**: `path` (obbligatorio, directory), `max_depth?` (default 4).
**Output**: `{path, tree:[{name,type:"directory",children:[...]} | {name,type:"file"}]}` — directory prima, poi ordine alfabetico. Skip `.git`, `build`, `node_modules`, `__pycache__`.

```json
{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"file_tree","arguments":
  {"path":"/home/me/progetto","max_depth":2}}}
```

---

## 10. `code_analysis` — statistiche file

**Input**: `file_path` (obbligatorio), `repo_id?`.
**Output**: `{file, total_lines, code_lines, comment_lines, blank_lines, symbols:{tipo:conteggio,...}}`.

```json
{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"code_analysis","arguments":
  {"repo_id":"a386c17ff522c568","file_path":"src/engine/knowledge_store.cpp"}}}
```

---

## Test boundary e sicurezza (comportamento verificato)

| Tentativo | Esito |
|---|---|
| `file_content` con `../../etc/passwd` | errore del tool: "Path escapes repository boundary" |
| `execute` senza `repo_id` | errore: richiede repo_id |
| `edit` con `start_line=3,end_line=1` | errore: range invalido |
| Tool sconosciuto | errore: "Unknown tool: ..." |
| Richiesta non-JSON | JSON-RPC error `-32600` |
| Metodo inesistente | JSON-RPC error `-32601` |

## Script di smoke-test rapido

```bash
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
 '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"index","arguments":{"path":"'$PWD'"}}}' \
 | ./build/ai-tools
```
