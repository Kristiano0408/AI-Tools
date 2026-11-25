# AI-Tools --- Registro delle problematiche tecniche

**Repository:** https://github.com/Kristiano0408/AI-Tools\
**Ambito:** revisione statica della versione pubblica `master` esaminata
durante la code review.\
**Scopo:** raccogliere le problematiche, i rischi e i miglioramenti
individuati, organizzandoli per priorità.

> **Nota sulla verifica:** questo documento riporta i rilievi emersi
> dall'ispezione statica dei sorgenti. Non è stata eseguita in questa
> sede una compilazione completa né la suite di test. I punti descritti
> come bug sono quindi problemi dedotti dal flusso del codice, da
> confermare con test di regressione prima della correzione. I
> suggerimenti architetturali sono distinti dai difetti osservati.

> **Addendum (2026-11-25):** il rilievo IDX-04 nasce invece da una
> prova runtime del server MCP in esecuzione e dal riscontro diretto
> sul database di conoscenza persistito (`knowledge.db` per
> repository). Per quel punto la dinamica è stata riprodotta e
> verificata sui dati reali, non solo dedotta dall'ispezione statica.

------------------------------------------------------------------------

# 1. Riepilogo delle priorità

| ID     | Priorità                            | Area               | Problema |
|--------|-------------------------------------|--------------------|----------|
| IDX-01 | Risolto                             | Indicizzazione     | Un'indicizzazione parziale può essere interpretata come indicizzazione completa |
| DEP-01 | Risolta                             | Dipendenze/cache   | I dipendenti vengono raccolti prima delle modifiche agli archi |
| DEP-02 | Risolta                             | Dipendenze/cache   | Propagazione BFS ricorsiva con protezione dai cicli e limite esplicito |
| DEP-03 | Alta                                | Dipendenze/cache   | Le modifiche esterne possono aggiornare un file senza invalidare i suoi dipendenti |
| IDX-02 | Risolta                             | Indicizzazione     | I file eliminati possono restare nell'indice |
| DB-01  | Alta                                | SQLite             | Gli errori nelle operazioni di indicizzazione non sono propagati/gestiti in modo robusto |
| IDX-03 | Risolta                             | Consistenza        | Parser e calcolo hash leggono il file in momenti distinti |
| IDX-04 | Risolto                             | Indicizzazione     | Freshness per contenuto e diagnostica esplicita se il gate resta stale dopo la scansione |
| SUM-01 | Alta rispetto alla visione prodotto | Knowledge store    | Le summary sono a livello di file, non di funzione/simbolo |
| SUM-02 | Media                               | Knowledge store    | La repository summary non è legata a una versione effettiva dei contenuti |
| MCP-01 | Media                               | Tool API           | Alcuni parametri dichiarati nello schema non vengono applicati |
| MCP-02 | Media                               | Tool API           | La descrizione di alcune ricerche non corrisponde al comportamento effettivo |
| MCP-03 | Media                               | Context efficiency | `file_content` restituisce il file intero se non vengono specificati intervalli |
| PAR-01 | Media                               | Parser             | Il fallback regex per linguaggi non riconosciuti non risulta raggiungibile nel flusso ordinario |
| PAR-02 | Media                               | Call graph         | Le chiamate sono risolte principalmente per nome e possono essere ambigue |
| PAR-03 | Media                               | Call graph         | La ricerca SQL con `LIKE` può produrre corrispondenze imprecise |
| FS-01  | Media                               | Filesystem         | La scansione incrementale continua a dover attraversare e verificare i file |
| FS-02  | Media                               | Filesystem         | Le esclusioni delle directory non equivalgono a una gestione completa di `.gitignore` |
| SEC-01 | Alta se usato con agenti non fidati | Sicurezza          | Il tool `execute` usa `std::system` e non costituisce un sandbox |
| SEC-02 | Bassa/Media                         | Sicurezza          | Interpretazione dell'exit status senza verificare prima il tipo di terminazione |

------------------------------------------------------------------------

# 2. Indicizzazione e consistenza dell'indice - DONE

## IDX-01 --- Indicizzazione parziale interpretata come completa

**Priorità: Critica**

### Comportamento osservato

Il percorso di indicizzazione di un singolo file aggiorna
`indexing_state`. La funzione `ensure_repo_indexed()` verifica la
presenza di una riga in tale tabella e, se la trova, può considerare il
repository già indicizzato.

Il controllo non distingue chiaramente tra: - repository sottoposto a
indicizzazione completa; - repository in cui è stato indicizzato
soltanto un file; - indicizzazione completa ancora in corso o fallita.

### Scenario problematico

1.  Un agente richiede informazioni su un file.
2.  AI-Tools indicizza soltanto quel file.
3.  Viene creata la riga in `indexing_state`.
4.  In seguito l'agente esegue una ricerca globale.
5.  `ensure_repo_indexed()` trova la riga e può saltare il full index.
6.  La ricerca opera su un indice incompleto e può restituire risultati
    parziali senza segnalarlo.

### Intervento suggerito

Rappresentare esplicitamente lo stato e la copertura dell'indice. Per
esempio: - `full_index_completed`; - `indexing_in_progress`; -
`last_full_index_at`; - stato di eventuali indicizzazioni
incrementali; - eventuale errore dell'ultima scansione.

La presenza di dati indicizzati non deve essere usata come prova della
completezza dell'indice.

### Test di regressione

-   Creare un repository con più file.
-   Indicizzare un solo file.
-   Richiedere una ricerca globale.
-   Verificare che venga avviata o richiesta una scansione completa.
-   Verificare che un indice parziale sia identificabile come tale.

------------------------------------------------------------------------

## IDX-02 --- I file eliminati possono restare nell'indice DONE 

**Priorità: Alta**

### Comportamento osservato

Lo schema prevede `is_deleted`, ma nel flusso di `index_repository()`
esaminato non risulta una fase completa di riconciliazione tra: - file
presenti nel filesystem; - file registrati nel database.

### Conseguenze

Dopo la cancellazione di un file dal filesystem, il database può
conservare: - record del file; - simboli estratti; - dipendenze; -
summary; - risultati di ricerca riferiti a un percorso inesistente.

Questo può indurre l'agente a credere che una funzione o un file esista
ancora.

### Intervento suggerito

Durante una scansione completa: 1. raccogliere i file effettivamente
trovati; 2. confrontarli con i record del repository; 3. marcare come
eliminati i file non più presenti; 4. rimuovere o invalidare simboli,
dipendenze e summary associate; 5. mantenere eventualmente tombstone per
audit/debug.

La rimozione deve essere transazionale e non deve cancellare dati di
altri repository.

### Test di regressione

Indicizzare un file, eliminarlo dal filesystem, eseguire una nuova
scansione e verificare che non compaia più nelle ricerche e nel grafo.

------------------------------------------------------------------------

## IDX-03 --- Hash e parsing possono riferirsi a versioni diverse del file DONE

**Priorità: Media**

> **Risolto.** Il flusso di indicizzazione ora fissa il contenuto in un unico
> buffer: `indexer::index_snapshot(store, file_path, repo_id, content)` è l'unico
> punto che scrive la conoscenza derivata di un file e deriva hash (`compute_string_hash`),
> parsing (`parse_file(path, language, content)`), simboli, dipendenze, `size_bytes` e
> `line_count` tutti dallo stesso snapshot; `index_file()` legge il file una sola
> volta e delega. Il loop di `index_repository()` consuma l'hash già letto da
> `needs_indexing()` e rilegge al massimo una volta (retry bounded) se il file è
> cambiato tra confronto e lettura. Coperto da `tests/test_snapshot_consistency.cpp`
> (invariante deterministica + modifica concorrente con writer thread, voce T-12).

### Comportamento osservato

Il flusso esaminato esegue il parsing del percorso e successivamente
calcola l'hash rileggendo il file.

### Rischio

Se il file viene modificato tra le due letture: - il parser può produrre
simboli dalla versione A; - il database può associare quei simboli
all'hash della versione B.

L'indice risulterebbe internamente incoerente.

### Intervento suggerito

Leggere il contenuto una sola volta in un buffer immutabile e utilizzare
lo stesso snapshot per: - calcolo dell'hash; - parsing; - estrazione di
simboli e dipendenze; - eventuale estrazione di summary o metadati.

In alternativa, verificare che il file non sia cambiato durante
l'elaborazione e ripetere l'operazione se necessario.

### Test di regressione

Simulare una modifica concorrente del file durante l'indicizzazione e
verificare che hash e dati estratti appartengano sempre allo stesso
contenuto.

------------------------------------------------------------------------

## IDX-04 --- Reindicizzazione completa che non raggiunge mai `full_fresh`: tool di ricerca a livello di repository silenziosamente vuoti

**Priorità: Alta**

> **Risolto (2026-11-25).** Aggiunto `files.content_verified_at`, distinto da
> `last_indexed_at`: le scansioni complete marcano anche i file invariati
> verificati dall'hash. `touched_records()` usa il timestamp come fast path e,
> quando il file e' piu' recente o il marcatore manca, confronta l'hash corrente
> con `content_hash` prima di dichiarare stale (limite 2.000 hash per controllo).
> Il gate espone i record touched e, se una scansione termina ma la copertura
> resta non fresca, risponde con `index_status=index_stale_after_scan` e un hint
> invece di un envelope vuoto. Regressioni: test store IDX-04 e test MCP in
> `tests/test_tools.cpp`; build e suite complete passano.

**Verificato runtime:** questo punto non è dedotto solo dall'ispezione
statica. È stato riprodotto interrogando il server MCP in esecuzione e
confrontando i record del database persistito con lo stato reale dei
file su disco.

### Comportamento osservato (prima del fix)

Interrogando il server MCP con i tool che leggono l'indice a livello di
repository (`symbol_search`, `repository_search`, `dependency_graph`),
la chiamata esegue la gate-verification prevista da IDX-01: quando la
copertura non è `full_fresh`, `ensure_repo_indexed()` avvia una
scansione completa e poi rivalida la copertura. La scansione termina
con esito positivo — lo stato nel database riporta `status=full` e un
snapshot di copertura coerente (tutti i file indexati, nessuno stale) —
ma il calcolo di copertura immediatamente successivo alla scansione
ritorna di nuovo `index_stale`. L'handler dei tool di ricerca esce di
fronto al gate e restituisce un envelope privo di risultati: contiene
solo `repo_id`, `index_status` e `index_coverage`, senza `files`, senza
`symbols` e senza `callers`.

Il risultato è una loop invisibile: ogni query a livello di repository
riesegue una scansione completa, la scansione non migliora mai la
copertura, la query ritorna vuota. Nessun errore viene segnalato; il
tool sembra semplicemente non trovare nulla. Simmetricamente, i tool
per-file (`file_content`, `summary`) continuano a funzionare, perché
non passano dal gate di copertura repository: la loro disponibilità può
mascherare il problema.

### Dinamica verificata sul database (prima del fix)

Confronto record per record tra `last_indexed_at` nel DB e mtime su
disco per i file vivi non stale dell'indice del repository in esame:
11 file risultano con mtime **più recente** del proprio
`last_indexed_at`, pur avendo `content_hash` identico all'hash SHA-256
calcolato sui byte attuali su disco. In altri termini il contenuto di
quei file non è cambiato; è solo più vecchio del timestamp scritto
dall'ultima scansione che li ha indicati.

Il meccanismo che produce l'incoerenza è questo:

1.  La scansione completa è dichiaratamente incrementale (IDX-03):
    `needs_indexing()` confronta l'hash e i file il cui contenuto non è
    cambiato vengono saltati (`unchanged_skipped`). Un file saltato non
    riceve alcuna scrittura, quindi il suo `last_indexed_at` resta
    fermo alla scansione precedente.
2.  A fine scansione lo stato viene certificato `full` e la copertura
    rivalidata. `touched_records()` considera "toccato" — quindi non
    provabilmente fresco — ogni record vivo il cui mtime su disco è
    **maggiore** di `last_indexed_at`, senza controllare se l'hash è
    cambiato. Per i file saltati al punto 1 la condizione è vera per
    costruzione: un file scritto prima della scansione ha mtime
    successivo alla sua ultima indicizzazione.
3.  Anche un solo record "toccato" porta la copertura a `full_stale`.
    Il gate ritorna il risultato vuoto con `index_status=index_stale`,
    e lo stesso ciclo si ripresenta a ogni query successiva.

La freshness richiesta dal gate di rivalidazione non è una proprietà
che la scansione incrementale può stabilire: il gate pretende che ogni
record sia indicato a o dopo l'ultima scrittura del file, ma la scanning
skip deliberately quei record senza aggiornarne il timestamp. La
condizione non si auto-risolve mai, e i tool di ricerca repository-level
restano disabilitati finché un file touched esiste nell'indice.

### Conseguenze

-   I tool che dovrebbero essere i più preziosi per un agente — ricerca
    simboli, ricerca file, grafo delle dipendenze, call graph —
    restituiscono risultati vuoti su repository con indice pieno e
    coerente, senza segnalare alcun errore.
-   L'agente non può distinguere "nessuna corrispondenza" da "l'indice
    è completo ma il gate lo ritiene non attendibile": l'envelope vuoto
    ha la stessa forma di una ricerca senza hit.
-   Ogni query sprecata esegue una scansione completa del repository
    (walk più reconciliation) con costo ripetuto e mai conclusivo.
-   La disponibilità dei tool per-file maschera il malfunzionamento:
    l'indice sembra sano dall'esterno.
-   Il problema è silenzioso anche nei log: la scansione riporta
    esito ok, lo stato nel DB dichiara copertura `full`, eppure ogni
    query a livello repository fallisce il gate.

### Intervento suggerito (storico; sostituito dall'intervento sopra)

Rendere coerenti le due nozioni di freshness usate dal gate, scegliendo
una sola strada (non servono entrambe):

1.  **Freshness per contenuto, non per tempo.** In
    `touched_records()`, quando l'hash corrente su disco coincide con
    il `content_hash` memorizzato nel record, il record è fresco per
    contenuto e non va contato come "toccato": il confronto mtime ha
    senso solo come scorciatoia quando l'hash diverge. Il gate di
    copertura deve rispondere alla domanda "la conoscenza derivata
    corrisponde ai byte attuali?", non "il file è stato scritto dopo
    l'ultima scansione?".
2.  **Timestamp di verifica del contenuto.** In alternativa, la
    scansione completa — che già legge e confronta l'hash di ogni file
    visitato — deve aggiornare un marcatore di verifica (`last_indexed_at`
    o un nuovo `content_verified_at`) anche per i record saltati come
    invariati, così che la rivalidazione immediatamente successiva
    possa certificare `full_fresh` senza riparsare nulla.

In entrambi i casi aggiungere una guardia sul gate: quando una scansione
appena completata non produce copertura `full_fresh`, il tool deve
restituire un errore esplicito o un hint operativo (ad esempio
`index_status=index_stale_after_scan` con la lista dei record touched),
non un risultato vuoto indistinguibile da una ricerca senza hit. Un
fallimento del gate che si ripete a ogni chiamata è una condizione da
segnalare, non da reiterare.

### Test di regressione

-   Indicizzare un repository completo, toccare (mtime) alcuni file senza
    modificarne il contenuto — o equivalentemente simulare la situazione
    osservata: file con hash invariato e `last_indexed_at` anteriore al
    mtime — e verificare che una query repository-level ritorni i
    risultati attesi dopo una singola scansione, non un envelope vuoto.
-   Verificare che dopo `index_repository()` lo stato di copertura sia
    `full_fresh` senza alcuna ulteriore scrittura sui file: la scansione
    appena completata deve essere auto-evidente.
-   Verificare il caso opposto (regressione negativa): un file davvero
    modificato nel contenuto tra due scansioni non deve mai essere
    considerato fresco dal gate.
-   Assert sulla forma dell'envelope: quando il gate blocca la query, la
    risposta deve contenere un segnale distinguibile da "nessun
    risultato" (stato, hint, o lista dei record touched).

------------------------------------------------------------------------

# 3. Dependency graph e invalidazione

## DEP-01 --- Ordine errato nell'invalidazione degli header DONE

**Priorità originale: Critica — Risolto**

`CacheManager::invalidate()` ora raccoglie i dipendenti prima di toccare
la cache o il grafo. `handle_edit()` usa questa operazione prima di
reindicizzare l'header. Gli archi necessari alla propagazione non vengono
cancellati in anticipo; le relazioni del file modificato vengono
ricostruite durante l'indicizzazione dello snapshot aggiornato.

Il tool `edit` restituisce anche il numero e i percorsi dei file
invalidati. La verifica end-to-end in `tests/test_tools.cpp` modifica un
header incluso da più sorgenti e controlla che i dipendenti vengano
segnalati e poi reindicizzati dalla scansione.

------------------------------------------------------------------------

## DEP-02 --- Invalidazione transitive non ricorsiva DONE

**Priorità originale: Alta — Risolto**

La raccolta dei dipendenti ora attraversa il grafo inverso con una BFS e
un insieme `visited`, così le catene di include/import vengono percorse
senza ripetere nodi o bloccarsi in presenza di cicli. La ricerca copre
gli archi `include` e `import`, ma esclude le chiamate tra funzioni,
che non sono dipendenze di file da invalidare. Un limite di sicurezza
impedisce di accumulare un numero illimitato di dipendenti; la risposta
indica quando la raccolta è stata interrotta.

La regressione in `tests/test_store.cpp` verifica una catena a tre
livelli, import Python, esclusione delle chiamate e rispetto del limite.
La stessa suite verifica che una scansione successiva reindicizzi i file
marcati come obsoleti.

------------------------------------------------------------------------

## DEP-03 --- Modifiche esterne senza invalidazione dei dipendenti

**Priorità: Alta**

### Comportamento osservato

Alcuni percorsi possono invocare direttamente `index_file()` quando
rilevano o ricevono contenuto aggiornato, senza passare dalla stessa
logica di invalidazione usata dal tool `edit`.

### Rischio

La correttezza della cache dipende dal percorso con cui la modifica
viene rilevata. Una modifica effettuata da un editor esterno, da Git o
da un altro processo può aggiornare il file ma lasciare obsoleti i dati
derivati dei dipendenti.

### Intervento suggerito

Centralizzare il flusso di aggiornamento. Ogni modifica rilevata deve
passare attraverso una singola operazione di alto livello che
gestisca: - acquisizione dello snapshot; - confronto hash; - raccolta
dei dipendenti; - invalidazione; - reindicizzazione; - aggiornamento del
grafo; - commit dello stato.

Non duplicare la logica di invalidazione nei singoli handler MCP.

------------------------------------------------------------------------

# 4. Database e transazioni

## DB-01 --- Errori SQLite non gestiti in modo robusto durante l'indicizzazione

**Priorità: Alta**

### Comportamento osservato

`KnowledgeStore::execute()` registra gli errori e restituisce un
risultato, ma nel percorso di indicizzazione alcune operazioni vengono
eseguite senza controllare sistematicamente il valore restituito.

La transazione può comprendere: - `BEGIN`; - aggiornamento del record
file; - eliminazione dei vecchi simboli; - inserimento dei nuovi
simboli; - inserimento delle dipendenze; - `COMMIT`.

### Rischio

Se una query fallisce e l'errore non viene propagato: - il chiamante può
non sapere che l'indicizzazione è fallita; - può essere lasciato uno
stato incompleto; - il codice può proseguire come se l'operazione fosse
riuscita.

### Intervento suggerito

Introdurre una transazione RAII: - inizio automatico; - commit
esplicito; - rollback automatico in caso di errore o eccezione; -
propagazione dell'errore al livello superiore.

Ogni operazione necessaria alla consistenza deve essere verificata.

### Test di regressione

Forzare un errore SQLite durante l'inserimento dei simboli e verificare
che: - la transazione venga annullata; - il vecchio indice resti
coerente; - l'errore arrivi al chiamante; - non venga registrato uno
stato di successo.

------------------------------------------------------------------------

## DB-02 --- Coerenza tra schema, vincoli e dati derivati

**Priorità: Da verificare**

Il modello dati contiene entità collegate: repository, file, simboli,
dipendenze e summary. La correttezza dipende dal fatto che aggiornamenti
e cancellazioni mantengano coerenti tutte queste relazioni.

### Verifiche da aggiungere

-   Vincoli `FOREIGN KEY` effettivamente attivi su ogni connessione.
-   Indici sulle colonne usate frequentemente nei join e nelle ricerche.
-   Comportamento delle cancellazioni: cascade, invalidazione o
    tombstone.
-   Unicità delle identità di file e simboli nel rispettivo repository.
-   Migrazioni dello schema e compatibilità con database già esistenti.

Questo punto richiede un audit specifico dello schema e delle migrazioni
prima di classificarlo come bug.

------------------------------------------------------------------------

# 5. Summary e memoria semantica

## SUM-01 --- Summary soltanto a livello di file

**Priorità: Alta rispetto agli obiettivi del progetto**

### Comportamento osservato

La tabella `summaries` associa la summary a `file_id` e `content_hash`.
Il flusso di scrittura invalida la summary attiva del file e ne
inserisce una nuova.

Non risulta implementata una persistenza equivalente a livello di
singolo simbolo/funzione.

### Limite rispetto alla visione di AI-Tools

L'obiettivo è permettere all'agente di chiedere informazioni su una
funzione senza dover leggere ogni volta il suo intero corpo.

Una summary per file non è abbastanza granulare: - un file può contenere
molte funzioni; - una modifica a una funzione può rendere obsoleta la
summary dell'intero file; - l'agente può dover leggere comunque molto
codice per ottenere un'informazione circoscritta.

### Intervento suggerito

Introdurre summary per simbolo, con: - identità del simbolo; - hash del
corpo o della regione rilevante; - testo della summary; - versione del
modello; - versione del prompt; - timestamp; - stato di validità; -
provenienza e stato di verifica.

La summary del file può restare come livello aggregato.

------------------------------------------------------------------------

## SUM-02 --- Repository summary non legata alla versione dei contenuti

**Priorità: Media**

### Comportamento osservato

La repository summary esaminata usa `repo_id` come valore di
`content_hash` e il recupero verifica lo stesso identificatore.

### Rischio

La summary generale può rimanere valida anche quando cambiano file,
simboli o dipendenze che ne modificano il contenuto semantico.

### Intervento suggerito

Associare la repository summary a una versione della conoscenza del
repository, per esempio: - generazione dell'indice; - revision ID; -
hash aggregato dei contenuti rilevanti; - versione del grafo.

Invalidare o rigenerare la summary quando cambiano i dati da cui
dipende.

------------------------------------------------------------------------

## SUM-03 --- La generazione della summary è responsabilità dell'agente esterno

**Priorità: Informativa / scelta architetturale**

### Comportamento osservato

Il server espone operazioni per recuperare e scrivere summary. Il testo
della summary viene fornito dal client MCP: è quindi l'agente esterno a
leggere il codice e a produrre il contenuto semantico.

### Valutazione

Non è necessariamente un difetto. Mantiene AI-Tools indipendente dal
provider LLM e consente di usare modelli locali o remoti.

È però importante che il protocollo renda chiaro: - quando una summary
manca; - quando è obsoleta; - quale sorgente deve essere letta; - come
salvare la nuova summary; - quale versione del modello/prompt l'ha
generata.

Non introdurre una dipendenza obbligatoria da un provider specifico solo
per automatizzare la generazione.

------------------------------------------------------------------------

## SUM-04 --- Mancanza di provenienza e livello di affidabilità delle summary

**Priorità: Media, miglioramento progettuale**

Una summary generata da un LLM è una descrizione derivata, non una fonte
di verità equivalente al codice.

### Intervento suggerito

Memorizzare almeno: - modello e versione; - prompt/versione del
formato; - hash esatto del contenuto analizzato; - timestamp; -
eventuale validazione; - stato `valid`, `stale`, `unverified` o
equivalente.

L'agente deve poter distinguere una summary aggiornata da una summary
non verificata o obsoleta. In caso di dubbio deve poter recuperare il
sorgente.

------------------------------------------------------------------------

# 6. Tool MCP e consumo del context window

## MCP-01 --- Parametri dichiarati ma non applicati

**Priorità: Media**

### Comportamento osservato

Alcuni JSON Schema dei tool dichiarano parametri come `symbol_type`, ma
gli handler esaminati non li utilizzano effettivamente nelle query.

### Conseguenza

L'agente può inviare un filtro credendo che venga applicato, mentre il
server restituisce risultati non filtrati.

### Intervento suggerito

Per ogni tool: 1. confrontare JSON Schema e parametri letti
dall'handler; 2. implementare ogni parametro dichiarato oppure
rimuoverlo dallo schema; 3. aggiungere test per ogni filtro; 4.
verificare che i valori non supportati producano un errore esplicito.

------------------------------------------------------------------------

## MCP-02 --- Descrizione dei tool non sempre coerente con il comportamento

**Priorità: Media**

### Comportamento osservato

Alcuni tool sono descritti come capaci di accettare pattern o ricerche
flessibili, mentre il percorso di recupero dei dettagli utilizza un
confronto esatto sul nome del simbolo.

### Conseguenza

Gli agenti scelgono i tool sulla base delle descrizioni. Una discrepanza
può produrre chiamate inappropriate e risultati vuoti o inattesi.

### Intervento suggerito

Allineare: - nome del tool; - descrizione in linguaggio naturale; - JSON
Schema; - comportamento dell'handler; - test.

Distinguere chiaramente ricerca per pattern, ricerca esatta e recupero
tramite ID stabile.

------------------------------------------------------------------------

## MCP-03 --- `file_content` restituisce il file intero per default

**Priorità: Media rispetto all'obiettivo di ridurre i token**

### Comportamento osservato

Se non vengono forniti `start_line` ed `end_line`, il tool può
restituire l'intero file. La presenza di una summary non impedisce
necessariamente la restituzione del sorgente completo.

### Conseguenza

L'agente può consumare molte più informazioni del necessario, anche
quando gli basterebbero firma o summary.

### Intervento suggerito

Offrire modalità esplicite e distinte: - `metadata`; - `signature`; -
`summary`; - `source` di un simbolo o intervallo; - `full` per il file
completo.

La modalità predefinita dovrebbe essere quella meno costosa e più
circoscritta, purché non nasconda informazioni necessarie.

------------------------------------------------------------------------

## MCP-04 --- Identità e riferimenti ai simboli da rendere stabili

**Priorità: Media, miglioramento progettuale**

Per sostenere summary per funzione e recupero preciso, è opportuno che i
tool possano riferirsi a un simbolo in modo non ambiguo.

Un semplice nome non basta in presenza di: - overload; - metodi omonimi
in classi diverse; - namespace; - template; - funzioni anonime o locali.

Introdurre un ID del simbolo o una chiave composta stabile, accompagnata
da qualified name e firma.

------------------------------------------------------------------------

# 7. Parser e call graph

## PAR-01 --- Fallback regex non raggiungibile per estensioni non riconosciute

**Priorità: Media**

### Comportamento osservato

Il README descrive un fallback regex per altri linguaggi. Tuttavia, il
riconoscimento dell'estensione restituisce `unknown` per i linguaggi non
supportati e il flusso ordinario di `index_file()` può interrompersi
prima di raggiungere il fallback.

### Conseguenza

La copertura effettiva dei linguaggi può essere inferiore a quella
suggerita dalla documentazione.

### Intervento suggerito

-   Definire esplicitamente i linguaggi supportati.
-   Separare parser Tree-sitter, fallback regex e linguaggi sconosciuti.
-   Aggiungere test per ogni estensione dichiarata.
-   Aggiornare il README in base al comportamento effettivo.

------------------------------------------------------------------------

## PAR-02 --- Call graph non semanticamente risolto

**Priorità: Media**

### Comportamento osservato

L'estrazione delle chiamate tramite Tree-sitter individua le call
expression, ma non dispone necessariamente delle informazioni di tipo
necessarie per risolvere il destinatario reale.

### Esempio

``` cpp
objectA.execute();
objectB.execute();
```

Le due chiamate possono essere registrate con lo stesso nome `execute`,
anche se i metodi appartengono a tipi differenti.

### Limiti da considerare

-   overload;
-   metodi omonimi;
-   namespace;
-   dispatch virtuale;
-   function pointer;
-   function object/lambda;
-   template;
-   chiamate indirette.

### Intervento suggerito

Mantenere Tree-sitter per l'estrazione sintattica e introdurre
gradualmente: - qualified names; - scope e class scope; - signature
hash; - informazioni sul receiver; - risoluzione opzionale basata su
`compile_commands.json` e Clang.

Non presentare il grafo corrente come call graph semanticamente
completo.

------------------------------------------------------------------------

## PAR-03 --- Query `LIKE` potenzialmente ambigue

**Priorità: Media**

### Comportamento osservato

Alcune query del grafo usano confronti `LIKE` con wildcard per
individuare i simboli chiamati.

### Rischio

Il matching può includere simboli omonimi o corrispondenze parziali non
desiderate.

### Intervento suggerito

Usare, quando possibile: - ID del simbolo; - qualified name; - firma
normalizzata; - tipo di relazione esplicito.

Se il matching approssimativo resta necessario, il risultato dovrebbe
indicare il livello di confidenza e non essere confuso con una
risoluzione certa.

------------------------------------------------------------------------

# 8. Filesystem e indicizzazione incrementale

## FS-01 --- Scansione del filesystem ancora necessaria a ogni controllo

**Priorità: Media / dipendente dalla dimensione dei repository**

### Comportamento osservato

L'indicizzazione evita di riparsare i file invariati, ma
`index_repository()` deve comunque attraversare i percorsi e verificare
i file.

### Distinzione importante

Il sistema è incrementale nel parsing e nell'aggiornamento dei record,
ma non necessariamente nella scoperta delle modifiche.

### Intervento suggerito

Per repository molto grandi valutare: - filesystem watcher; - registro
delle modifiche; - controllo incrementale per directory; - scansione
completa periodica come riconciliazione; - fallback alla scansione
quando il watcher perde eventi.

Non introdurre watcher prima di avere misurato il costo reale della
scansione.

------------------------------------------------------------------------

## FS-02 --- Esclusioni directory e `.gitignore`

**Priorità: Media**

### Comportamento osservato

Il codice esclude alcune directory note, ma non risulta equivalente a
una gestione completa delle regole `.gitignore`. Inoltre, escludere file
dopo averli raggiunti non evita necessariamente di attraversare
directory generate o molto grandi.

### Intervento suggerito

-   Integrare una libreria o un parser affidabile per le regole ignore.
-   Applicare le esclusioni durante la traversata delle directory.
-   Consentire configurazioni per repository.
-   Documentare le differenze tra esclusioni predefinite e `.gitignore`.

------------------------------------------------------------------------

# 9. Sicurezza

## SEC-01 --- `execute` non è un sandbox

**Priorità: Alta se il server è usato con agenti non fidati o autonomi**

### Comportamento osservato

Il tool esegue comandi tramite `std::system()` dopo aver cambiato
directory nel repository.

### Problema

Il cambio di directory non limita i permessi del processo. Il comando
può potenzialmente: - leggere file esterni al repository; - modificare
file esterni; - accedere alla rete; - terminare o interferire con altri
processi; - consumare risorse senza limiti.

### Intervento suggerito

In base al modello di minaccia: - non eseguire comandi non fidati; -
richiedere conferma esplicita per operazioni sensibili; - impostare
timeout e limiti di output; - applicare limiti di risorse; - isolare il
processo tramite strumenti OS appropriati; - limitare filesystem e rete
quando necessario.

La documentazione deve dichiarare chiaramente che `execute` esegue
comandi con i permessi del processo che ospita AI-Tools, salvo
isolamento aggiuntivo.

------------------------------------------------------------------------

## SEC-02 --- Interpretazione dell'exit status

**Priorità: Bassa/Media**

### Comportamento osservato

Il codice esaminato usa `WEXITSTATUS(rc)` dopo aver controllato soltanto
il caso `rc == -1`.

### Problema

`WEXITSTATUS` deve essere utilizzato dopo aver verificato
`WIFEXITED(rc)`. Un processo può terminare anche a causa di un segnale.

### Intervento suggerito

Distinguere: - errore di avvio; - terminazione normale e relativo exit
code; - terminazione tramite segnale; - eventuale timeout.

Restituire queste condizioni in modo esplicito al client MCP.

------------------------------------------------------------------------

# 10. Testing e qualità

## TEST-01 --- Test mancanti sui percorsi di regressione critici

**Priorità: Alta**

La suite esaminata copre diverse funzionalità, ma non intercetta tutti i
problemi individuati.

### Test da aggiungere

| ID   | Test | Priorità |
|------|------|----------|
| T-01 | Indicizzazione di un singolo file seguita da ricerca globale | Critica |
| T-02 | Modifica di header tramite tool `edit` e invalidazione dipendenti | Critica |
| T-03 | Modifica esterna di header e invalidazione dipendenti | Critica |
| T-04 | Eliminazione di un file e rimozione dei suoi simboli | Critica |
| T-05 | Catena di dipendenze a tre o più livelli | Alta |
| T-06 | Due summary distinte per due funzioni dello stesso file | Alta |
| T-07 | Repository summary dopo modifica dei sorgenti | Alta |
| T-08 | Errore SQLite durante una transazione | Alta |
| T-09 | Overload e metodi omonimi nel call graph | Media |
| T-10 | Directory generate e regole `.gitignore` | Media |
| T-11 | Processo `execute` terminato da un segnale | Media |
| T-12 | Modifica concorrente durante parsing/hash (DONE: run_snapshot_consistency_tests) | Media |
| T-13 | Mtime piu' recente ma hash invariato; la copertura resta `full_fresh` e una scansione marca il record verificato (IDX-04) | Alta |
| T-14 | Timestamp di verifica arretrato con hash invariato; una scansione raggiunge `full_fresh` senza riscrivere il file (IDX-04) | Alta |
| T-15 | Modifica reale del contenuto rilevata dal gate e corretta dalla scansione (IDX-04) | Alta |
| T-16 | Timestamp di verifica mancante: freshness decisa dall'hash, mai presunta dal solo timestamp (IDX-04) | Media |

### Nota sui test esistenti

È presente un test che documenta l'ordine corretto tra invalidazione
transitive e cancellazione degli archi. Il percorso `handle_edit()`
esaminato sembra però eseguire le operazioni in ordine opposto. È un
esempio di test unitario corretto che non viene ancora tradotto in una
garanzia sul flusso integrato.

------------------------------------------------------------------------

# 11. Piano di intervento consigliato

L'ordine seguente evita di costruire nuove funzionalità sopra uno stato
di indicizzazione potenzialmente incoerente.

## Fase 1 --- Correttezza e consistenza

1.  Correggere la distinzione tra indicizzazione parziale e completa.
2.  Correggere l'ordine di invalidazione degli header.
3.  Implementare la propagazione ricorsiva delle invalidazioni.
4.  Applicare la stessa logica alle modifiche esterne.
5.  Riconciliare i file eliminati.
6.  Rendere atomico e verificabile il flusso SQLite.
7.  Rendere coerente la rivalidazione della copertura dopo una scansione
    completa (IDX-04), così che i tool di ricerca repository-level non
    restino silenziosamente disabilitati.
8.  Aggiungere i test T-01--T-08.

**Criterio di completamento:** il database non deve presentare dati
apparentemente validi quando l'indice è parziale, obsoleto o incompleto.
Una query repository-level su un indice completo e coerente non deve
restituire un risultato vuoto senza spiegare il motivo.

## Fase 2 --- Knowledge per simbolo

1.  Introdurre identità persistenti dei simboli.
2.  Memorizzare summary per funzione/metodo.
3.  Associare ogni summary all'hash del contenuto analizzato.
4.  Memorizzare modello, prompt e provenienza.
5.  Invalidare selettivamente le summary quando cambia il simbolo.
6.  Mantenere summary di file e repository come livelli aggregati.

**Criterio di completamento:** una modifica a una funzione non deve
obbligare a rigenerare indiscriminatamente la conoscenza di tutto il
file o repository.

## Fase 3 --- Ottimizzazione del context window

1.  Separare metadata, signature, summary e source.
2.  Rendere possibile il recupero del corpo di un singolo simbolo.
3.  Rendere esplicito il livello di dettaglio richiesto.
4.  Allineare descrizioni, schema e comportamento dei tool MCP.
5.  Misurare token restituiti e numero di letture evitate.

**Criterio di completamento:** l'agente deve poter rispondere a
richieste comuni utilizzando firma e summary, recuperando il sorgente
soltanto quando serve.

## Fase 4 --- Precisione del grafo

1.  Migliorare la qualificazione dei simboli.
2.  Ridurre le ambiguità del matching `LIKE`.
3.  Gestire overload e scope.
4.  Valutare un backend Clang opzionale.
5.  Misurare precisione e recall del call graph.

**Criterio di completamento:** il sistema deve distinguere chiaramente
relazioni certe, probabili e non risolte.

## Fase 5 --- Performance misurabile

1.  Misurare il costo della scansione filesystem.
2.  Misurare parsing e hashing separatamente.
3.  Misurare query SQLite e tempi MCP.
4.  Valutare watcher soltanto se la scansione è un collo di bottiglia.
5.  Misurare il risparmio di token con scenari riproducibili.

------------------------------------------------------------------------

# 12. Benchmark da realizzare

Per dimostrare il valore di AI-Tools non basta misurare quanto
velocemente indicizza. Il suo obiettivo è ridurre il lavoro ripetuto
dell'agente mantenendo l'accuratezza.

Registrare almeno:

| Metrica | Cosa misura |
|---------|-------------|
| Full indexing time | Tempo per costruire l'indice iniziale |
| Incremental indexing time | Costo di una modifica |
| Filesystem scan time | Costo della scoperta delle modifiche |
| Parse time | Tempo Tree-sitter |
| Hash time | Tempo di hashing |
| DB query latency | Latenza delle query |
| MCP response bytes/tokens | Dimensione delle risposte |
| Source reads avoided | Letture del sorgente evitate |
| Summary hit rate | Frequenza di recupero summary valida |
| Summary staleness rate | Frequenza di summary obsolete |
| Answer accuracy | Correttezza delle risposte dell'agente |
| Graph precision/recall | Qualità delle relazioni estratte |

Confrontare almeno tre modalità: 1. Agente senza AI-Tools. 2. Agente con
indicizzazione e ricerca strutturale. 3. Agente con indicizzazione,
summary persistenti e recupero progressivo.

Usare gli stessi task, lo stesso modello e lo stesso repository.
Registrare token in input/output, numero di chiamate ai tool e
correttezza del risultato.

------------------------------------------------------------------------

# 13. Conclusione

La struttura generale di AI-Tools è una base ragionevole: parser
strutturali, database persistente, dependency graph, cache e interfaccia
MCP sono componenti coerenti con l'obiettivo.

Le criticità principali non richiedono una riscrittura completa.
Richiedono soprattutto di rendere affidabile il ciclo:

``` text
filesystem change
       |
       v
snapshot del contenuto
       |
       v
hash + parsing
       |
       v
dependency invalidation
       |
       v
database update
       |
       v
knowledge refresh
       |
       v
MCP retrieval
```

La direzione di prodotto più promettente è una **cache semantica
gerarchica del repository**: conoscenza a livello repository, modulo,
file e simbolo, con validità verificabile e recupero progressivo.

Il principio fondamentale deve restare questo:

> La summary è una rappresentazione compatta e derivata del codice, non
> una fonte di verità superiore al codice stesso.

Prima di ampliare il numero di tool o aggiungere altre funzionalità,
conviene rendere affidabili indicizzazione, invalidazione e consistenza
dei dati. Su queste fondamenta la memoria semantica per funzione potrà
diventare una caratteristica realmente utile e misurabile.
