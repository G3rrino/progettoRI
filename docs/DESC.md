SPECIFICHE DI PROGETTO - MINISTORE (RETI INFORMATICHE COD. 54511)
======================================================================

1. ARCHITETTURA GENERALE E VINCOLI DI SISTEMA
----------------------------------------------------------------------

- Obiettivo: Sistema distribuito basato su socket per memorizzare e
  recuperare file (MiniStore).
- Linguaggio e rete: Linguaggio C; uso esclusivo di socket TCP/IPv4
  sulla stessa macchina tramite indirizzo loopback 127.0.0.1. I processi
  devono comportarsi come entita' di rete indipendenti.
- Meccanismi vietati: E' vietato usare file condivisi, memoria
  condivisa, pipe o altri meccanismi IPC per sostituire le
  comunicazioni via socket tra le entita'.

Ruoli delle tre entita':
1) Coordinator:
   - Mantiene l'elenco degli Storage attivi e l'indice globale dei file.
   - Non riceve, non inoltra e non trasferisce mai il contenuto dei file.
2) Storage:
   - Memorizza fisicamente i file in una directory locale (ogni istanza
     di Storage deve avere una directory distinta).
   - Si registra presso il Coordinator e accetta connessioni TCP dai
     Client per operazioni di upload e download.
3) Client:
   - Interroga il Coordinator per sapere dove salvare o trovare un file
     e trasferisce il contenuto direttamente con lo Storage indicato.

Separazione obbligatoria tra piano di controllo e piano dati:
  Client <---- TCP controllo --------------------> Coordinator
  Client <---- TCP dati -------------------------> Storage
  Storage <--- TCP controllo persistente --------> Coordinator


2. ESEGUIBILI, PARAMETRI CLI ED EXIT STATUS
----------------------------------------------------------------------

Devono essere prodotti tre eseguibili nella radice del progetto:
- ./coordinator <coordinator_port>
- ./storage <coordinator_port> <storage_port> <storage_directory>
- ./client <coordinator_port> put <local_file> <remote_name>
- ./client <coordinator_port> get <remote_name> <local_file>
- ./client <coordinator_port> list

Regole di esecuzione:
- Le porte sono fornite a runtime (mai codificate nel sorgente); l'IP
  e' fissato a 127.0.0.1 e non va passato da riga di comando.
- Client: esegue una singola operazione e termina. Exit status 0 in
  caso di successo, diverso da 0 in caso di errore. L'output stampato
  dal Client per debug e' libero e non viene usato dal protocollo.
- Coordinator e Storage: restano in esecuzione fino alla ricezione di
  SIGINT o SIGTERM. Se all'avvio un parametro non e' valido, oppure se
  lo Storage non riesce a registrarsi presso il Coordinator, il processo
  stampa un messaggio di errore e termina con exit status diverso da 0.


3. CONVENZIONI SU FILE, NOMI REMOTI E FRAMING TCP
----------------------------------------------------------------------

File e payload:
- Contenuto trattato come sequenza generica di byte (binario/raw): non
  e' ammesso interpretarlo riga per riga per determinare la fine.
- Dimensione ammessa: da $0$ byte (file vuoto) a $10\text{ MB}$ inclusi,
  dove $10\text{ MB}$ equivale esattamente a $10\,485\,760$ byte.
- La dimensione del file deve essere ricavata programmaticamente
  tramite stat() o fstat() (mai inserita manualmente).
- Trasferimento in streaming obbligatorio: non e' ammesso allocare un
  buffer grande quanto l'intero file; ogni iterazione di lettura o
  scrittura deve gestire al massimo $4096$ byte.

Nomi remoti (remote_name):
- Formato ammesso: [A-Za-z0-9._-]+ con lunghezza da $1$ a $64$
  caratteri.
- Vincoli negativi: non puo' essere ".", non puo' contenere "..", non
  puo' terminare con ".part" (suffisso riservato ai file temporanei).
- Spazi, slash (/) e backslash (\) non sono mai ammessi.
- I vincoli sul remote_name devono essere verificati sia dal Client
  (prima di contattare il Coordinator), sia dal Coordinator e dallo
  Storage.

Messaggi di controllo e interi:
- Formato testuale ASCII terminato da un singolo '\n'. Il carattere
  '\r' non fa parte del protocollo: una riga terminata da '\r\n' e'
  malformata (ERR BAD_REQUEST).
- Campi separati da esattamente uno spazio: spazi iniziali, finali o
  multipli rendono il messaggio malformato (ERR BAD_REQUEST).
- Lunghezza massima riga di controllo: $512$ byte, '\n' incluso. Se il
  ricevente accumula $512$ byte senza trovare '\n', risponde
  ERR BAD_REQUEST e chiude la connessione.
- Interi: rappresentazione decimale ASCII composta solo dalle cifre
  '0'..'9', senza segno ne' altri caratteri (es. -1 o 12a sono non
  validi: ERR INVALID_SIZE per size, ERR INVALID_PORT per port).

Framing TCP:
- Non assumere corrispondenza 1-a-1 tra send() e recv(): gestire
  ricezioni/invii parziali e piu' messaggi accorpati nello stesso
  flusso TCP.
- Accumulare i byte ricevuti in un buffer associato alla connessione;
  estrarre una riga completa solo quando compare '\n'; conservare e
  utilizzare i byte successivi al '\n' (appartenenti al messaggio o al
  payload seguente).
- Ripetere send() in ciclo fino all'invio completo di tutti i byte.
- Dopo DATA <size> o READY (in UPLOAD) vengono scambiati esattamente i
  byte dichiarati in size senza delimitatore di fine file.


4. PROTOCOLLO STORAGE <-> COORDINATOR
----------------------------------------------------------------------

Sequenza di avvio dello Storage:
1) Verifica che storage_directory esista e sia una directory, altrimenti
   termina con errore.
2) Crea il listening socket sulla storage_port.
3) Apre una connessione TCP persistente verso il Coordinator, invia
   REGISTER e attende REGISTERED.
4) Solo dopo la registrazione inizia ad accettare connessioni dai Client.
5) I file eventualmente gia' presenti nella storage_directory all'avvio
   non vengono annunciati al Coordinator; lo Storage puo' eliminare
   all'avvio eventuali file .part residui.

Registrazione dello Storage:
- Storage -> Coordinator: REGISTER <storage_port>
- Coordinator -> Storage: REGISTERED <storage_id>
- storage_id: intero positivo assegnato dal Coordinator in ordine
  crescente a partire da $1$ per ogni nuova registrazione valida; gli ID
  non vengono mai riutilizzati.
- Il Coordinator accetta sulla stessa porta sia Storage sia Client e
  li distingue dal primo messaggio: REGISTER identifica una connessione
  persistente Storage; PUT, GET o LIST identificano una connessione
  Client.
- Sulla connessione persistente il Coordinator non invia mai messaggi
  di propria iniziativa: risponde solo a REGISTER (con REGISTERED) e a
  STORED (con ACK).
- Se la porta non e' un intero in $[1, 65535]$: ERR INVALID_PORT e
  chiusura della connessione.
- Se il messaggio di registrazione e' malformato: ERR BAD_REQUEST e
  chiusura della connessione.
- Disconnessione: se il Coordinator rileva EOF o errore sulla
  connessione persistente di uno Storage, rimuove lo Storage dall'elenco
  degli Storage attivi e rimuove dall'indice tutti i file associati a
  quello Storage. Se lo Storage rileva la chiusura della connessione con
  il Coordinator, termina con exit status diverso da 0.

Notifica di memorizzazione completata:
- Storage -> Coordinator: STORED <remote_name> <size>
- Coordinator -> Storage: ACK
- Inviato solo dopo che lo Storage ha ricevuto tutti i byte previsti,
  ha chiuso il file e il file finale e' disponibile nella directory di
  storage.
- Il Coordinator associa il file allo Storage della connessione
  persistente, aggiorna l'indice e risponde ACK.
- Un messaggio STORED malformato comporta ERR BAD_REQUEST e chiusura
  della connessione, con conseguente rimozione dello Storage e dei suoi
  file.


5. PROTOCOLLO CLIENT <-> COORDINATOR
----------------------------------------------------------------------

Per ogni comando put, get o list, il Client apre una connessione TCP
verso il Coordinator, invia una singola richiesta, riceve la risposta
e chiude la connessione.

PUT - Richiesta di memorizzazione:
- Client -> Coordinator: PUT <remote_name> <size>
- Coordinator -> Client: STORE <storage_id> 127.0.0.1 <storage_port>
- Il Coordinator seleziona, tra gli Storage attivi, quello con
  storage_id piu' piccolo.
- La risposta STORE NON prenota il nome: il file compare nell'indice
  solo dopo la notifica STORED dello Storage.
- Ordine tassativo dei controlli (risponde con il primo errore):
  1) Numero campi diverso da 3 -> ERR BAD_REQUEST
  2) Nome non valido -> ERR INVALID_NAME
  3) size non intera valida o $> 10\text{ MB}$ -> ERR INVALID_SIZE
  4) remote_name gia' presente nell'indice -> ERR FILE_EXISTS
  5) Nessuno Storage attivo presente -> ERR NO_STORAGE

GET - Individuazione di un file:
- Client -> Coordinator: GET <remote_name>
- Coordinator -> Client: FOUND <storage_id> 127.0.0.1 <storage_port> <size>
- Ordine dei controlli:
  1) Numero campi diverso da 2 -> ERR BAD_REQUEST
  2) Nome non valido -> ERR INVALID_NAME
  3) File assente nell'indice o Storage associato non attivo -> ERR NOT_FOUND

LIST - Elenco dei file disponibili:
- Client -> Coordinator: LIST
- Coordinator -> Client:
    FILES <n>
    FILE <remote_name_1> <size_1> <storage_id_1>
    ...
    FILE <remote_name_n> <size_n> <storage_id_n>
    END
- Il comando LIST non ammette argomenti (altrimenti ERR BAD_REQUEST).
- Le righe FILE devono essere ordinate in ordine crescente di
  remote_name confrontando byte per byte come strcmp() (lettere
  maiuscole precedono le minuscole).
- Se non esistono file, la risposta e' FILES 0 seguita da END.


6. PROTOCOLLO CLIENT <-> STORAGE (TRASFERIMENTO FILE)
----------------------------------------------------------------------

UPLOAD:
- Client -> Storage:      UPLOAD <remote_name> <size>
- Storage -> Client:      READY   (oppure ERR ... e chiusura)
- Client -> Storage:      <esattamente size byte di payload>
- Storage -> Coordinator: STORED <remote_name> <size>
- Coordinator -> Storage: ACK
- Storage -> Client:      STORED <remote_name> <size>

Regole UPLOAD:
- Prima di READY lo Storage verifica nell'ordine: numero campi
  (ERR BAD_REQUEST), nome (ERR INVALID_NAME), dimensione
  (ERR INVALID_SIZE), esistenza file o .part locale (ERR FILE_EXISTS).
- Scrittura su file temporaneo <storage_directory>/<remote_name>.part;
  solo dopo aver ricevuto e scritto esattamente size byte, rinomina in
  <storage_directory>/<remote_name>.
- Se size e' $0$: dopo READY non attende byte, crea il file vuoto e
  prosegue con STORED al Coordinator e STORED al Client.
- Se il Client chiude prima di inviare tutti i byte: lo Storage elimina
  il file .part e non notifica il Coordinator.
- Errore di filesystem -> ERR IO (se la connessione consente risposta).
- Se la notifica STORED al Coordinator fallisce (connessione chiusa o
  risposta diversa da ACK): lo Storage elimina il file finale e
  risponde ERR IO al Client.

DOWNLOAD:
- Client -> Storage: DOWNLOAD <remote_name>
- Storage -> Client: DATA <size>
- Storage -> Client: <esattamente size byte di payload>

Regole DOWNLOAD:
- Errori Storage: nome non valido -> ERR INVALID_NAME; file non
  presente nello Storage -> ERR NOT_FOUND.
- Il Client crea <local_file>.part e lo rinomina in <local_file> solo
  dopo aver ricevuto esattamente size byte (sovrascrivendo <local_file>
  se gia' esistente); in caso di disconnessione prematura elimina .part.
- Il Client verifica che size annunciata in DATA coincida con size
  ricevuta in FOUND; in caso contrario termina con errore.


7. MESSAGGI DI ERRORE OBBLIGATORI E PRECEDENZA
----------------------------------------------------------------------

Elenco messaggi di errore:
- ERR BAD_REQUEST  : comando sconosciuto, campi errati o sintassi invalida.
- ERR INVALID_NAME : remote_name non conforme ai vincoli.
- ERR INVALID_SIZE : dimensione non valida o superiore a $10\text{ MiB}$.
- ERR INVALID_PORT : porta non valida (non intero in $[1, 65535]$).
- ERR NO_STORAGE   : nessuno Storage attivo disponibile per PUT.
- ERR FILE_EXISTS  : file gia' registrato o gia' presente nello Storage.
- ERR NOT_FOUND    : file non presente o non piu' disponibile.
- ERR IO           : errore locale apertura/lettura/scrittura/rinomina.

Precedenza in caso di errori multipli:
  ERR BAD_REQUEST -> ERR INVALID_NAME -> ERR INVALID_SIZE / ERR INVALID_PORT
  -> ERR FILE_EXISTS / ERR NOT_FOUND -> ERR NO_STORAGE

Chiusura dopo errore:
- Dopo un errore su Client <-> Coordinator o Client <-> Storage il
  ricevente puo' chiudere la connessione.
- Un errore di protocollo sulla connessione persistente Storage <->
  Coordinator comporta sempre la chiusura della connessione.


8. CONCORRENZA, SEGNALI E CAPACITA' MINIME
----------------------------------------------------------------------

- Concorrenza Coordinator: deve mantenere contemporaneamente le
  connessioni persistenti di piu' Storage e servire almeno $5$ Client
  contemporaneamente senza bloccarsi su Client lenti (anche 1 byte
  alla volta). Modello libero (select(), thread o processi) da motivare.
- Concorrenza Storage: deve mantenere aperta la connessione di controllo
  con il Coordinator mentre accetta Client. E' ammesso un server dati
  iterativo purche' resti registrato e non perda il controllo.
- Gestione SIGPIPE: ignorato con signal(SIGPIPE, SIG_IGN) o
  MSG_NOSIGNAL in send(), trattando EPIPE come disconnessione.
- Opzione socket: consigliato SO_REUSEADDR sui listening socket.
- Capacita' minime: strutture dati anche a dimensione fissa purche' il
  Coordinator gestisca almeno $16$ Storage registrati e $1024$ file
  nell'indice.


- si ha a disposizione 