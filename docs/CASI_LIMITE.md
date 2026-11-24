ANALISI DEI CASI LIMITE
======================================================================
CASI LIMITE COPERTI
======================================================================

1.1 Rete, Socket Non Bloccanti e Framing TCP
----------------------------------------------------------------------

- Client lento o invio frammentato (1 byte alla volta):
  * Problema: Un Client invia la riga di comando un byte per volta o
    tarda a inviare '\n', rischiando di bloccare il Coordinator.
  * Soluzione adottata: Nel Coordinator tutti i socket accettati sono
    impostati con set_nonblocking() e gestiti via select(). Ogni slot in
    conn_slot_t usa un buffer conn_t dedicato: conn_fill() legge solo i
    byte disponibili e conn_try_extract_line() ritorna LINE_INCOMPLETE
    finche' non arriva '\n', restituendo subito il controllo a select().

- Accorpamento di piu' messaggi in un singolo segmento TCP (TCP Coalescing):
  * Problema: Piu' righe di controllo (es. REGISTER + STORED) o un header
    seguito dai primi byte del payload binario arrivano nella stessa recv().
  * Soluzione adottata: conn_t conserva i byte successivi al primo '\n'
    tramite buf_start e buf_len; process_slot_read() itera nel ciclo
    while (slot->fd >= 0) finche' tutte le righe complete nel buffer non
    sono state estratte.

- Riga di controllo oltre il limite massimo ($\ge 512$ byte senza '\n'):
  * Problema: Un peer invia $512$ o piu' byte senza newline.
  * Soluzione adottata: conn_try_extract_line() rileva LINE_OVERFLOW, il
    server risponde ERR BAD_REQUEST tramite send_err_line() e chiude
    immediatamente la connessione con close_conn_slot().

- Scrittura su socket gia' chiuso dall'altro capo (SIGPIPE / EPIPE):
  * Problema: Scrivere su un socket chiuso dal peer genera SIGPIPE
    terminando il server.
  * Soluzione adottata: setup_signals() imposta signal(SIGPIPE, SIG_IGN),
    trasformando il segnale in un normale errore errno == EPIPE gestito
    con la chiusura dello slot.

- Riavvio rapido dei server sulla stessa porta (TIME_WAIT):
  * Problema: bind() fallisce con EADDRINUSE subito dopo uno stop.
  * Soluzione adottata: create_listening_socket() attiva SO_REUSEADDR
    prima di bind().


1.2 Validazione del Protocollo e Precedenza degli Errori
----------------------------------------------------------------------

- Ordine tassativo degli errori su richieste con piu' difetti:
  * Soluzione adottata: Gli handler (handle_put, handle_get, handle_list,
    handle_register, handle_upload, handle_download) verificano i vincoli
    nella sequenza esatta richiesta dalle specifiche, fermandosi al primo
    errore riscontrato:
      1. ERR BAD_REQUEST (numero token, '\r\n', spazi iniziali/finali/multipli)
      2. ERR INVALID_NAME
      3. ERR INVALID_SIZE / ERR INVALID_PORT
      4. ERR FILE_EXISTS / ERR NOT_FOUND
      5. ERR NO_STORAGE

- Delimitatori non conformi ('\r\n') e spazi multipli:
  * Soluzione adottata: parse_control_line() rifiuta '\r', spazi
    iniziali/finali e doppi spazi consecutivi.

- Nomi remoti malformati o path traversal:
  * Soluzione adottata: validate_remote_name() ammette solo caratteri in
    [A-Za-z0-9._-] di lunghezza compresa in $[1, 64]$, escludendo ".",
    ".." e il suffisso ".part" su Client, Coordinator e Storage.

- Interi non canonici (size e port):
  * Soluzione adottata: parse_strict_uint64() e parse_strict_port()
    accettano solo cifre decimale '0'..'9' controllando l'overflow
    aritmetico; size e' limitata a MAX_FILE_SIZE ($10\,485\,760$ byte).


1.3 Trasferimento File e Integrita' dei Dati
----------------------------------------------------------------------

- File vuoto ($0$ byte) e dimensioni di confine ($4095$, $4096$, $4097$ byte):
  * Soluzione adottata: size == 0 e' gestito senza attendere byte dal
    socket dopo READY; tutti gli altri trasferimenti avvengono in
    streaming a chunk di al massimo CHUNK_SIZE ($4096$ byte) senza allocare
    buffer grandi quanto l'intero file.

- Disconnessione prematura durante UPLOAD o DOWNLOAD:
  * Soluzione adottata: Sia lo Storage (UPLOAD) sia il Client (DOWNLOAD)
    scrivono su un file temporaneo <nome>.part (aperto con
    O_CREAT | O_EXCL lato Storage) ed eseguono rename() solo al
    completamento di tutti i size byte, invocando unlink() in caso di
    interruzione.

- File .part residui da arresti anomali precedenti:
  * Soluzione adottata: All'avvio dello Storage,
    verify_and_clean_storage_dir() verifica S_ISDIR ed elimina con
    unlink() eventuali file .part orfani.

- Fallimento della notifica STORED verso il Coordinator:
  * Soluzione adottata: Se notify_coordinator_stored() fallisce, lo
    Storage rimuove il file appena rinominato tramite unlink(final_path),
    risponde ERR IO al Client e termina con EXIT_FAILURE.

- Incongruenza di dimensione tra FOUND e DATA:
  * Soluzione adottata: Il Client confronta la dimensione ricevuta in
    DATA con quella annunciata in FOUND e interrompe con errore in caso
    di discrepanza.


1.4 Ciclo di Vita dei Nodi, Indice e Segnali
----------------------------------------------------------------------

- Caduta improvvisa di uno Storage:
  * Soluzione adottata: select() rileva EOF sul socket persistente dello
    Storage e close_conn_slot() invoca remove_storage() e
    remove_files_by_storage(), marcando il nodo inattivo e compattando
    ctx.files[] in complessita' $O(N)$.

- Non riutilizzo degli storage_id ed espansione dinamica:
  * Soluzione adottata: next_storage_id parte da $1$ ed e' strettamente
    crescente; ensure_storage_capacity() ed ensure_file_capacity()
    espandono gli array tramite realloc() salvando il puntatore precedente
    per evitare memory leak.

- Routing PUT, ordinamento LIST, STORED malformato e segnali:
  * Soluzione adottata: find_min_id_active_storage() seleziona lo Storage
    attivo con id minimo; handle_list() ordina con qsort() e strcmp();
    handle_stored() malformato chiude lo slot rimuovendo lo Storage;
    sigaction() senza SA_RESTART garantisce l'uscita pulita su SIGINT e
    SIGTERM.


======================================================================
CASI NON RISOLTI E POSSIBILI RISOLUZIONI
======================================================================
2.1 Riempimento del buffer di invio TCP su socket non bloccante nel
    Coordinator (EAGAIN / EWOULDBLOCK)
----------------------------------------------------------------------

- Situazione attuale (Non risolta):
  In coordinator.c i socket accettati sono impostati con
  set_nonblocking(), ma gli handler inviano le risposte tramite
  send_all(). Questa funzione ritenta solo su EINTR ed esce con -1 su
  qualsiasi altro errore (incluso EAGAIN / EWOULDBLOCK). Su loopback
  (127.0.0.1) il buffer kernel (circa 212 KB) basta per oltre $2500$ voci
  in LIST, ma se un Client legge molto lentamente o l'indice cresce oltre
  tale soglia, send() fallisce con EAGAIN e il Coordinator tronca la
  connessione. (Nota: aggiungere un loop continue su EAGAIN dentro
  send_all() causerebbe polling attivo al 100% di CPU, vietato dalle
  specifiche).

- Strategie per rimediare:
  1. Buffer di uscita per slot + writefds in select() (Consigliata):
     Aggiungere a conn_slot_t i campi char *out_buf, size_t out_len,
     size_t out_sent e bool close_after_write. Gli handler accodano la
     risposta in out_buf; build_fdsets() inserisce slot->fd in writefds
     solo quando out_sent < out_len. Quando select() segnala il socket
     scrivibile, process_slot_write() esegue una singola send() non
     bloccante e chiude lo slot solo quando out_sent == out_len e
     close_after_write == true. Elimina ogni rischio di troncamento senza
     polling.
  2. Socket bloccante con SO_SNDTIMEO:
     Mantenere i socket bloccanti in scrittura impostando un timeout con
     setsockopt(..., SO_SNDTIMEO, ...). E' piu' semplice ma meno reattivo
     di writefds se un Client blocca completamente la lettura.


2.2 Race condition su PUT simultanee dello stesso remote_name su
    Storage diversi
----------------------------------------------------------------------

- Situazione attuale (Parzialmente risolta):
  Per specifica, la risposta STORE del Coordinator non prenota il
  remote_name nell'indice: il file diventa visibile solo quando lo
  Storage invia STORED. Se due Client avviano contemporaneamente
  "put stesso_file.txt" mentre sono attivi piu' Storage (o durante un
  upload lento), entrambi ricevono STORE. Sullo stesso Storage il secondo
  upload viene bloccato con ERR FILE_EXISTS da O_CREAT | O_EXCL, ma
  nell'indice del Coordinator upsert_file_entry() sovrascrive
  silenziosamente size e storage_id con l'ultimo STORED arrivato,
  lasciando eventualmente un file orfano su uno Storage che non e' piu'
  referenziato dall'indice.

- Strategie per rimediare:
  1. Rifiuto lato Coordinator in handle_stored():
     Se upsert_file_entry() rileva che remote_name e' gia' presente
     nell'indice (inserito pochi millisecondi prima da un altro Storage),
     restituisce un errore applicativo (ERR FILE_EXISTS) al posto di ACK.
     Lo Storage che riceve ERR FILE_EXISTS al posto di ACK esegue
     unlink(final_path) eliminando il duplicato fisico e propaga
     ERR FILE_EXISTS al proprio Client.
  2. Stato Pending / Reserved con TTL nell'indice:
     Aggiungere in file_entry_t un flag bool committed e un timestamp
     time_t reserved_at. Alla PUT si inserisce la voce con
     committed = false: GET e LIST la ignorano, mentre una seconda PUT
     sullo stesso nome riceve subito ERR FILE_EXISTS (a meno che
     now - reserved_at > RESERVATION_TTL, nel qual caso la prenotazione
     scaduta viene sovrascritta). Alla ricezione di STORED si imposta
     committed = true.


2.3 Desincronizzazione tra file residui su disco e indice al riavvio
    dello Storage
----------------------------------------------------------------------

- Situazione attuale (Non risolta per vincolo di specifica):
  Le specifiche stabiliscono che "I file eventualmente gia' presenti
  nella storage_directory all'avvio non vengono annunciati al
  Coordinator". Se uno Storage termina e viene riavviato sulla stessa
  directory contenente file caricati nella sessione precedente, il
  Coordinator non li conosce (quindi LIST e GET non li vedono e PUT sullo
  stesso nome viene autorizzata con STORE), ma quando il Client tentera'
  l'UPLOAD sullo Storage ricevera' ERR FILE_EXISTS perche' il file esiste
  fisicamente nella cartella locale.

- Strategie per rimediare:
  1. Sincronizzazione dell'inventario durante l'handshake REGISTER
     (Estensione protocollo):
     Estendere il protocollo di avvio affinche' lo Storage invii, subito
     dopo REGISTER <port>, la lista dei file regolari presenti nella
     propria directory (FILE <name> <size> ... END), consentendo al
     Coordinator di ripopolare l'indice.
  2. Bonifica completa della directory all'avvio:
     Se il sistema non prevede persistenza tra riavvii di un nodo
     Storage, estendere verify_and_clean_storage_dir() affinche' rimuova
     tutti i file regolari residui (non solo i .part) prima di registrarsi
     presso il Coordinator, garantendo coerenza assoluta tra disco e
     indice.


2.4 Trasferimenti dati iterativi nello Storage (concorrenza Client su
    un singolo nodo)
----------------------------------------------------------------------

- Situazione attuale (Non risolta per semplificazione ammessa):
  Come consentito dalle specifiche ("Non e' obbligatorio gestire piu'
  upload/download contemporanei sullo stesso Storage; e' ammesso un
  server dati iterativo"), storage.c usa select() per monitorare
  listen_fd e coord_fd, ma una volta chiamato accept() esegue
  serve_client_connection() in modo bloccante fino al termine completo
  dell'UPLOAD o del DOWNLOAD. Se un Client invia un file da 10 MiB molto
  lentamente, gli altri Client diretti allo stesso Storage attendono
  nella backlog TCP e un'eventuale caduta del Coordinator viene rilevata
  solo al termine del trasferimento.

- Strategie per rimediare:
  1. I/O Multiplexing completo anche sui trasferimenti dati nello Storage:
     Mantenere un array di slot Client non bloccanti nel loop select()
     dello Storage con una macchina a stati per slot (ST_READ_CMD,
     ST_RECV_PAYLOAD, ST_SEND_PAYLOAD), leggendo/scrivendo al massimo
     CHUNK_SIZE ($4096$ byte) per ciascun Client pronto ad ogni iterazione
     di select().
  2. Modello ibrido select() + Worker Thread (pthread):
     Lasciare select() per listen_fd e coord_fd e delegare ogni
     client_sd accettato a un thread pthread dedicato per lo streaming su
     disco, proteggendo con un pthread_mutex_t l'invio di STORED e la
     ricezione di ACK su coord_fd.


2.5 Limite fisso di connessioni (MAX_CONNS = 128) e Client inattivi
    (Idle / Slowloris)
----------------------------------------------------------------------

- Situazione attuale (Non risolta):
  La tabella ctx.slots[MAX_CONNS] supporta fino a $128$ connessioni
  simultanee; se viene superata, il Coordinator chiude immediatamente il
  nuovo socket con close(new_fd) senza inviare un messaggio d'errore.
  Inoltre select() ha un limite strutturale pari a FD_SETSIZE ($1024$ su
  Linux). Come previsto dalle specifiche ("Non e' richiesta la gestione
  di timeout per Client che restano connessi senza inviare dati"), un
  Client che apre la connessione e non invia nulla non blocca il
  Coordinator, ma occupa permanentemente uno slot.

- Strategie per rimediare:
  1. Timeout di inattivita' sugli slot ROLE_UNKNOWN:
     Aggiungere un campo time_t connected_at in conn_slot_t, passare un
     timeout (es. $1$ secondo) come quinto parametro a select() e chiudere
     gli slot con role == ROLE_UNKNOWN che non completano una riga entro
     IDLE_TIMEOUT_SEC (es. $15$ secondi).
  2. Migrazione da select() a poll():
     Sostituire fd_set con un array dinamico struct pollfd[] per
     superare il limite di FD_SETSIZE = 1024 e far crescere dinamicamente
     la tabella degli slot in base al carico.


2.6 Crash silenzioso o congelamento di uno Storage senza chiusura TCP
    (SIGSTOP / partizione di rete)
----------------------------------------------------------------------

- Situazione attuale (Non risolta):
  Il Coordinator rileva la caduta di uno Storage quando il kernel chiude
  il socket TCP (EOF in conn_fill(), cosa che accade regolarmente su
  exit(), SIGINT, SIGTERM o SIGKILL). Tuttavia, se un processo Storage
  viene sospeso con SIGSTOP o resta bloccato in un deadlock senza
  chiudere il file descriptor, il socket TCP rimane aperto e il
  Coordinator continua a considerarlo active = true, indirizzando i
  Client verso uno Storage che non risponde.

- Strategie per rimediare:
  1. Meccanismo di HEARTBEAT periodico con Watchdog su select():
     Introdurre un messaggio di controllo periodico HEARTBEAT\n inviato
     dallo Storage ogni $T$ secondi (es. $3$ secondi) sfruttando il
     parametro struct timeval *timeout della select() dello Storage, e far
     tracciare al Coordinator il campo time_t last_seen per ogni
     storage_node_t: se (now - last_seen) > 3 * T, il Coordinator chiude
     lo slot dello Storage e rimuove i suoi file dall'indice.