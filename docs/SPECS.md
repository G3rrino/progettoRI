DOCUMENTO DEI REQUISITI E VINCOLI DI PROGETTO - MINISTORE
======================================================================

1. REQUISITI FUNZIONALI (RF)
----------------------------------------------------------------------

[RF-01] Avvio, CLI ed Exit Status
  Il sistema deve produrre tre eseguibili nella directory radice:
  - ./coordinator <coordinator_port>
  - ./storage <coordinator_port> <storage_port> <storage_directory>
  - ./client <coordinator_port> <put|get|list> ...
  Il Client esegue una singola operazione e termina con exit status 0
  in caso di successo o diverso da 0 in caso di errore. Coordinator e
  Storage restano in esecuzione fino alla ricezione di SIGINT o SIGTERM,
  terminando con exit status diverso da 0 (metteremo exit_status
  (EXIT_FAILURE) per compatibilità con qualsiasi macchina in cui 
  viene eseguito) se i parametri iniziali non sono validi oppure se lo 
  Storage non riesce a registrarsi presso il Coordinator.

[RF-02] Inizializzazione e Registrazione Storage
  All'avvio lo Storage verifica che storage_directory esista e sia una
  directory, rimuove eventuali file .part residui, apre il listening
  socket sulla storage_port e stabilisce una connessione TCP persistente
  verso il Coordinator inviando REGISTER <storage_port>. Solo dopo aver
  ricevuto REGISTERED <storage_id> (con ID intero positivo assegnato in
  ordine crescente a partire da 1 e mai riutilizzato) inizia ad
  accettare connessioni dai Client. Se il Coordinator chiude la
  connessione persistente, lo Storage termina con exit status diverso
  da 0.

[RF-03] Multiplexing e Indice Globale nel Coordinator
  Il Coordinator accetta sulla stessa porta sia Storage sia Client,
  distinguendoli dal primo messaggio ricevuto: REGISTER identifica una
  connessione persistente Storage; PUT, GET o LIST identificano una
  connessione one-shot Client. Il Coordinator aggiorna l'indice globale
  alla ricezione di STORED <remote_name> <size> rispondendo ACK. In
  caso di EOF, errore di rete o messaggio STORED malformato sulla
  connessione persistente, rimuove lo Storage dall'elenco dei nodi
  attivi e cancella dall'indice tutti i file ad esso associati.

[RF-04] Caricamento File (PUT e UPLOAD)
  Il Client verifica l'esistenza del file locale, ne ricava la
  dimensione tramite stat()/fstat(), valida remote_name e richiede
  PUT <remote_name> <size> al Coordinator. Il Coordinator seleziona lo
  Storage attivo con storage_id piu' piccolo e risponde
  STORE <storage_id> 127.0.0.1 <storage_port> senza prenotare il nome
  nell'indice. Il Client invia quindi UPLOAD <remote_name> <size> allo
  Storage, attende READY e trasmette esattamente size byte. Lo Storage
  salva su <remote_name>.part, rinomina atomicamente in <remote_name>
  a ricezione completata, notifica STORED al Coordinator attendendo
  ACK e risponde STORED <remote_name> <size> al Client.

[RF-05] Recupero File (GET e DOWNLOAD)
  Il Client interroga il Coordinator con GET <remote_name> ottenendo
  FOUND <storage_id> 127.0.0.1 <storage_port> <size>, quindi richiede
  DOWNLOAD <remote_name> allo Storage indicato. Lo Storage risponde
  DATA <size> seguito da esattamente size byte. Il Client verifica che
  la dimensione in DATA coincida con quella in FOUND, salva i dati su
  <local_file>.part e lo rinomina in <local_file> (sovrascrivendo se
  gia' presente) solo al completamento di tutti i byte, eliminando il
  file .part in caso di disconnessione prematura.

[RF-06] Elenco File Disponibili (LIST)
  Alla richiesta LIST del Client, il Coordinator risponde con
  FILES <n>, seguito da n righe FILE <remote_name> <size> <storage_id>
  ordinate in senso crescente byte-per-byte secondo strcmp() (lettere
  maiuscole precedenti alle minuscole), e chiude con END (oppure
  FILES 0 seguito da END se l'indice e' vuoto).

[RF-07] Validazione Rigorosa e Gestione Errori
  Client, Coordinator e Storage validano sintassi, tipi interi e nomi
  remoti ([A-Za-z0-9._-]+, lunghezza 1..64, diverso da ".", privo di
  ".." e non terminante con .part). In presenza di piu' errori
  contemporanei, il server risponde con il primo secondo l'ordine
  tassativo:
    1. ERR BAD_REQUEST
    2. ERR INVALID_NAME
    3. ERR INVALID_SIZE / ERR INVALID_PORT
    4. ERR FILE_EXISTS / ERR NOT_FOUND
    5. ERR NO_STORAGE


2. REQUISITI NON FUNZIONALI (RNF)
----------------------------------------------------------------------

[RNF-01] Concorrenza e assenza di blocchi (Coordinator)
  Il Coordinator deve mantenere contemporaneamente le connessioni
  persistenti di piu' Storage e servire almeno 5 Client simultanei
  senza bloccarsi su Client lenti (anche in caso di invio a 1 byte alla
  volta).

[RNF-02] Disponibilita' del canale di controllo (Storage)
  Lo Storage deve mantenere attiva e reattiva la connessione di
  controllo verso il Coordinator mentre accetta richieste dati dai
  Client.

[RNF-03] Trasferimento in streaming e memoria limitata
  E' vietato allocare buffer grandi quanto l'intero file; ogni
  iterazione di lettura/scrittura del payload deve elaborare al massimo
  4096 byte. Le righe di controllo non devono superare i 512 byte
  (carattere '\n' incluso).

[RNF-04] Affidabilita' e gestione dei segnali
  Coordinator e Storage devono ignorare SIGPIPE (signal(SIGPIPE, SIG_IGN)
  o MSG_NOSIGNAL) trattando EPIPE come disconnessione, gestire SIGINT e
  SIGTERM per un arresto pulito e utilizzare file temporanei .part per
  garantire atomicita' su disco. Si raccomanda l'uso di SO_REUSEADDR sui
  listening socket.

[RNF-05] Capacita' minime
  Il Coordinator deve supportare almeno 16 nodi Storage registrati
  contemporaneamente e almeno 1024 file nell'indice globale.

[RNF-06] Qualita' di compilazione e ambiente target
  Il codice deve compilare ed essere eseguito su Ubuntu 24 tramite
  Makefile con flag -Wall abilitato producendo zero errori e zero
  warning.


3. VINCOLI TECNOLOGICI E ARCHITETTURALI (VT)
----------------------------------------------------------------------

[VT-01] Linguaggio e Stack di Rete
  Implementazione in linguaggio C standard con uso esclusivo di socket
  TCP/IPv4 sull'indirizzo di loopback 127.0.0.1 e porte passate
  dinamicamente a runtime (mai codificate nel sorgente).

[VT-02] Separazione Piano di Controllo e Piano Dati
  Il Coordinator gestisce esclusivamente traffico di controllo testuale
  e non deve mai ricevere, inoltrare o transitare il contenuto binario
  dei file.

[VT-03] Divieto di IPC alternativo
  E' vietato sostituire le comunicazioni socket tra i processi mediante
  file condivisi, memoria condivisa (shm_open, mmap), pipe o code di
  messaggi.

[VT-04] Framing del Protocollo Applicativo
  - Messaggi di controllo in ASCII terminati da un singolo '\n' ('\r\n'
    e' considerato ERR_BAD_REQUEST).
  - Campi separati da esattamente uno spazio (spazi iniziali, finali o
    multipli causano ERR_BAD_REQUEST).
  - Interi rappresentati esclusivamente da cifre decimali ASCII '0'..'9'
    senza segno ne' altri caratteri (se rappresentati con segno, ERR_BAD_
    REQUEST)
  - Dimensione massima del file pari a 10 MB (esattamente 10485760 byte)
    (se viene superata la dimensione massima, ERR_INVALID_SIZE)
  - Gestione esplicita dello stream TCP tramite buffer di accumulo per
    connessione (nessuna assunzione di corrispondenza 1-a-1 tra send() e
    recv()) e cicli completi di send().

[VT-05] Struttura della Consegna
  Archivio <matricola>.zip contenente esclusivamente:
  - Makefile
  - sorgenti *.c e *.h
  - report.pdf (massimo 3 pagine, almeno 3 interazioni AI documentate)
  - validation_report.txt (generato da validator_studente.py)
  E' vietato includere file binari compilati o directory di build.


4. CRITERI DI ACCETTAZIONE (CA)
----------------------------------------------------------------------

[CA-01] Compilazione
  "make clean && make" dalla directory radice genera coordinator,
  storage e client senza errori ne' warning sotto gcc -Wall.

[CA-02] Registrazione e Routing
  Lo Storage riceve ID progressivi a partire da 1 (REGISTERED <id>);
  PUT instrada sempre verso lo Storage attivo con storage_id minimo;
  STORE non inserisce il file nell'indice prima di STORED.

[CA-03] Integrita' End-to-End
  I file caricati tramite "./client ... put" e recuperati tramite
  "./client ... get" risultano identici byte-per-byte agli originali.

[CA-04] Dimensioni Limite
  Trasferimenti completati con successo per file di dimensione 0, 1,
  4095, 4096, 4097 byte e fino a 10485760 byte (10 MB); rifiuto con
  ERR INVALID_SIZE oltre i 10 MB.

[CA-05] Framing TCP Frammentato
  Righe di controllo inviate 1 byte alla volta o header e payload
  accorpati nella stessa send() vengono ricostruiti correttamente senza
  perdita di byte ne' blocchi.

[CA-06] Formato e Ordinamento LIST
  L'output di LIST rispetta la sintassi FILES <n> / FILE <name> <size>
  <id> / END ed e' ordinato secondo strcmp().

[CA-07] Gestione Errori e Precedenze
  Tutti gli errori obbligatori (ERR BAD_REQUEST, ERR INVALID_NAME,
  ERR INVALID_SIZE, ERR INVALID_PORT, ERR NO_STORAGE, ERR FILE_EXISTS,
  ERR NOT_FOUND, ERR IO) sono restituiti secondo l'ordine di priorita'
  stabilito.

[CA-08] Fault Tolerance e Pulizia
  La chiusura improvvisa di uno Storage rimuove immediatamente il nodo
  e i suoi file dall'indice; l'interruzione di un upload o download
  elimina i file .part temporanei senza lasciare voci corrotte.

[CA-09] Concorrenza Coordinator
  Un Client lento che invia dati a rilento non impedisce ad altri 5 o
  piu' Client simultanei o agli Storage di ricevere risposta immediata.

[CA-10] Validazione e Documentazione
  Esecuzione completa di "python3 validator_studente.py" con
  generazione di validation_report.txt e redazione di report.pdf
  conforme alle linee guida.


5. ELEMENTI FUORI DALLO SCOPO (OUT OF SCOPE)
----------------------------------------------------------------------

- Comandi di modifica o cancellazione remota: non sono previste
  operazioni per eliminare (DELETE), rinominare o sovrascrivere file
  gia' registrati nell'indice del Coordinator (un nome gia' presente
  restituisce ERR FILE_EXISTS).
- Persistenza dell'indice e recupero di file preesistenti: il
  Coordinator mantiene l'indice esclusivamente in memoria volatile; i
  file eventualmente gia' presenti nella storage_directory all'avvio
  dello Storage non vengono annunciati al Coordinator.
- Replicazione e bilanciamento avanzato: i file non vengono replicati
  su piu' nodi Storage e il bilanciamento del carico non considera
  spazio disco residuo o banda, attenendosi alla regola deterministica
  dello storage_id attivo piu' piccolo.
- Heartbeat periodici e timeout su Client inattivi: non e' richiesto un
  meccanismo di heartbeat applicativo tra Storage e Coordinator (la
  disponibilita' coincide con lo stato della connessione TCP
  persistente), ne' e' richiesto di disconnettere Client inattivi
  purche' non blocchino il Coordinator.
- Trasferimenti dati paralleli sul singolo Storage: lo Storage non e'
  tenuto a gestire piu' upload/download simultanei (e' esplicitamente
  ammesso un server dati iterativo).
- Sicurezza e comunicazione multi-host: sono esclusi indirizzi IP
  diversi da 127.0.0.1, autenticazione degli utenti, controllo degli
  accessi, cifratura TLS e calcolo di digest/checksum applicativi.