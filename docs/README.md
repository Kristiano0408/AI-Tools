# Documentazione di AI-Tools

Benvenuto. Questa cartella contiene la mappa completa del progetto. Leggila in ordine oppure usa questa pagina come indice per saltare direttamente a ciò che ti serve.

## Cosa è AI-Tools (in 30 secondi)

AI-Tools è un **server MCP (Model Context Protocol) in C++23** che fornisce a un agente AI una conoscenza **persistente e strutturata** di uno o più repository di codice. Non rilegge i file a ogni richiesta: indicizza i repository una volta (con tree-sitter), salva tutto in database SQLite (uno per repository), e riusa quella conoscenza finché gli hash dei contenuti non cambiano.

```
Agente AI  ──JSON-RPC/stdio──▶  server ai-tools  ──▶  motore di conoscenza
                                      │                    │
                                      │                    ├─ parser tree-sitter (C/C++/CUDA/Python)
                                      │                    ├─ grafo dipendenze (file + chiamate simbolo)
                                      │                    ├─ cache hash-driven (SHA-256)
                                      │                    └─ SQLite per repository (~/.ai-tools/)
```

## Indice dei documenti

| Documento | Cosa contiene | Leggi questo se... |
|---|---|---|
| [01-architettura.md](01-architettura.md) | Architettura a livelli, flusso dati end-to-end, ciclo di vita di una richiesta | vuoi capire il sistema nel complesso |
| [02-call-graph.md](02-call-graph.md) | **Grafo di chiamata completo**: da `main()` fino alle foglie, funzione per funzione, chi chiama chi | vuoi navigare il codice seguendo le chiamate |
| [03-moduli.md](03-moduli.md) | **Reference funzione per funzione** di ogni modulo: firma, comportamento, effetti collaterali | cerchi il dettaglio di una specifica funzione |
| [04-data-model.md](04-data-model.md) | Schema SQLite completo, tabelle, indici, come hash e validità funzionano, query di esempio | vuoi interrogare i database o estendere lo schema |
| [05-tools.md](05-tools.md) | I 10 tool MCP: input, output, errori, esempi JSON-RPC reali | vuoi usare il server o scrivere un client |
| [06-build-run-test.md](06-build-run-test.md) | Build, esecuzione, test, troubleshooting, dipendenze | vuoi compilare, eseguire o debuggare |

## Mappa rapida dei file sorgente

```
src/
├── main.cpp                      ← punto di ingresso: init() + run()
├── server/
│   ├── server.cpp/.h             ← trasporto JSON-RPC (stdio), dispatch metodi
│   └── tools.cpp/.h              ← router dei 10 tool, validazione, boundary check
├── engine/
│   ├── repository.cpp/.h         ← identità repository + registry globale
│   ├── knowledge_store.cpp/.h    ← wrapper SQLite (schema, prepared statements)
│   ├── content_hasher.cpp/.h     ← SHA-256 di file e stringhe
│   ├── cache_manager.cpp/.h      ← invalidazione cache (file + transitiva)
│   ├── indexer.cpp/.h            ← orchestratore indicizzazione (full/incrementale)
│   ├── parser.cpp/.h             ← dispatch linguaggio + metadati base + fallback regex
│   ├── parser/
│   │   ├── cpp_parser.cpp/.h     ← AST C/C++ (tree-sitter): simboli + call edges
│   │   ├── cuda_parser.cpp/.h    ← AST CUDA: kernel __global__/__device__
│   │   └── python_parser.cpp/.h  ← AST Python: classi, def, async def
│   ├── dependency_graph.cpp/.h   ← query grafo: file e call graph simboli
│   ├── symbol_index.cpp/.h       ← query sui simboli indicizzati
│   └── summary_store.cpp/.h      ← riassunti semantici con validità hash
└── utils/
    ├── path_utils.cpp/.h         ← confine repository, normalizzazione path
    └── file_utils.cpp/.h         ← I/O file, lettura righe, walk directory

tests/
├── test_main.cpp                 ← runner aggregato (main dei test)
├── test_parsers.cpp              ← parser + indexer end-to-end
├── test_store.cpp                ← store / hasher / cache / repository
└── test_tools.cpp                ← tool layer + server JSON-RPC

third_party/
├── tree-sitter/                  ← runtime tree-sitter (libreria statica prebuilt)
├── grammars/                     ← parser.c pregenerati: c, cpp, python
└── ankerl-unordered_dense/       ← mappe hash veloci header-only
```

## Lo stato persistente su disco

```
~/.ai-tools/
├── registry.db                   ← registro globale: repo_id → root_path (identità)
└── repos/
    └── <repo_id>/                ← repo_id = primi 16 hex char di SHA-256(root_path canonico)
        └── knowledge.db          ← files, symbols, dependencies, summaries, indexing_state
```

## Percorso di lettura consigliato

1. **01-architettura.md** — 10 minuti, dà il quadro.
2. **02-call-graph.md** — il cuore della documentazione: ogni cammino da `main()` a ogni funzione foglia.
3. **03-moduli.md** — consultazione: quando nel call graph vedi una funzione e vuoi sapere esattamente cosa fa.
4. **04-data-model.md** — se ti interessano i dati persistiti.
5. **05-tools.md** — se usi il server come utente.
6. **06-build-run-test.md** — se devi compilare o estendere.
