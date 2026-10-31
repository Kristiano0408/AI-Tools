# AI-Tools — Cose da Completare & Roadmap

Documento riassuntivo delle attività necessarie e consigliate prima di rendere pubblica la repository su GitHub e per completare l'implementazione del server MCP.

---

## 1. Bloccanti per il Rilascio Pubblico su GitHub

### Licenza e Aspetti Legali
- [x] **Aggiungere un file `LICENSE`**: aggiunto file di licenza **MIT**.
- [x] **Verifica attribuzioni terze parti (`THIRD_PARTY_NOTICES.md`)**: creato documento con attribuzioni per Tree-sitter, grammatiche (C, C++, Python), nlohmann/json e ankerl::unordered_dense.

### Portabilità & Build System (`CMakeLists.txt`)
- [x] **Rimuovere il binario precompilato Linux**:
  - Rimosso `third_party/tree-sitter/usr/lib/x86_64-linux-gnu/libtree-sitter.a`.
  - Integrati i sorgenti C di Tree-sitter (`third_party/tree-sitter/src/`) e configurata la compilazione da sorgente tramite CMake (`add_library(tree-sitter STATIC ...)`).
  - Piattaforme supportate: **Linux x86_64, Linux ARM64, macOS (Apple Silicon / Intel)**.
- [x] **Snellire la cartella `third_party/`**:
  - Eliminata la documentazione Debian e gli esempi (`third_party/usr/share/doc/`).
  - Eliminati i binding inutilizzati (Rust, Swift, Go, Zig, Python) dalle grammatiche, mantenendo i sorgenti del parser e i file di licenza.

### CI/CD (GitHub Actions)
- [x] **Creare `.github/workflows/ci.yml`**:
  - Matrix build automatizzato su Ubuntu (`ubuntu-latest`) con GCC e Clang e su macOS (`macos-latest`) con Clang.
  - Esecuzione automatica dei test (`./build/run_tests`).

### Script di Installazione (`install.sh`)
- [x] **Supporto multipiattaforma per il file di config di VS Code**:
  - Linux: `${XDG_CONFIG_HOME:-~/.config}/Code/User/mcp.json`
  - macOS: `~/Library/Application Support/Code/User/mcp.json`
  - Windows/MSYS/WSL: `%APPDATA%/Code/User/mcp.json`
- [x] **Opzione dry-run / conferma**:
  - Aggiunto flag `--dry-run` per visualizzare le operazioni senza modificare file o configurazioni.
  - Aggiunto prompt di conferma interattivo (bypassabile con `-y` / `--yes`).

---

## 2. Funzionalità Mancanti del Protocollo MCP (Model Context Protocol)

Attualmente il server implementa solo l'interfaccia **Tools**. Per essere conforme e sfruttare appieno le specifiche MCP:

### MCP Resources (`resources/*`)
- [ ] Implementare `resources/list` e `resources/read`.
- [ ] Esporre i file del repository come risorse URI (es. `file://<rel_path>` o `repo://<repo_id>/file/<path>`).
- [ ] Esporre i riassunti semantici come risorse (es. `repo://<repo_id>/summary`).
- [ ] Permette agli agenti AI (es. Claude Desktop, Cursor) di agganciare il contesto della codebase senza consumare chiamate di tool.

### MCP Prompts (`prompts/*`)
- [ ] Implementare `prompts/list` e `prompts/get`.
- [ ] Fornire template di prompt pronti all'uso per gli agenti, ad esempio:
  - `explain-architecture`: riassume architettura e moduli principali.
  - `impact-analysis`: analizza cosa rischia di rompersi modificando un determinato file/simbolo.
  - `refactor-symbol`: guida alla modifica coerente di un'interfaccia/simbolo.

### Progress Reporting (`notifications/progress`)
- [ ] Inviare notifiche di avanzamento durante l'operazione `index` su progetti di grandi dimensioni (evitando che l'agente vada in timeout credendo che il server sia bloccato).

### Workspace Roots (`roots/*`)
- [ ] Implementare `roots/list` e ascoltare `notifications/roots/list_changed`.
- [ ] Rilevare automaticamente le directory di lavoro aperte nel client/IDE senza dover passare manualmente percorsi o dipendere solo dalla directory corrente (`cwd`).

### Validazione Input & Gestione Errori MCP
- [ ] Validare a monte la presenza e il tipo dei parametri obbligatori (`inputSchema`).
- [ ] Restituire codice di errore JSON-RPC standard `-32602 (Invalid params)` quando un tipo o parametro obbligatorio manca, anziché l'errore generico `-32600 (Invalid request)`.
- [ ] **Bug (scoperto nei test MCP end-to-end): `repository_search` con `repo_id`**:
  - Una query di path (es. `helper`) ritorna `files: []` a meno che `scope` non sia passato come path assoluto (`/abs/path/src`).
  - Con scope relativo (`src`) o assente il match dei file non funziona.
  - *Fix*: confrontare la query anche contro il path relativo al repository root, e risolvere `scope` relativo rispetto alla root del repo.
- [ ] **Bug (scoperto nei test MCP end-to-end): `edit` con `end_line` fuori range**:
  - `end_line` oltre EOF viene clampato silenziosamente invece di essere rifiutato (il README dichiara "Invalid line ranges are rejected").
  - *Fix*: restituire errore `-32602 (Invalid params)` quando `start_line`/`end_line` eccedono il numero di righe del file.

---

## 3. Ottimizzazioni Performance (Hot Paths)

Identificate dall'analisi di profilazione (`AI_TOOLS_DEBUG=ON`):

- [ ] **Batch Insert su `dependencies`**:
  - Sostituire le ~2000 query `INSERT INTO dependencies` singole con un'unica transazione batch con insert multipli (`INSERT INTO ... VALUES (...), (...), ...`).
- [ ] **Transazioni SQLite raggruppate per l'indicizzazione**:
  - Evitare `BEGIN` / `COMMIT` per ciascuno dei file; raggruppare l'intero processo di indicizzazione in un'unica transazione (o batch da 50-100 file).
- [ ] **Ottimizzazione query `file_content`**:
  - Unificare le 3 query separate (metadata, simboli, summary) e il calcolo hash tramite un'unica query con `LEFT JOIN`.
- [ ] **Cache in-memory degli hash**:
  - Mantenere una cache in-memory `path -> sha256` con validazione veloce del `mtime` (last modified) per evitare di ricalcolare l'hash SHA-256 su disco ad ogni query su file non modificati.
- [ ] **Cache connessione SQLite**:
  - Evitare di riaprire `registry.db` e il DB del repository ad ogni singola chiamata di tool (`lookup_repository`).

---

## 4. Estensioni Funzionali dell'Engine

- [ ] **Supporto Linguaggi Aggiuntivi**:
  - Aggiungere grammatiche per linguaggi diffusi: JavaScript / TypeScript, Rust, Go.
- [ ] **Miglioramento Call Graph (Risoluzione Chiamate C++)**:
  - Gestire meglio i metodi sovraccarichi (overloading) e la risoluzione dei tipi per le chiamate polimorfiche.
- [ ] **Cancellazione richieste (`notifications/cancelled`)**:
  - Gestire la notifica di cancellazione se l'agente o l'utente interrompe una ricerca o un'indicizzazione lunga.

