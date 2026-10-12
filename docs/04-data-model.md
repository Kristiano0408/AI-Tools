# 04 — Data model: SQLite, hash e validità

## Dove vivono i dati

```
~/.ai-tools/   # data dir: $AI_TOOLS_DATA_DIR > default compilato (-DAI_TOOLS_DATA_DIR) > $HOME/.ai-tools
├── registry.db                     registry globale dei repository
└── repos/<repo_id>/knowledge.db    database di conoscenza per repository
```

- `repo_id` = primi 16 hex di `SHA-256(path assoluto canonico della root)`.
- Ogni `knowledge.db` è indipendente: nessun dato di un repository può finire in un altro (isolamento).
- WAL attivo (`journal_mode=WAL`), `busy_timeout=5000`, `foreign_keys=ON`.

## Schema (creato da `KnowledgeStore::init_schema`)

### `repositories`
```sql
repo_id   TEXT PRIMARY KEY      -- identità stabile (hash del root path)
root_path TEXT UNIQUE NOT NULL  -- path canonico
name      TEXT NOT NULL         -- basename della root
created_at TEXT NOT NULL        -- ISO-8601 UTC
updated_at TEXT NOT NULL
```
Replicata sia nel registry globale sia in ogni `knowledge.db` (l'indexer risolve `repo_id → root_path` dal DB locale).

### `files`
```sql
file_id        INTEGER PRIMARY KEY AUTOINCREMENT
repo_id        INTEGER NOT NULL
path           TEXT NOT NULL           -- path assoluto canonico
content_hash   TEXT NOT NULL           -- SHA-256 al momento dell'indicizzazione
language       TEXT                    -- c|cpp|cuda|python|...
file_type      TEXT                    -- source|header|cuda|other
size_bytes     INTEGER DEFAULT 0
line_count     INTEGER DEFAULT 0
last_indexed_at TEXT
is_binary      INTEGER DEFAULT 0
is_deleted     INTEGER DEFAULT 0
UNIQUE(repo_id, path)
```

### `symbols`
```sql
symbol_id  INTEGER PRIMARY KEY AUTOINCREMENT
file_id    INTEGER NOT NULL              -- FK logico a files
name       TEXT NOT NULL
symbol_type TEXT                         -- namespace|class|struct|union|enum|function|variable|typedef|using|kernel|device_function|host_function|shared_variable|decorator
signature  TEXT
start_line INTEGER NOT NULL              -- 1-based
end_line   INTEGER NOT NULL
scope      TEXT                          -- es. "app::Widget", "Config", "" = top level
is_declaration INTEGER DEFAULT 0         -- prototipo / dichiarazione
is_definition  INTEGER DEFAULT 0         -- corpo presente
is_exported    INTEGER DEFAULT 0         -- non ancora popolato (limitazione nota)
content_hash_at_index TEXT               -- hash del file quando il simbolo è stato estratto
```

### `dependencies`
```sql
dep_id        INTEGER PRIMARY KEY AUTOINCREMENT
repo_id       INTEGER NOT NULL
source_path   TEXT NOT NULL              -- file che dipende / che chiama
target_path   TEXT NOT NULL              -- file dipeso (per 'call': lo stesso file)
source_symbol TEXT                       -- per 'call': caller qualificato (es. "indexer::index_file")
target_symbol TEXT                       -- per 'call': callee (es. "compute_file_hash")
dep_type      TEXT                       -- 'include' | 'import' | 'call'
dep_direction TEXT                       -- 'out'
```
Regola di lettura:
- edge **file-level**: `dep_type IN ('include','import')` — `get_dependencies`/`get_dependents` escludono `'call'`.
- edge **symbol-level (call graph)**: `dep_type = 'call'` — interrogati da `get_callers`/`get_callees` su `source_symbol`/`target_symbol`.

### `summaries`
```sql
summary_id    INTEGER PRIMARY KEY AUTOINCREMENT
file_id       INTEGER NOT NULL           -- 0 = sentinella riassunto repository-level
content_hash  TEXT NOT NULL              -- hash del file alla scrittura (per repo: repo_id)
summary_text  TEXT NOT NULL
created_at    TEXT NOT NULL
invalidated_at TEXT
is_valid      INTEGER DEFAULT 1
```
Validità doppia: flag `is_valid` **e** join con `files.content_hash` corrente in `get_file_summary` — un riassunto è servito solo se l'hash combacia.

### `indexing_state`
```sql
repo_id          TEXT PRIMARY KEY
last_full_index_at TEXT
last_incremental_at TEXT
status           TEXT DEFAULT 'idle'    -- idle|incremental|full
files_indexed    INTEGER DEFAULT 0
files_total      INTEGER DEFAULT 0
```

### Indici
```
idx_files_repo_path        (repo_id, path)
idx_files_content_hash     (content_hash)
idx_symbols_file           (file_id)
idx_symbols_name           (name)
idx_deps_repo              (repo_id)
idx_deps_source            (source_path)
idx_deps_target            (repo_id, target_path)
idx_deps_type              (repo_id, dep_type)
idx_deps_source_symbol     (repo_id, source_symbol)
idx_deps_target_symbol     (repo_id, target_symbol)
idx_summaries_file         (file_id)
idx_summaries_hash         (content_hash)
```

## Il modello di validità hash-driven

Ogni artefatto cache porta con sé l'hash del contenuto al momento della creazione:

| Artefatto | Hash associato | Controllo di freschezza |
|---|---|---|
| `files` | `content_hash` | `needs_indexing()` confronta con hash a disco |
| `symbols` | `content_hash_at_index` | ricreati quando il file è re-indicizzato |
| `dependencies` | (per file) | ricreati quando il file è re-indicizzato |
| `summaries` | `content_hash` | `get_file_summary` ritorna solo se `s.content_hash = f.content_hash` |

Flusso di invalidazione:

```
file modificato
  → hash corrente ≠ files.content_hash
  → needs_indexing == true
  → al prossimo index/inspect: re-parse e sostituzione atomica (transazione)
  → se header: invalidate_transitive → dipendenti marcati stale (lazy)
  → summaries del file: non più validi (hash mismatch) senza alcuna azione esplicita
```

## Query di esempio (interrogare i DB a mano)

```sql
-- Chi chiama compute_file_hash (call graph)
SELECT DISTINCT source_symbol, source_path FROM dependencies
WHERE repo_id = :repo AND dep_type = 'call'
  AND (target_symbol = 'compute_file_hash' OR target_symbol LIKE '%compute_file_hash%');

-- Cosa chiama indexer::index_file
SELECT DISTINCT target_symbol FROM dependencies
WHERE repo_id = :repo AND dep_type = 'call'
  AND (source_symbol = 'indexer::index_file' OR source_symbol LIKE '%indexer::index_file%');

-- Chi include un header
SELECT source_path FROM dependencies
WHERE repo_id = :repo AND dep_type = 'include' AND target_path = :header_path;

-- File stale (hash cambiato) — confronto applicativo, non SQL puro
SELECT path, content_hash FROM files WHERE is_deleted = 0;
-- poi: hash a disco != content_hash → stale

-- Simboli di un file
SELECT name, symbol_type, start_line, end_line, scope FROM symbols
WHERE file_id = (SELECT file_id FROM files WHERE repo_id = :repo AND path = :file)
ORDER BY start_line;
```

## Legacy migration

`init_schema()` gestisce database creati con lo schema precedente (dove `repositories.repo_id` era `INTEGER AUTOINCREMENT`): rileva le colonne via `PRAGMA table_info` e migra. Un DB legacy con vecchie tabelle non è compatibile con le identità testuali: cancellare `~/.ai-tools` e ri-indicizzare.
