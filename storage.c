#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net_utils.h"
#include "protocol.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Margine sufficiente a contenere "<storage_dir>/<remote_name>.part\0" senza troncamenti */
#define FULL_PATH_CAP (PATH_MAX + MAX_NAME_LEN + 16)
#define LISTEN_BACKLOG 64

/* Flag globale impostato dall'handler di SIGINT / SIGTERM per terminazione pulita */
static volatile sig_atomic_t g_stop = 0;
//facciamo in modo che sia volatile e che la cpu non se lo tenga in un registro 
//ma sia forzata sempre a leggerlo dalla ram, evita di fare ottimizzazioni sul ciclo

//atomic perchè voglio che lettura e scrittura avvenga in maniera atomica e senza creare delle inconsistenze

//HANDLER per sigint e sigterm
static void handle_term_signal(int sig) {
    (void)sig; //silenzia il warning -Wunused-parameter del compilatore
    //la firma chiede il segnale, noi non lo usiamo nel corpo
    g_stop = 1;
}

 //installazione dell'handler per SIGINT e SIGTERM 
 //preoccupiamoci di togliere SA_RESTART per non fare restartare chiamate bloccanti
 //come select() o accept() (vengono interrottre con EINTR) 
static int setup_signals(void) {
    //ignoriamo sigpipe
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("Errore configurazione SIGPIPE");
        return -1;
    }


    //costruiamoci una struttura sigaction
    /*
    struct sigaction{
        void(*sa_handler)(int); //funzione che fa da handler al segnale, SIG_DFL, SIG_IGN oppure un handler del tipo miohandler(int sig)
        void(*sa_sigaction)(int,sig_info_t*,void*); //handler esteso- non mi serve in questo caso
        sigset_t sa_mask; //altri segnali che sono bloccati mentre l'handler va 
        int sa_flags; //flags
        void(*sa_restorer)(void); //obsoleto/interno in linux
    }
    */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); //clear sulla struttura dati
    sa.sa_handler = handle_term_signal;
    sigemptyset(&sa.sa_mask); //svuoto il set dei segnali
    //le chiamate bloccanti abortiscono subito restituendo -1 con errno==EINTR, sbloccano il 
    //ciclo chiamante subito
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        //se fallisce uno dei due lo notifico
        perror("Errore configurazione sigaction su SIGINT/SIGTERM");
        return -1;
    }

    return 0;
}

//funzione di controllo se un nome ha .part come suffisso
static bool has_part_suffix(const char *name) {
    size_t len = strlen(name);
    return len >= 5 && strcmp(name + (len - 5), ".part") == 0;
}

//costruisce il percorso dir/name e il percorso dir/name.part senza che avvengano troncamenti
static int build_storage_paths(const char *dir, const char *name, char *final_buf, size_t final_cap, char *part_buf, size_t part_cap) {
    int n1 = snprintf(final_buf, final_cap, "%s/%s", dir, name);
    //controlliamo se la snprintf ha fallito oppure se ho superato il numero di caratteri disponibili
    if (n1 < 0 || (size_t)n1 >= final_cap) {
        return -1;
    }

    //costruisco il percorso part solo quando mi viene richiesto dal chiamante, metterò part_buf!=NULL  
    //e part_cap >0
    if (part_buf!=NULL && part_cap>0) {
        int n2=snprintf(part_buf, part_cap, "%s/%s.part", dir, name);
        //controlliamo se n2 ha fallito oppure se è andato oltre al numero disponibile di spazi
        if (n2 < 0 || (size_t)n2 >= part_cap) {
            return -1;
        }
    }

    return 0;
}



//funzione di supporto per controllare se lo storage dir esiste ed è una directory
//se assente non facciamo niente, altrimenti togliamo eventuali file .part residui
static int verify_and_clean_storage_dir(const char *storage_dir) {
    if(storage_dir==NULL) return -1;
    struct stat st;
    //La directory è già esistente ed è effettivamente una directory
    //usata funzione stat della libreria <sys/stat.h>
    //int stat(const char*restrict pathname,struct stat*restrict statbuf);
    //dove statbuf è una struct stat

    //struct stat{
    //...
    //mode_t st_mode;
    //...
    //}

    //uso S_ISDIR per controllare st_mode e controllare se è una directory
    if (stat(storage_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Errore: '%s' non esiste o non è una directory\n", storage_dir);
        return -1;
    }

    //DIR-> formato cartella
    //opendir-> apre una cartella passato il pathname e restituisce un DIR stream
    //dalla libreria dirent.h
    DIR *dp = opendir(storage_dir);
    if (dp == NULL) {
        perror("Errore apertura directory storage");
        return -1;
    }

    struct dirent *ent;
    //struttura dati puntatore agli elementi presenti dentro una cartella
    /*
    struct dirent{
        ino_t d_ino; //numero di i-node;
        off_t d_off; //non offset ma posizione nella lunghezza del record
        unsigned short d_reclen; //lunghezza del record
        char d_name[256]; //è il nome del file incontrato (file.txt.part ad esempio)
    }
    */
    char full_path[FULL_PATH_CAP];
    errno = 0;

    while ((ent = readdir(dp)) != NULL) { //nel caso buono restituisce un puntatore all'elemento successivo nella 
        //directory, altrimenti NULL e usciamo fuori dal ciclo
        //nella cartella abbiamo detto, teniamo contro solo di file.part 
        //altri file li ignoriamo
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        //eliminiamo solo file .part che sono rimasti da precedenti iterazioni interrotte
        if (has_part_suffix(ent->d_name)) {
            if (build_storage_paths(storage_dir, ent->d_name,full_path, sizeof(full_path), NULL, 0) < 0) {
                fprintf(stderr, "Errore: percorso file .part troppo lungo\n");
                closedir(dp);
                return -1;
            }
            if (unlink(full_path) < 0) {
                perror("Avviso: impossibile eliminare file .part residuo");
            }
        }
        //siccome abbiamo che readdir imposta NULL sia in caso di errore che di fine sarebbe buono 
        //impostare ad ogni giro errno per capirci meglio
        errno = 0;
    }

    //distinguiamo cosa è successo da errno(FINE CARTELLA/errore)
    if (errno != 0) {
        perror("Errore scansione directory storage");
        closedir(dp);
        return -1;
    }

    //chiudiamo il descrittore di cartella, senza liberare memoria
    closedir(dp);
    return 0;
}

//apriamo la connessione persistente verso il coordinator, inviamo il messaggio preliminare di register e 
 //attendiamo la risposta di registered, facciamo ritornare il file descriptor
static int register_with_coordinator(uint16_t coord_port, uint16_t storage_port,conn_t *out_coord_conn, uint64_t *out_storage_id) {
    if(coord_port==0 || storage_port==0 || out_coord_conn==NULL || out_storage_dir==NULL){
        return -1;
    }
    int fd = connect_to_loopback(coord_port);
    if (fd < 0) {
        perror("Errore connessione al coordinator");
        return -1;
    }

    conn_init(out_coord_conn, fd);

    char line[MAX_LINE_LEN];
    int n = snprintf(line, sizeof(line), "REGISTER %u\n", (unsigned int)storage_port);
    //inviamo al coordinator la frase register più la porta
    //gestiamo caso di errore dell'invio o di copia nel buffer
    if (n < 0 || (size_t)n >= sizeof(line) || send_all(fd, line, (size_t)n) < 0) {
        fprintf(stderr, "Errore invio comando REGISTER al coordinator\n");
        close(fd);
        return -1;
    }

    size_t len = 0;
    //attendiamo la risposta registered da parte del coordinator
    //se la funzione fallisce
    if (recv_line_blocking(out_coord_conn, line, sizeof(line), &len) != LINE_OK) {
        fprintf(stderr, "Errore ricezione risposta REGISTER dal coordinator\n");
        close(fd);
        return -1;
    }


    //se il coordinator risputa indietro un errore
    if (strncmp(line, "ERR ", 4) == 0 && strlen(line) == len) {
        fprintf(stderr, "Registrazione rifiutata dal coordinator: %s\n", line);
        close(fd);
        return -1;
    }

    parsed_line_t parsed;
    uint64_t assigned_id = 0;
    //se non è error controlla la risposta del coordinator con il parser, controlli di formato REGISTERED <storage_id>
    if (parse_control_line(line, (int)len, &parsed) < 0 || parsed.token_count != 2 || strcmp(parsed.tokens[0], "REGISTERED") != 0 || parse_strict_uint64(parsed.tokens[1], &assigned_id) < 0 || assigned_id == 0) {
        fprintf(stderr, "Risposta REGISTERED non valida dal coordinator\n");
        close(fd);
        return -1;
    }

    *out_storage_id = assigned_id; //assegniamo l'id 
    return fd;
}

 //funzione di handler per gestire il dopo put di un client
 //inviamo una notifica STORED <remote_name> <size>
 //ci aspettiamo semplicemente ACK
static int notify_coordinator_stored(conn_t *coord_conn, const char *remote_name, uint64_t size, bool *out_coord_lost) {
    if(coord_conn==NULL || remote_name==NULL || size==0){
        return -1;
    }
    char line[MAX_LINE_LEN];
    int n = snprintf(line, sizeof(line), "STORED %s %llu\n", remote_name, (unsigned long long)size);
    //verifichiamo che il messaggio non superi i 512 byte
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return -1;
    }

    //inviamo la notifica STORED al coordinator
    if (send_all(coord_conn->fd, line, (size_t)n) < 0) {
        *out_coord_lost = true;
        return -1;
    }

    size_t len = 0;
    line_status_t lst = recv_line_blocking(coord_conn, line, sizeof(line), &len);
    //aspettiamo la ricezione della frase (dovrebbe essere ACK)
    //linea non OK
    if (lst != LINE_OK) {
        *out_coord_lost = true;
        return -1;
    }

    //mi ha risposto con err, lo mostro
    if (strncmp(line, "ERR ", 4) == 0 && strlen(line) == len) {
        fprintf(stderr, "Errore dal coordinator su STORED: %s\n", line);
        return -1;
    }

    parsed_line_t parsed;
    //Linea ricevuta: controllo che sia semplicemente ACK
    if (parse_control_line(line, (int)len, &parsed) < 0 || parsed.token_count != 1 ||  strcmp(parsed.tokens[0], "ACK") != 0) {
        *out_coord_lost = true;
        return -1;
    }

    return 0;
}

/*
 * Gestisce il comando "UPLOAD <remote_name> <size>" di un Client rispettando
 * l'ordine dei controlli della specifica:
 * 1) numero campi (già verificato dal chiamante), 2) nome, 3) size, 4) esistenza file/.part.
 * Parametri: storage_dir, coord_conn, client_conn, parsed (riga UPLOAD già tokenizzata),
 *            out_coord_lost (true se il Coordinator ha chiuso la connessione durante STORED).
 */

//handler per gestire il comando UPLOAD del CLIENT
//invia UPLOAD <remote_name> <size> 
static void handle_upload(const char *storage_dir, conn_t *coord_conn, conn_t *client_conn, const parsed_line_t *parsed, bool *out_coord_lost) {
    int client_sd = client_conn->fd;

    //come da specifica controlliamo cmq quello che ci invia il client, non possiamo fidarci, nel caso in cui 
    //sia errato propaghiamo l'errore
    if (parsed->token_count != 3) {
        send_err_line(client_sd, ERR_BAD_REQUEST);
        return;
    }

    const char *remote_name = parsed->tokens[1];
    const char *size_tok = parsed->tokens[2];

    //controlliamo la validità del remote_name
    if (validate_remote_name(remote_name) < 0) {
        send_err_line(client_sd, ERR_INVALID_NAME);
        return;
    }

    uint64_t file_size = 0;
    //controlliamo la dimensione del file, deve rispettare il fatto di essere sotto i 10MB
    if (parse_strict_uint64(size_tok, &file_size) < 0 || file_size > MAX_FILE_SIZE) {
        send_err_line(client_sd, ERR_INVALID_SIZE);
        return;
    }

    char final_path[FULL_PATH_CAP];
    char part_path[FULL_PATH_CAP];
    //costruiamo i percorsi nella directory, specifichiamo che deve essere un file.part(mandando true nel campo giusto)
    if (build_storage_paths(storage_dir, remote_name,final_path, sizeof(final_path),part_path, sizeof(part_path)) < 0) {
        send_err_line(client_sd, ERR_IO);
        return;
    }

    struct stat st;
    //controlliamo se il file esiste già, usiamo le funzioni di dirent.h
    if (stat(final_path, &st) == 0 || stat(part_path, &st) == 0) {
        send_err_line(client_sd, ERR_FILE_EXISTS);
        return;
    }

    //apriamo il file mettendo tutti i permessi possibili e immaginabili su noi stessi
    int part_fd = open(part_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (part_fd < 0) {
        //questo controllo mi è ignoto
        send_err_line(client_sd, (errno == EEXIST) ? ERR_FILE_EXISTS : ERR_IO);
        return;
    }

    //inviamo READY al client, per dirgli che ora è possibile cominciare a spedire il file
    if (send_all(client_sd, "READY\n", 6) < 0) {
        //se il messaggio non arriva, chiudi il socket e fai l'unlink del percorso creato
        close(part_fd);
        unlink(part_path);
        //decrementa il numero di hard_link associati a quel file, siccome ne abbiamo creato solo uno noi
        //il file smette di esistere (invece no)
        return;
    }

    //aspettiamo di ricerverci esattamente il numero di byte, se 0 non faccio nulla
    if (recv_file_stream(client_conn, part_fd, file_size) < 0) {
        //qui gestiamo il caso di disconnessione prematura 
        //chiudiamo il socket 
        close(part_fd);
        unlink(part_path);
        return;
    }

    //chiudiamo il file temporaneo
    if (close(part_fd) < 0) {
        perror("Errore chiusura file .part");
        unlink(part_path);
        send_err_line(client_sd, ERR_IO);
        return;
    }

    //rinominiamo in remote_name effettivo
    if (rename(part_path, final_path) < 0) {
        perror("Errore rename file temporaneo");
        unlink(part_path);
        send_err_line(client_sd, ERR_IO);
        return;
    }

    //notifichiamo al coordinator con STORED <remote_name> <size>
    if (notify_coordinator_stored(coord_conn, remote_name, file_size, out_coord_lost) < 0) {
        //caso in cui la notifica fallisce, eliminiamo il file e spediamo errore io al client
        unlink(final_path);
        send_err_line(client_sd, ERR_IO);
        return;
    }

    char resp[MAX_LINE_LEN];
    int n = snprintf(resp, sizeof(resp), "STORED %s %llu\n",remote_name, (unsigned long long)file_size);
    // invia STORED <remote_name> <size>
    if (n > 0 && (size_t)n < sizeof(resp)) {
        (void)send_all(client_sd, resp, (size_t)n);
    }
}


//comando per gestire il DOWNLOAD da parte del client, devo verificare prima tutti quanti i parametri (comando passato, esistenza file, poi invio il DATA <size>)
static void handle_download(const char *storage_dir, conn_t *client_conn, const parsed_line_t *parsed) {
    if(storage_dir==NULL || client_conn==NULL || parsed==NULL){
        return -1;
    }
    int client_sd = client_conn->fd;

    
    if (parsed->token_count != 2) {
        send_err_line(client_sd, ERR_BAD_REQUEST);
        return;
    }

    const char *remote_name = parsed->tokens[1];

    //controllo validità del remote_name
    if (validate_remote_name(remote_name) < 0) {
        send_err_line(client_sd, ERR_INVALID_NAME);
        return;
    }

    char final_path[FULL_PATH_CAP];
    //costruisce tutto il percorso completo per il file
    if (build_storage_paths(storage_dir, remote_name, final_path, sizeof(final_path), NULL, 0) < 0) {
        send_err_line(client_sd, ERR_IO);
        return;
    }

    //apre il file definitivo dal path creato come read only
    //se fallisce il descrittore quindi il file non esiste oppure errori di pipe
    int file_fd = open(final_path, O_RDONLY);
    if (file_fd < 0) {
        send_err_line(client_sd, (errno == ENOENT) ? ERR_NOT_FOUND : ERR_IO);
        return;
    }

    struct stat st;
    //verifica che il file sia regolare e se le dimensioni sono regolari 
    if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file_fd);
        send_err_line(client_sd, ERR_IO);
        return;
    }

    uint64_t file_size = (uint64_t)st.st_size;

    char hdr[MAX_LINE_LEN];
    int n = snprintf(hdr, sizeof(hdr), "DATA %llu\n", (unsigned long long)file_size);
    //invio il messaggio DATA <size> al client
    //procedo poi a inviare lo stream dei file 
    if (n > 0 && (size_t)n < sizeof(hdr) && send_all(client_sd, hdr, (size_t)n) == 0) {
        (void)send_file_stream(client_sd, file_fd, file_size);
        //non possiamo interrompere lo stream inviando un errore in quanto se il client si disconnettesse
        //corromperemmo il payload binario quindi boh è inutile
    }

    //chiudiamo il descrittore del file
    close(file_fd);
}

//gestisce una singola connessione con il client, aspettiamo il comando anche
static void serve_client_connection(const char *storage_dir, conn_t *coord_conn, int client_sd, bool *out_coord_lost) {
    if(storage_dir==NULL || client_sd==0 || out_coord_lost==NULL){
        return;
    }
    conn_t client_conn;
    conn_init(&client_conn, client_sd);

    char line[MAX_LINE_LEN];
    size_t len = 0;
    line_status_t st = recv_line_blocking(&client_conn, line, sizeof(line), &len);

    //se la linea supera più di 512 byte senza un \n mandiamo un err di BAD_REQUEST
    if (st == LINE_OVERFLOW) {
        send_err_line(client_sd, ERR_BAD_REQUEST);
        return;
    }

    //se il client chiude prima o si verifica un errore sul socket
    if (st != LINE_OK) {
        return;
    }

    parsed_line_t parsed;
    //controlliamo la linea di comando ricevuta, chiamiamo la funzione di parsing linea di controllo
    //se anche solo qualche test non passa e torna meno di 0 oppure ha un solo token segnaliamo 
    //l'errore di BAD_REQUEST
    if (parse_control_line(line, (int)len, &parsed) < 0 || parsed.token_count < 1) {
        send_err_line(client_sd, ERR_BAD_REQUEST);
        return;
    }

    //controlliamo il comando, 
    //se DOWNLOAD andiamo in handle_download
    //se UPLOAD andiamo in handle_upload
    //se nessuno dei due andiamo in mandiamo ERR_REQUEST a client
    if (strcmp(parsed.tokens[0], "UPLOAD") == 0) {
        handle_upload(storage_dir, coord_conn, &client_conn, &parsed, out_coord_lost);
    } else if (strcmp(parsed.tokens[0], "DOWNLOAD") == 0) {
        handle_download(storage_dir, &client_conn, &parsed);
    } else {
        send_err_line(client_sd, ERR_BAD_REQUEST);
    }
}

 //validatore di argomenti passati a linea di comando 
static int parse_sto_args(int argc, char **argv,uint16_t *out_coord_port,uint16_t *out_storage_port,const char **out_storage_dir) {
    //verifica se sono stati passati esattamente 3 argomenti
    if (argc != 4 || argv == NULL) {
        return -1;
    }

    //validazione della porta tramite funzione in net_utils
    if (parse_strict_port(argv[1], out_coord_port) < 0) {
        fprintf(stderr, "Errore: coordinator_port non valida ('%s')\n", argv[1]);
        return -1;
    }

    //validazione porta dello storage
    if (parse_strict_port(argv[2], out_storage_port) < 0) {
        fprintf(stderr, "Errore: storage_port non valida ('%s')\n", argv[2]);
        return -1;
    }

    //verifica lunghezza dello storage indicato
    if (argv[3][0] == '\0' || strlen(argv[3]) >= PATH_MAX) {
        fprintf(stderr, "Errore: storage_directory non valida\n");
        return -1;
    }

    *out_storage_dir = argv[3];
    return 0;
}

int main(int argc, char **argv) {
    //configuriamo i segnali, funzione di utilità 
    if (setup_signals() < 0) {
        exit(EXIT_FAILURE);
    }

    uint16_t coord_port = 0;
    uint16_t storage_port = 0;
    const char *storage_dir = NULL;

    //validazione dei parametri a linea di comando
    if (parse_sto_args(argc, argv, &coord_port, &storage_port, &storage_dir) < 0) {
        fprintf(stderr, "Uso: %s <coordinator_port> <storage_port> <storage_directory>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    //controllo e pulizia della directory, si rimanda a commento sopra
    if (verify_and_clean_storage_dir(storage_dir) < 0) {
        exit(EXIT_FAILURE);
    }

    //creiamo il socket di ascolto prima di registrarsi
    int listen_fd = create_listening_socket(storage_port, LISTEN_BACKLOG);
    if (listen_fd < 0) {
        perror("Errore creazione socket di ascolto storage");
        exit(EXIT_FAILURE);
    }

    //apriamo una connessione verso il coordinator e inviamo il register
    conn_t coord_conn;
    uint64_t storage_id = 0;
    int coord_fd = register_with_coordinator(coord_port, storage_port, &coord_conn, &storage_id);
    if (coord_fd < 0) {
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    int exit_code = EXIT_SUCCESS;

     //loop di controllo, sulla variabile atomica g_stop controllata dai segnali
     //leggiamo i due set di socket fra quelli coordinatore e quelli dei client
     //fuori da un register con il coordinator ci sono solo pochi messaggi di stored altrimenti sono tutti errori
    while (!g_stop) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        FD_SET(coord_fd, &readfds);

        int max_fd = (listen_fd > coord_fd) ? listen_fd : coord_fd;
        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if (ready < 0) {
            //se qui veniamo interrotti terminaniamo solo uscendo dal ciclo per mantenere intatto lo stato e non creare 
            //delle inconsistenze
            if (errno == EINTR) {
                continue;
            }
            perror("Errore select nello storage");
            exit_code = EXIT_FAILURE;
            break;
        }

        //rileva chiusura improvvisa del coordinator
        if (FD_ISSET(coord_fd, &readfds)) {
            fprintf(stderr, "Connessione con il coordinator chiusa inaspettatamente\n");
            exit_code = EXIT_FAILURE;
            break;
        }

        //accetta e serve la connessione con un client (DOWNLOAD, UPLOAD)
        if (FD_ISSET(listen_fd, &readfds)) {
            int client_sd = accept(listen_fd, NULL, NULL);
            if (client_sd < 0) {
                if (errno == EINTR) {
                    //se sono interrotto continuo, finisco il giro al loop
                    continue;
                }
                perror("Errore accept client");
                continue;
            }

            bool coord_lost = false;
            serve_client_connection(storage_dir, &coord_conn, client_sd, &coord_lost);
            close(client_sd);

            //controllo lo stato del coordinator rispetto a me, stampo errori in base a quello 
            if (coord_lost) {
                fprintf(stderr, "Connessione con il coordinator persa durante STORED\n");
                exit_code = EXIT_FAILURE;
                break;
            }
        }
    }

    //chiudo tutti i descrittori aperti
    close(listen_fd);
    close(coord_fd);
    return exit_code;
}
