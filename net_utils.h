struct conn_t{
    int fd;
    char buf[CHUNK_SIZE];
    size_t start;
    size_t end;
}

enum line_status_t{
    LINE_OK=1,
    LINE_INCOMPLETE=0,
    LINE_EOF=-1,
    LINE_OVERFLOW=-2,
    LINE_ERR=-3
}
//enum per comunicare gli esiti di estrazione di una riga
//OK: linea completa estratta fino a \n
//INCOMPLETA: fino ad ora nessuno \n di quelli ricevuti
//EOF: stato di errore in cui c'è stata una chiusura ordinata della connessione
//OVERFLOW: sono stati accumulati più di 512 byte senza un '\n'
//ERR: errore fatale del socket durante la lettura


//PARTE CONNESSIONE
void conn_init(conn_t*c,int fd);
//inizializza la struttura di stato della connessione
void conn_compact(conn_t*c);
//compatta la lettura se ci sono delle cose già lette in testa
int create_listening_socket(uint16_t port,int backlog);
//crea socket ed esegue tutte le operazioni per configurarlo()
int connect_to_loopback(uint16_t port);
//connessione parte client
int set_nonblocking(int fd);
//lettura flag e impostazione di non bloccante


//PARTE TRASMISSIONE
ssize_t send_all(int fd,const void*buf,size_t len);
//ci garantiamo l'invio di len byte tramite send()
ssize_t conn_fill(conn_t*c);
//
line_status_t conn_try_extract_line(conn_t*c,char*line_out,size_t max_len,size_t *out_len);
//scansionna i byte presenti nel buffer nell'intervallo tra start e end 
//cercando il carattere di terminazione \n senza chiamate di rete
line_status_t recv_line_blocking(conn_t *c,char *line_out,size_t max_len, size_t *out_len);
//riceviamo solo fino al prossimo \n

//STREAMING BINARIO
int send_file_stream(int sock_fd,int file_fd,uint64_t size);
//legge size byte dal file aperto e li invia sul socket senza interpretare il contenuto
int recv_file_stream(conn_t *c,int file_fd,uint64_t size);
//riceve esattamente size byte dal socket associato e li scrive nel file locale file_fd
