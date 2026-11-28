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
content_verified_at TEXT             -- ultima verifica dell'hash su disco
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
qualified_name TEXT                      -- SUM-01: nome qualificato (scope + separatore + name)
body_hash  TEXT                          -- SUM-01: SHA-256 delle righe start_line..end_line
```

> **SUM-01 — identità del simbolo.** `qualified_name` è la chiave stabile del simbolo per la conoscenza per-symbol: separatore `::` per C++/CUDA, `.` per Python (come nei parser); `scope` vuoto → nome nudo. `body_hash` è l'hash del *corpo del simbolo* (slice `start_line..end_line` dello stesso snapshot da cui il simbolo è stato estratto, IDX-03), non del file: due funzioni nello stesso file hanno hash indipendenti. `symbol_id` AUTOINCREMENT non è un'identità affidabile (viene ricreato a ogni re-indicizzazione), quindi la conoscenza per-symbol non vi si aggancia mai.

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

### `symbol_summaries` (SUM-01)
```sql
symbol_summary_id INTEGER PRIMARY KEY AUTOINCREMENT
file_id       INTEGER NOT NULL           -- FK logico a files
symbol_key    TEXT NOT NULL              -- qualified_name del simbolo
symbol_type   TEXT NOT NULL              -- tipo del simbolo (partecipa all'identità)
body_hash     TEXT NOT NULL              -- hash del corpo del simbolo alla scrittura
start_line    INTEGER                    -- snapshot della regione alla scrittura
end_line      INTEGER
summary_text  TEXT NOT NULL
model_version TEXT                       -- provenienza: modello che l'ha scritta
prompt_version TEXT                      -- provenienza: versione del prompt
verification_state TEXT DEFAULT 'unverified'
created_at    TEXT NOT NULL
invalidated_at TEXT
is_valid      INTEGER DEFAULT 1
UNIQUE(file_id, symbol_key, symbol_type, body_hash)
```
Identità di una claim per-symbol: `(file_id, symbol_key, symbol_type)`; il `body_hash` distingue le *versioni* della stessa claim. L'UNIQUE composito fa sì che una re-indicizzazione di contenuto invariato non duplichi mai la riga (ON CONFLICT DO UPDATE riattiva la stessa riga); una modifica seguita da rollback riporta il corpo alla versione vecchia e la riga storica corrispondente viene **resuscitata** (`is_valid=1`, `invalidated_at=NULL`) alla ri-indicizzazione.

Validità doppia, speculare a `summaries`: flag `is_valid` **e** `body_hash` della riga == hash del corpo live del simbolo (riga `symbols.body_hash` se presente, altrimenti slice del file live verificato contro `files.content_hash`) — leggere un riassunto richiede che *sia* il file sia il corpo del simbolo coincidano ancora con la versione a cui la claim è stata escritta (`stale_file_index` e `stale_symbol_body` sono stati distinti e comunicati).

### `indexing_state`
```sql
repo_id          TEXT PRIMARY KEY
last_full_index_at TEXT
last_incremental_at TEXT
status           TEXT DEFAULT 'idle'    -- idle|incremental|partial|full|failed
files_indexed    INTEGER DEFAULT 0
files_total      INTEGER DEFAULT 0
full_index_covered INTEGER               -- contatore di file che una scansione full completata ha certificato
coverage_computed_at TEXT               -- quando quella certificazione e' stata misurata
last_error       TEXT                   -- motivo dell'ultima scansione non completata
```

> **IDX-01 — copertura, non presenza.** `status` descrive il *tipo dell'ultima scansione*, mai la copertura. Una riga esiste anche quando e' stato indicizzato un solo file: la presenza di dati indicizzati **non** e' prova di completezza. Solo una scansione `index_repository()` completata scrive `full_index_covered` (quanti record live di file quella scansione si e' impegnata a coprire) e `coverage_computed_at`. Un indice `partial` non puo' rispondere a query repo-level: se il file singolo e' bastato a creare la riga, `status` resta `partial` finche' una scansione completa non certifica la copertura (`full` o `failed`).

> Validita' di una claim `full`: `repo_coverage()` la riverifica prima di permetterle di servire una query repo-level — i record live devono ancora coincidere con il snapshot certificato, nessun record deve essere `stale`, l'insieme dei file su disco deve coincidere con quello tracciato. `content_verified_at` registra quando l'hash del contenuto su disco e' stato confrontato con `content_hash`; se il file ha un mtime successivo a quel timestamp, la copertura confronta nuovamente l'hash prima di dichiarare stale. `last_indexed_at` mantiene invece la semantica di momento in cui e' stata scritta la conoscenza derivata.

> **IDX-04 — verifica del contenuto.** Una scansione completa aggiorna `content_verified_at` anche per i file invariati saltati dal parsing: la scansione puo' quindi certificare `full_fresh` senza riscrivere simboli o dipendenze. Se il timestamp e' meno recente del file, il gate usa l'hash come tie-breaker: stesso hash significa conoscenza ancora valida, hash diverso significa record touched. L'hash tie-breaker e' limitato a 2.000 record per controllo; oltre il budget i record non verificati restano conservativamente touched. Una modifica esterna che preserva o arretra manualmente il mtime rispetto alla verifica puo' sfuggire al gate fino alla scansione successiva, che controlla sempre l'hash.

> **IDX-02 — riconciliazione dei file eliminati.** Una scansione completa confronta i file trovati su disco con i record del repository: i record la cui copia non esiste piu' diventano **tombstone** (`is_deleted=1`, mantenuti per audit) e la loro conoscenza derivata viene rimossa nella stessa transazione (symbols DELETE, summaries `is_valid=0`, dependencies DELETE sui due lati), limitata al `repo_id` corrente e all-or-nothing (ROLLBACK su qualsiasi errore). I **dipendenti** marcati `stale` prima della cancellazione degli edge vengono ri-indicizzati dalla stessa scansione. Un tombstone non e' conoscenza live: `repo_coverage()` lo conteggia come `tombstoned_records` e le query symbol/file/graph lo escludono; se il file torna su disco, `needs_indexing` lo considera da ri-indicizzare e `index_file` lo resuscita (`is_deleted=0`).

> **IDX-03 — hash e conoscenza derivano da un unico snapshot.** La scrittura della conoscenza di un file passa solo da `index_snapshot(store, path, repo_id, content)`: `files.content_hash`, `symbols.content_hash_at_index`, `files.size_bytes`, `files.line_count` e gli edge derivano tutti dagli stessi byte del buffer fornito dal chiamante (hash = `compute_string_hash(content)`, identica a `compute_file_hash` sulla stessa sequenza di byte). Il file non viene riaperto durante l'indicizzazione, quindi non esiste piu' una finestra in cui i simboli descrivono una versione e l'hash un'altra: `files.content_hash` e' sempre l'hash del contenuto da cui ogni `symbols.content_hash_at_index` e' stato derivato.

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
idx_symbols_qualified      (file_id, qualified_name)
idx_symbol_summaries_file  (file_id)
idx_symbol_summaries_lookup (file_id, symbol_key, symbol_type, body_hash)
```

## Il modello di validità hash-driven

Ogni artefatto cache porta con sé l'hash del contenuto al momento della creazione:

| Artefatto | Hash associato | Controllo di freschezza |
|---|---|---|
| `files` | `content_hash` | `needs_indexing()` confronta con hash a disco |
| `symbols` | `content_hash_at_index` | ricreati quando il file è re-indicizzato |
| `dependencies` | (per file) | ricreati quando il file è re-indicizzato |
| `summaries` | `content_hash` | `get_file_summary` ritorna solo se `s.content_hash = f.content_hash` |
| `symbol_summaries` | `body_hash` (hash del *corpo* del simbolo) | `get_symbol_summary` ritorna solo se `ss.body_hash` == hash live del corpo **e** `ss.is_valid = 1`; la validità del file è un prerequisito (`stale_file_index`) |

Flusso di invalidazione (DEP-03: la propagazione vive nel motore, non negli handler):

```
file modificato (edit esterno, Git, altro processo)
  → al prossimo accesso (index mono-file, file_content, summary, code_analysis, edit)
  → index_snapshot(): gate supersessione (hash snapshot ≠ files.content_hash, o tombstone)
      ├ invariato → nessuna BFS, nessun reap (self-heal a costo zero)
      └ cambiato  → collect_dependents(): BFS transitiva SOLO LETTURA sul grafo inverso
                    (dep_type IN ('include','import'), mai 'call') PRIMA di cancellare
      BEGIN
        reap_dependent() per ogni dipendente: symbols DELETE · summaries is_valid=0
                                           · symbol_summaries is_valid=0 (SUM-01)
                                           · files.stale=1 (edge conservati — lazy)
        sostituzione atomica dello snapshot del file modificato
        (invalidazione dei symbol_summaries di QUESTO file + resurrezione delle
         righe il cui body_hash ri-describe un simbolo live invariato)
      COMMIT  (ROLLBACK se una qualunque scrittura fallisce)
  → summaries del file: non più validi (hash mismatch) senza alcuna azione esplicita
  → symbol summaries del file: invalidati come i riassunti di file; quelli il cui
    simbolo è davvero invariato tornano validi alla prima ri-indicizzazione
    (revival hash-driven, senza alcuna azione esplicita)
  → dipendenti stale: ri-indicizzati dal prossimo accesso o dalla passata di
    stabilizzazione della full scan (record `stale=1`, mai una falsa copertura `full`)
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

> **SUM-01 — migrazione pre-SUM-01.** Un database scritto prima di SUM-01 viene aperto e completato in posto: `PRAGMA table_info` aggiunge `symbols.qualified_name` / `symbols.body_hash` (nullable) e crea `symbol_summaries`. `qualified_name` viene backfillato con il separatore giusto per linguaggio (`.` per Python, `::` per gli altri, join su `files.language`); `body_hash` **non** viene backfillato: le righe simbolo pre-SUM-01 restano marcabili (`body_hash_verified: false` nell'inventario), e la decisione di validità resta hash-driven (un body non memorizzato non viene mai *assunto* valido). La prima ri-indicizzazione del file popola la colonna.
