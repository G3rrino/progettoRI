#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h> // Necessario per il tipo bool e i valori true/false
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "protocol.h"
#include "net_utils.h"

// MAX_CONNS deve contenere sia gli Storage persistenti sia i Client simultanei (almeno 16 + 5)
#define MAX_CONNS 128
#define NUMERO_BASE_STORAGE 16
#define NUMERO_BASE_FILE 1024
// Dimensione della coda di connessioni pendenti passata a listen()
#define LISTEN_BACKLOG 64

// Flag atomico impostato dall'handler di SIGINT e SIGTERM per interrompere il loop principale
static volatile sig_atomic_t g_stop = 0;

typedef enum {
    ROLE_UNKNOWN = 0, //appena registrato senza un ruolo (negli enum serve la virgola, non il punto e virgola)
    ROLE_STORAGE = 1  //ruolo di uno storage (connessione persistente)
} conn_role_t;

typedef struct {
    int fd; //descrittore del socket(-1 se non usato)
    conn_t conn; //struttura dati della connessione
    conn_role_t role; //ruolo
    uint64_t storage_id; //numero dello storage_id, 0 se conn_role_t!=ROLE_STORAGE (uint64_t non ha segno)
} conn_slot_t;

//rappresentiamo le informazioni di uno storage che ha completato bene l'handshake
typedef struct {
    uint64_t id; //id dello storage (parte da 1 come da specifica)
    uint16_t port; //porta di ascolto
    int sock_fd; //descrittore del socket associato
    bool active; //se attivo o meno
} storage_node_t;

//informazioni relative ai file che storiamo dentro il coordinator
typedef struct {
    char name[MAX_NAME_LEN]; //nome con dim massima
    uint64_t size;
    uint64_t storage_id; //id dello storage all'interno del quale è contenuto
} file_entry_t;

//per tenere traccia di tutti i dati insieme senza dovere ricorrere a più strutture dati sconnesse 
//se ne può mettere una sola per domarli tutti, ci mettiamo tutto
typedef struct {
    conn_slot_t slots[MAX_CONNS]; // Array fisso per tutte le connessioni TCP monitorate da select()

    //dati sugli storage
    storage_node_t *storages; //array dinamico degli storage registrati
    size_t storage_count;
    size_t storage_cap;

    file_entry_t *files; //lista dinamica dei file presenti
    size_t file_count;
    size_t file_cap;

    uint64_t next_storage_id; //counter per assegnare il prossimo id allo storage 
} coord_ctx_t; // Mancava il nome del typedef alla chiusura della struct

// Dichiarazioni anticipate per evitare errori di compilazione sulle chiamate incrociate
static void remove_storage(coord_ctx_t *ctx, uint64_t storage_id);
static void remove_files_by_storage(coord_ctx_t *ctx, uint64_t storage_id);

//================================================
//GESTIONE DEI SEGNALI (SIGINT, SIGTERM, SIGPIPE)
//================================================

// Handler async-signal-safe: si limita ad alzare il flag globale g_stop
static void handle_term_signal(int sig) {
    (void)sig; // Evita warning del compilatore per parametro non utilizzato
    g_stop = 1;
}

// Configura SIGPIPE su SIG_IGN e installa sigaction su SIGINT e SIGTERM senza SA_RESTART
static int setup_signals(void) {
    // Ignora SIGPIPE per non terminare il processo quando si scrive su un socket già chiuso
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("Errore configurazione SIGPIPE");
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_term_signal;
    sigemptyset(&sa.sa_mask);
    // sa_flags = 0 fa sì che select() venga interrotta immediatamente con EINTR all'arrivo del segnale
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("Errore configurazione SIGINT/SIGTERM");
        return -1;
    }

    return 0;
}

//funzioni di gestione delle strutture dati

//================================================
//GESTIONE DEGLI SLOT DI CONNESSIONE (CONN_SLOT_T)
//================================================

static void reset_conn_slot(conn_slot_t *slot) {
    //devo reimpostare tutti i suoi campi
    if (slot == NULL) return;
    slot->fd = -1;
    slot->role = ROLE_UNKNOWN;
    slot->storage_id = 0;
    //reset conn_t: non ho ben inteso cosa fare
    // conn_init reimposta conn.fd = -1 e azzera gli indici del buffer di accumulo byte
    conn_init(&slot->conn, -1);
}

//trova il primo conn_slot_t libero per ospitare una nuova connessione TCP (Client o Storage)
static conn_slot_t *find_free_conn_slot(coord_ctx_t *ctx) {
    if (ctx == NULL) return NULL;
    // Scorriamo ctx->slots (le connessioni TCP), non ctx->storages (i nodi già registrati)
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (ctx->slots[i].fd < 0) {
            return &ctx->slots[i];
        }
    }
    return NULL; //non ne ho trovato nessuno libero in questo caso
    //il main si occuperà di chiudere il socket
}

//chiudiamo in maniera sicura una connessione
static void close_conn_slot(coord_ctx_t *ctx, conn_slot_t *slot) {
    if (ctx == NULL || slot == NULL) {
        return;
    }
    //chiudiamo prima il socket di connessione se è valido
    if (slot->fd >= 0) {
        close(slot->fd);
    }
    //poi verifichiamo che sia uno storage
    //se lo è bisogna togliergli lo slot e i file che sono attivi a suo conto nella LIST
    if (slot->role == ROLE_STORAGE) {
        remove_storage(ctx, slot->storage_id);
    }
    reset_conn_slot(slot);
}

//================================================
//GESTIONE DEGLI STORAGE (STORAGE_NODE_T)
//================================================

//funzione di espansione dinamica degli slot relativi agli storage
static int ensure_storage_capacity(coord_ctx_t *ctx) {
    if (ctx == NULL) return -1;

    // Se c'è ancora capacità libera nell'array già allocato, non serve chiamare realloc
    if (ctx->storage_count < ctx->storage_cap) {
        return 0;
    }

    //raddoppiamo la capacità ad ogni esaurimento(nuovo cap max)
    size_t new_cap = (ctx->storage_cap == 0) ? NUMERO_BASE_STORAGE : ctx->storage_cap * 2;

    // Salviamo il puntatore originale: se realloc fallisce ritorna NULL senza liberare il vecchio blocco
    void *ptr = ctx->storages;
    ctx->storages = realloc(ctx->storages, sizeof(storage_node_t) * new_cap);
    if (ctx->storages == NULL) {
        ctx->storages = ptr;
        return -1;
    }

    //aggiorniamo il cap
    ctx->storage_cap = new_cap;
    return 0;
}

static storage_node_t *register_storage_node(coord_ctx_t *ctx, uint16_t port, int sockfd) {
    //controlli preliminari di quello che è passato in ingresso
    if (ctx == NULL || port == 0 || sockfd < 0) {
        return NULL;
    }

    // Gli ID degli Storage non vengono mai riutilizzati: accodiamo il nuovo nodo in ctx->storages
    // garantendo spazio con ensure_storage_capacity (mantiene l'array ordinato per ID crescente)
    if (ensure_storage_capacity(ctx) < 0) {
        return NULL;
    }

    // Prendiamo il puntatore al prossimo elemento libero in fondo all'array dinamico ctx->storages
    storage_node_t *node = &ctx->storages[ctx->storage_count++];

    //assegniamoli i dati corrispondenti
    node->port = port;
    node->active = true;
    node->sock_fd = sockfd;
    node->id = ctx->next_storage_id++;
    return node;
}

static storage_node_t *find_active_storage_id(coord_ctx_t *ctx, uint64_t storage_id) {
    //funzione di handling ricerca storage tramite id (gli ID partono da 1, quindi 0 non è valido)
    if (ctx == NULL || storage_id == 0 || storage_id >= ctx->next_storage_id) {
        return NULL;
    }

    for (size_t i = 0; i < ctx->storage_count; ++i) {
        if (ctx->storages[i].id == storage_id && ctx->storages[i].active == true) {
            return &ctx->storages[i];
        }
    }
    //nodo storage non trovato
    return NULL;
}

static storage_node_t *find_min_id_active_storage(coord_ctx_t *ctx) {
    if (ctx == NULL) return NULL;

    // Cerchiamo tra tutti gli Storage attivi quello con il campo id più piccolo
    storage_node_t *best = NULL;
    for (size_t i = 0; i < ctx->storage_count; ++i) {
        if (ctx->storages[i].active == true) {
            if (best == NULL || ctx->storages[i].id < best->id) {
                best = &ctx->storages[i];
            }
        }
    }
    return best; // Se NULL non c'è nessun nodo attivo: l'handler della PUT invierà ERR_NO_STORAGE
}

static void remove_storage(coord_ctx_t *ctx, uint64_t storage_id) {
    if (ctx == NULL || storage_id == 0 || storage_id >= ctx->next_storage_id) {
        return;
    }

    storage_node_t *snt = find_active_storage_id(ctx, storage_id);
    if (snt == NULL) {
        return;
    }
    snt->active = false;
    snt->sock_fd = -1;
    remove_files_by_storage(ctx, storage_id);
}

//=====================================================================
//GESTIONE DELL'ENTRY FILES (LISTA DEI FILES quindi file_entry_t *files)
//=====================================================================

//aumento l'array dinamico di lista dei file
static int ensure_file_capacity(coord_ctx_t *ctx) {
    if (ctx == NULL) {
        return -1;
    }

    // Se abbiamo ancora posti preallocati, non serve riallocare
    if (ctx->file_count < ctx->file_cap) {
        return 0;
    }

    //stessa logica rispetto agli storage; ogni volta raddoppiamo
    size_t new_cap = (ctx->file_cap == 0) ? NUMERO_BASE_FILE : ctx->file_cap * 2;
    void *ptr = ctx->files;
    ctx->files = realloc(ctx->files, sizeof(file_entry_t) * new_cap);
    if (ctx->files == NULL) {
        ctx->files = ptr;
        return -1;
    }

    //aggiorniamo di nuovo il cap
    ctx->file_cap = new_cap;
    return 0;
}

//funzione di handler per trovare l'indice i del file richiesto
static int find_file_index(coord_ctx_t *ctx, const char *remote_name) {
    if (ctx == NULL || remote_name == NULL) {
        return -1;
    }

    // Confrontiamo il campo .name della struct file_entry_t con remote_name
    for (size_t i = 0; i < ctx->file_count; ++i) {
        if (strcmp(ctx->files[i].name, remote_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

//funzione per inserire un file dentro la lista, viene chiamata subito dopo la ricezione di stored
static int upsert_file_entry(coord_ctx_t *ctx, const char *name, uint64_t size, uint64_t storage_id) {
    // Attenzione: size == 0 è ammesso dalla specifica (file vuoto), rifiutiamo solo > MAX_FILE_SIZE
    if (ctx == NULL || name == NULL || size > MAX_FILE_SIZE || storage_id == 0 || storage_id >= ctx->next_storage_id) {
        return -1;
    }

    //caso LIMITE: due PUT dello stesso file, se il file già esiste, si prende
    //le ultime statistiche inviate
    //secondo me la gestione è sbagliata in questo caso, chi prima arriva meglio alloggia, se arrivo
    //e c'è già il file mando l'errore, un altro client non può sovrascrivere la statistica del file vecchio,
    //perderei la consistenza di avere una struttura dati di files introdotti e aggiornatiù
    //in maniera semplice: possono esserci file uguali? non lo so
    //possono esserci ma provenienti da due store diversi 
    //quindi la possiamo gestire così
    //a. permettiamo dei duplicati fra diversi storage, quindi file di nome uguale ma da diversi storage
    //(dovremmo gestire il fatto che quando un client chiede quel file lo storage risponde con la lista 
    //dei possibili, quindi file1 storage1, file1 storage2)
    //b. non permettiamo file di nome uguale (questa è molto più semplice da gestire)
    int existing_idx = find_file_index(ctx, name);
    if (existing_idx >= 0) {
        ctx->files[existing_idx].size = size;
        ctx->files[existing_idx].storage_id = storage_id;
        return 0;
    }

    // Se il file è nuovo, verifichiamo che ci sia spazio nell'array dinamico ctx->files
    if (ensure_file_capacity(ctx) < 0) {
        return -1;
    }

    // Inseriamo il nuovo record in coda e incrementiamo file_count
    file_entry_t *entry = &ctx->files[ctx->file_count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->size = size;
    entry->storage_id = storage_id;
    return 0;
}

//togliamo e compattiamo i file appartenenti ad un certo storage, aggiorniamo file_count
static void remove_files_by_storage(coord_ctx_t *ctx, uint64_t storage_id) {
    if (ctx == NULL) return;

    // Usiamo due indici: read_idx legge tutto l'array, write_idx riscrive solo i file da conservare
    // In questo modo non lasciamo buchi nell'array in un solo passaggio O(N) senza riallocare
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < ctx->file_count; ++read_idx) {
        if (ctx->files[read_idx].storage_id != storage_id) {
            if (write_idx != read_idx) {
                ctx->files[write_idx] = ctx->files[read_idx];
            }
            write_idx++;
        }
    }

    // Aggiorniamo il numero totale di file validi (file_cap resta invariato per riutilizzare la RAM)
    ctx->file_count = write_idx;
}

static int cmp_file_entries_by_name(const void *a, const void *b) {
    //funzione di comparazione per qsort() utilizzata dalla LIST
    //utilizziamo strcmp garantendo l'ordinamento crescente byte per byte
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    return strcmp(fa->name, fb->name);
}

//=================================
//HANDLER DEI MESSAGGI DI CONTROLLO
//=================================

//controlli sul camando REGISTER
//se tutto ok registro lo storage in un nuovo slot
static void handle_register(coord_ctx_t *ctx, conn_slot_t *slot, const parsed_line_t *parsed) {
    if (ctx == NULL || slot == NULL || parsed == NULL) {
        errno = EINVAL;
        return;
    }
    // Verifichiamo che il comando sia esattamente "REGISTER" e abbia 2 token totali
    if (parsed->token_count != 2 || strcmp(parsed->tokens[0], "REGISTER") != 0) {
        send_err_line(slot->fd, ERR_BAD_REQUEST);
        close_conn_slot(ctx, slot);
        return;
    }

    //bisognerà anche aggiungere, forse nell'if sopra che il comando deve essere preciso REGISTER, quindi 
    //confrontare il primo token passato con "REGISTER"
    // La variabile locale deve essere uint16_t (non puntatore) per passare il suo indirizzo &port
    uint16_t port = 0;
    if (parse_strict_port(parsed->tokens[1], &port) < 0) {
        send_err_line(slot->fd, ERR_INVALID_PORT);
        close_conn_slot(ctx, slot);
        return;
    }

    // Registriamo il nodo Storage nell'anagrafica assegnandogli un nuovo storage_id progressivo
    storage_node_t *snt = register_storage_node(ctx, port, slot->fd);
    if (snt == NULL) {
        send_err_line(slot->fd, ERR_IO);
        close_conn_slot(ctx, slot);
        return;
    }

    // Trasformiamo lo slot di connessione in connessione persistente Storage
    slot->role = ROLE_STORAGE;
    slot->storage_id = snt->id;

    char buffer_risposta_registered[MAX_LINE_LEN];
    // Passiamo sempre sizeof(buffer) a snprintf e controlliamo che non ci siano troncamenti
    int n = snprintf(buffer_risposta_registered, sizeof(buffer_risposta_registered),
                     "REGISTERED %llu\n", (unsigned long long)slot->storage_id);
    if (n < 0 || (size_t)n >= sizeof(buffer_risposta_registered)) {
        close_conn_slot(ctx, slot);
        return;
    }

    // Inviamo esattamente (size_t)n byte (non l'intero buffer da 512 byte)
    ssize_t inviati = send_all(slot->fd, buffer_risposta_registered, (size_t)n);
    //qui dovremmo controllare che è tutto ok nella send_all
    if (inviati < 0) {
        // Se l'invio fallisce chiudiamo lo slot (che rimuove automaticamente anche lo Storage registrato)
        close_conn_slot(ctx, slot);
    }
}

//funzione handler della riga di comando stored
static void handle_stored(coord_ctx_t *ctx, conn_slot_t *slot, const parsed_line_t *parsed) {
    if (ctx == NULL || slot == NULL || parsed == NULL) {
        return;
    }

    uint64_t size = 0;
    // Se STORED è malformato inviamo ERR_BAD_REQUEST e chiudiamo la connessione rimuovendo lo Storage
    if (parsed->token_count != 3 ||
        strcmp(parsed->tokens[0], "STORED") != 0 ||
        validate_remote_name(parsed->tokens[1]) < 0 ||
        parse_strict_uint64(parsed->tokens[2], &size) < 0 ||
        size > MAX_FILE_SIZE) {
        send_err_line(slot->fd, ERR_BAD_REQUEST);
        close_conn_slot(ctx, slot);
        return; // Fondamentale per non proseguire con l'inserimento e l'ACK dopo l'errore
    }

    //se invece i controlli sono finiti tutti a buon fine, la riga STORED è ben formata, allora inseriamo l'indice del file e 
    //la targhetta ACK
    if (upsert_file_entry(ctx, parsed->tokens[1], size, slot->storage_id) < 0) {
        send_err_line(slot->fd, ERR_IO);
        close_conn_slot(ctx, slot);
        return;
    }

    char buffer_risposta_ack[MAX_LINE_LEN];
    int n = snprintf(buffer_risposta_ack, sizeof(buffer_risposta_ack), "ACK\n");
    if (n < 0 || (size_t)n >= sizeof(buffer_risposta_ack)) {
        close_conn_slot(ctx, slot);
        return;
    }

    // Inviamo solo i 4 byte di "ACK\n" mantenendo aperta la connessione persistente dello Storage
    ssize_t inviati = send_all(slot->fd, buffer_risposta_ack, (size_t)n);
    //qui dovremmo controllare che è tutto ok nella send_all
    if (inviati < 0) {
        // Se l'invio dell'ACK fallisce chiudiamo lo slot e de-registriamo lo Storage
        close_conn_slot(ctx, slot);
    }
}

static void handle_put(coord_ctx_t *ctx, conn_slot_t *slot, const parsed_line_t *parsed) {
    if (ctx == NULL || slot == NULL || parsed == NULL) {
        return;
    }

    // 1. Controllo numero campi: su ogni errore chiudiamo subito la connessione one-shot del Client
    if (parsed->token_count != 3) {
        send_err_line(slot->fd, ERR_BAD_REQUEST);
        close_conn_slot(ctx, slot);
        return;
    }

    // 2. Controllo validità del remote_name
    if (validate_remote_name(parsed->tokens[1]) < 0) {
        send_err_line(slot->fd, ERR_INVALID_NAME);
        close_conn_slot(ctx, slot);
        return;
    }

    // 3. Controllo dimensione file (intero decimale puro <= 10 MB)
    uint64_t size = 0;
    if (parse_strict_uint64(parsed->tokens[2], &size) < 0 || size > MAX_FILE_SIZE) {
        send_err_line(slot->fd, ERR_INVALID_SIZE);
        close_conn_slot(ctx, slot);
        return;
    }

    // 4. Controllo che il file non sia già presente nell'indice globale
    if (find_file_index(ctx, parsed->tokens[1]) >= 0) {
        send_err_line(slot->fd, ERR_FILE_EXISTS);
        close_conn_slot(ctx, slot);
        return;
    }

    // 5. Selezione dello Storage attivo con storage_id più piccolo
    storage_node_t *snt = find_min_id_active_storage(ctx);
    if (snt == NULL) {
        send_err_line(slot->fd, ERR_NO_STORAGE);
        close_conn_slot(ctx, slot);
        return;
    }

    //se viene validato tutto, invio la stringa STORE <storage_id> 127.0.0.1 <storage_port> \n e chiudo lo slot
    char buffer_dove_si_trova[MAX_LINE_LEN];
    // Usiamo snt->id e snt->port dello Storage selezionato (slot è la connessione del Client)
    int n = snprintf(buffer_dove_si_trova, sizeof(buffer_dove_si_trova),
                     "STORE %llu 127.0.0.1 %u\n",
                     (unsigned long long)snt->id, (unsigned int)snt->port);
    if (n > 0 && (size_t)n < sizeof(buffer_dove_si_trova)) {
        // Inviamo la risposta STORE al Client prima di chiudere il socket
        (void)send_all(slot->fd, buffer_dove_si_trova, (size_t)n);
    }

    close_conn_slot(ctx, slot);
}

static void handle_get(coord_ctx_t *ctx, conn_slot_t *slot, const parsed_line_t *parsed) {
    if (ctx == NULL || slot == NULL || parsed == NULL) {
        return;
    }

    if (parsed->token_count != 2) {
        send_err_line(slot->fd, ERR_BAD_REQUEST);
        close_conn_slot(ctx, slot);
        return;
    }

    if (validate_remote_name(parsed->tokens[1]) < 0) {
        send_err_line(slot->fd, ERR_INVALID_NAME);
        close_conn_slot(ctx, slot);
        return;
    }

    int indice = find_file_index(ctx, parsed->tokens[1]);
    if (indice < 0) {
        send_err_line(slot->fd, ERR_NOT_FOUND);
        close_conn_slot(ctx, slot);
        return;
    }

    // Leggiamo la voce dall'array ctx->files (non ctx->slots) tramite puntatore
    const file_entry_t *fet = &ctx->files[indice];
    // Passiamo ctx come primo argomento per verificare che lo Storage sia ancora attivo
    storage_node_t *snt = find_active_storage_id(ctx, fet->storage_id);
    if (snt == NULL) {
        send_err_line(slot->fd, ERR_NOT_FOUND);
        //non so che errore mettere qui in qeusto caso
        close_conn_slot(ctx, slot);
        return;
    }
    //altrimenti invio la stringa al client e chiudo la connessione

    char buffer_found[MAX_LINE_LEN];
    // Il protocollo per GET prevede "FOUND <storage_id> 127.0.0.1 <storage_port> <size>\n"
    int n = snprintf(buffer_found, sizeof(buffer_found),
                     "FOUND %llu 127.0.0.1 %u %llu\n",
                     (unsigned long long)fet->storage_id,
                     (unsigned int)snt->port,
                     (unsigned long long)fet->size);
    if (n > 0 && (size_t)n < sizeof(buffer_found)) {
        // Spediamo la riga FOUND completa al Client
        (void)send_all(slot->fd, buffer_found, (size_t)n);
    }

    // Chiudiamo la connessione one-shot del Client
    close_conn_slot(ctx, slot);
}

//handler per comando LIST da parte del client
static void handle_list(coord_ctx_t *ctx, conn_slot_t *slot, const parsed_line_t *parsed) {
    if (ctx == NULL || slot == NULL || parsed == NULL) {
        return;
    }

    if (parsed->token_count != 1) {
        send_err_line(slot->fd, ERR_BAD_REQUEST);
        close_conn_slot(ctx, slot);
        return;
    }

    // Ordiniamo in-place l'indice dei file in ordine crescente byte-per-byte tramite strcmp
    if (ctx->file_count > 1) {
        qsort(ctx->files, ctx->file_count, sizeof(file_entry_t), cmp_file_entries_by_name);
    }

    char line[MAX_LINE_LEN];
    // Inviamo l'intestazione "FILES <n>\n" (se file_count == 0 invia "FILES 0\n")
    int n = snprintf(line, sizeof(line), "FILES %llu\n", (unsigned long long)ctx->file_count);
    if (n < 0 || (size_t)n >= sizeof(line) || send_all(slot->fd, line, (size_t)n) < 0) {
        close_conn_slot(ctx, slot);
        return;
    }

    // Inviamo una riga "FILE <remote_name> <size> <storage_id>\n" per ciascun file nell'indice
    for (size_t i = 0; i < ctx->file_count; ++i) {
        n = snprintf(line, sizeof(line), "FILE %s %llu %llu\n",
                     ctx->files[i].name,
                     (unsigned long long)ctx->files[i].size,
                     (unsigned long long)ctx->files[i].storage_id);
        if (n < 0 || (size_t)n >= sizeof(line) || send_all(slot->fd, line, (size_t)n) < 0) {
            close_conn_slot(ctx, slot);
            return;
        }
    }

    // Inviamo la riga conclusiva "END\n" e chiudiamo la connessione one-shot del Client
    (void)send_all(slot->fd, "END\n", 4);
    close_conn_slot(ctx, slot);
}

//===========================================
//FUNZIONI PER LA GESTIONE DELLE CONNESSIONI
//===========================================

//per creare il set da leggere per la select
static int build_read_fdset(const coord_ctx_t *ctx, int listen_fd, fd_set *readfds) {
    // Azzera il set e inserisce sempre il socket di ascolto principale per nuove accept()
    FD_ZERO(readfds);
    FD_SET(listen_fd, readfds);
    int max_fd = listen_fd;

    // Aggiunge al set tutti i socket attualmente aperti nella tabella slots[]
    for (int i = 0; i < MAX_CONNS; ++i) {
        int fd = ctx->slots[i].fd;
        if (fd >= 0) {
            FD_SET(fd, readfds);
            if (fd > max_fd) {
                max_fd = fd;
            }
        }
    }

    // Ritorna il descrittore più alto trovato (nel main andrà passato max_fd + 1 a select)
    return max_fd;
}

//gestione concorrente dei canali tramite select 
static void process_slot_read(coord_ctx_t *ctx, conn_slot_t *slot) {
    ssize_t r = conn_fill(&slot->conn);
    if (r == 0) {
        //EOF: chiusura pulita dall'altra parte
        close_conn_slot(ctx, slot);
        return;
    }
    if (r < 0) {
        //falso risveglio o dati esauriti, torniamo alla select
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        close_conn_slot(ctx, slot);
        return;
    }

    //estrae tutte le righe complete presenti nel buffer, un segmento TCP può contenere più messaggi accodati
    while (slot->fd >= 0) {
        char line[MAX_LINE_LEN];
        size_t len = 0;
        // Estraiamo una riga completa terminata da '\n' dal buffer conn_t dello slot
        line_status_t st = conn_try_extract_line(&slot->conn, line, sizeof(line), &len);

        if (st == LINE_INCOMPLETE) {
            //riga imparziale (manca \n), ritorno al while
            break;
        }

        if (st == LINE_OVERFLOW) {
            send_err_line(slot->fd, ERR_BAD_REQUEST);
            close_conn_slot(ctx, slot);
            break;
        }

        parsed_line_t parsed;
        if (parse_control_line(line, (int)len, &parsed) < 0 || parsed.token_count < 1) {
            send_err_line(slot->fd, ERR_BAD_REQUEST);
            close_conn_slot(ctx, slot);
            break;
        }

        //smistiamo in base al comando usato e al ruolo
        if (slot->role == ROLE_STORAGE) {
            handle_stored(ctx, slot, &parsed);
        } else if (strcmp(parsed.tokens[0], "REGISTER") == 0) {
            handle_register(ctx, slot, &parsed);
        } else if (strcmp(parsed.tokens[0], "PUT") == 0) {
            handle_put(ctx, slot, &parsed);
        } else if (strcmp(parsed.tokens[0], "GET") == 0) {
            handle_get(ctx, slot, &parsed);
        } else if (strcmp(parsed.tokens[0], "LIST") == 0) {
            handle_list(ctx, slot, &parsed);
        } else {
            send_err_line(slot->fd, ERR_BAD_REQUEST);
            close_conn_slot(ctx, slot);
        }
    }
}

// Valida gli argomenti passati al coordinator: ./coordinator <coordinator_port>
static int parse_coord_args(int argc, char **argv, uint16_t *out_coord_port) {
    if (argc != 2 || argv == NULL || out_coord_port == NULL) {
        return -1;
    }

    // Verifica che la porta sia un intero decimale puro compreso tra 1 e 65535
    if (parse_strict_port(argv[1], out_coord_port) < 0) {
        fprintf(stderr, "Errore: coordinator_port non valida ('%s')\n", argv[1]);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    // Configura SIGPIPE (ignorato) e SIGINT/SIGTERM (terminazione pulita tramite g_stop)
    if (setup_signals() < 0) {
        exit(EXIT_FAILURE);
    }

    uint16_t coord_port = 0;
    // Valida la porta passata da riga di comando
    if (parse_coord_args(argc, argv, &coord_port) < 0) {
        fprintf(stderr, "Uso: %s <coordinator_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Inizializza la struttura di contesto globale del Coordinator
    coord_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    // Gli storage_id sono interi positivi assegnati a partire da 1 in ordine crescente
    ctx.next_storage_id = 1;
    for (int i = 0; i < MAX_CONNS; ++i) {
        reset_conn_slot(&ctx.slots[i]);
    }

    // Crea il socket TCP di ascolto con SO_REUSEADDR sulla porta specificata
    int listen_fd = create_listening_socket(coord_port, LISTEN_BACKLOG);
    if (listen_fd < 0) {
        perror("Errore creazione socket di ascolto coordinator");
        exit(EXIT_FAILURE);
    }

    // Imposta non-bloccante anche il socket di ascolto per evitare blocchi su accept()
    if (set_nonblocking(listen_fd) < 0) {
        perror("Errore set_nonblocking su listen_fd");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    int exit_code = EXIT_SUCCESS;

    // Loop principale guidato dagli eventi I/O tramite select() fino a SIGINT/SIGTERM
    while (!g_stop) {
        fd_set readfds;
        // Costruisce il set dei descrittori da monitorare (listen_fd + tutti gli slot attivi)
        int max_fd = build_read_fdset(&ctx, listen_fd, &readfds);

        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            // Se select() è stata interrotta da SIGINT/SIGTERM ricontrolla !g_stop ed esce pulitamente
            if (errno == EINTR) {
                continue;
            }
            perror("Errore select nel coordinator");
            exit_code = EXIT_FAILURE;
            break;
        }

        // Controlla se sono arrivate nuove connessioni TCP sul socket di ascolto
        if (FD_ISSET(listen_fd, &readfds)) {
            while (!g_stop) {
                int new_fd = accept(listen_fd, NULL, NULL);
                if (new_fd < 0) {
                    // EAGAIN/EWOULDBLOCK indica che non ci sono altre connessioni in coda
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("Errore accept nel coordinator");
                    break;
                }

                // Rende non bloccante la nuova connessione (requisito anti-blocco su client lenti)
                if (set_nonblocking(new_fd) < 0) {
                    perror("Errore set_nonblocking su nuovo client");
                    close(new_fd);
                    continue;
                }

                // Cerca uno slot libero nella tabella delle connessioni
                conn_slot_t *slot = find_free_conn_slot(&ctx);
                if (slot == NULL) {
                    fprintf(stderr, "Avviso: raggiunto limite massimo connessioni (%d)\n", MAX_CONNS);
                    close(new_fd);
                    continue;
                }

                // Associa il descrittore allo slot in stato ROLE_UNKNOWN in attesa della prima riga
                slot->fd = new_fd;
                slot->role = ROLE_UNKNOWN;
                slot->storage_id = 0;
                conn_init(&slot->conn, new_fd);
            }
        }

        // Scorre tutti gli slot attivi per servire i socket pronti in lettura
        for (int i = 0; i < MAX_CONNS; ++i) {
            int fd = ctx.slots[i].fd;
            if (fd >= 0 && FD_ISSET(fd, &readfds)) {
                process_slot_read(&ctx, &ctx.slots[i]);
            }
        }
    }

    // Pulizia finale: chiude il socket di ascolto e tutte le connessioni ancora aperte
    close(listen_fd);
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (ctx.slots[i].fd >= 0) {
            close_conn_slot(&ctx, &ctx.slots[i]);
        }
    }

    // Libera gli array dinamici allocati nell'heap
    free(ctx.storages);
    free(ctx.files);

    return exit_code;
}