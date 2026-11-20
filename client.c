#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "net_utils.h"
#include "protocol.h"

#define PATH_MAX 4096

typedef enum {
    CLIENT_CMD_LIST = 1,
    CLIENT_CMD_PUT,
    CLIENT_CMD_GET
} client_cmd_t;

typedef struct {
    uint16_t coord_port;
    client_cmd_t cmd;
    const char *local_path;   //Usato solo in PUT e GET 
    const char *remote_name;  // Usato solo in PUT e GET 
} client_args_t;

static int parse_cli_args(int argc, char **argv, client_args_t *out) {
    //controlli preliminari su quello che viene passato
    if (argc < 3 || argv == NULL || out == NULL) {
        return -1;
    }

    //puliamo la struttura dati del client
    memset(out, 0, sizeof(*out));

    if (parse_strict_port(argv[1], &out->coord_port) < 0) {
        fprintf(stderr, "Errore: porta del coordinator non valida ('%s')\n", argv[1]);
        return -1;
    }

    //controllo se è LIST, seè list gli argomenti devo essere 3
    // ./client <coord_port> LIST
    if (strcmp(argv[2], "LIST") == 0) {
        if (argc != 3) {
            return -1;
        }
        out->cmd = CLIENT_CMD_LIST;
        return 0;
    }

    //controllo se è PUT, gli argomenti devono essere 5
    // ./client <coord_port> PUT <local_path> <remote_name> 
    if (strcmp(argv[2], "PUT") == 0) {
        if (argc != 5 || argv[3][0] == '\0' || argv[4][0] == '\0') {
            return -1;
        }
        if (validate_remote_name(argv[4]) < 0) {
            fprintf(stderr, "Errore: remote_name non valido ('%s')\n", argv[4]);
            return -1;
        }
        out->cmd = CLIENT_CMD_PUT;
        out->local_path = argv[3];
        out->remote_name = argv[4];
        return 0;
    }

    //controllo se è GET, gli argomento devono essere anche qui 5
    //./client <coord_port> GET <remote_name> <local_path> 
    if (strcmp(argv[2], "GET") == 0) {
        if (argc != 5 || argv[3][0] == '\0' || argv[4][0] == '\0') {
            return -1;
        }
        if (validate_remote_name(argv[3]) < 0) {
            fprintf(stderr, "Errore: remote_name non valido ('%s')\n", argv[3]);
            return -1;
        }
        out->cmd = CLIENT_CMD_GET;
        out->remote_name = argv[3];
        out->local_path = argv[4];
        return 0;
    }

    return -1;
}

//la uso dentro handle_put, gestisco la fase di put solo partendo preliminarmente da 
//questo , il file deve esistere, essere leggibile e regolare
static int open_local_for_upload(const char*path,uint64_t*out_size){
    if(path==NULL || out_size==NULL){
        errno=EINVAL;
        return -1;
    }
    int fd=open(path,O_RDONLY);
    if(fd<0){
        perror("Errore nell'apertura del file locale");
        return -1;
    }
    struct stat statbuf;
    memset(&statbuf,0,sizeof(statbuf));
    if(fstat(fd,&statbuf)==-1){
        perro("Errore nella fstat del file, file corrotto");
        close(fd);
        return -1;
    }
    //piccola indecisione con il modello
    //la specifica mi dice: non dovrei preoccuparmi del tipo di dato 
    //trasmesso più di tanto, la maggior parte sarà un .txt, il modello
    //mi suggerisce di controllare che non si tratti di una cartella o 
    //di un qualcosa che sia più leggibile in modo tale da evitare 
    //incosistenze/problemi/ errori che non gestiamo di norma
    //dice per esempio /cartella/sottocartella (controllo sul local_path)
    //non sul remote name
    if(!S_ISREG(statbuf.st_mode)){
        fprintf(stderr,"Errore: il file è non regolare");
        close(fd);
        return -1;
    }

    //controllo se la grandezza del file sia ok
    if(statbuf.st_size<0){
        fprintf(stderr,"File vuoto\n");
        close(fd);
        return -1;
    }
    *out_size=(uint64_t)statbuf.st_size;
    return fd;
}

static int build_part_path(const char*dest_path,char*part_buf,size_t cap){
    //questo è l'handler di get per ricavare il nome del file temporaneo
    //su cui scaricare i byte prima di fare rename() al file definitivo
    if(dest_path==NULL || part_buf==NULL || cap==0){
        return -1;
    }
    int n=snprintf(part_buf,cap,"%s.part",dest_path);
    if(n<0 || (size_t)n>=cap){
        return -1;
    }
    return 0;
}


//procediamo ora con le funzioni ausiliarie di gestione del client 
//per ogni tipologia di comando che è stato inserito 

//gestione LIST
static int handle_list(uint16_t coord_port) {
    int fd = connect_to_loopback(coord_port);
    if (fd < 0) {
        return -1;
    }

    conn_t conn;
    conn_init(&conn, fd);

    if (send_all(fd, "LIST\n", 5) < 0) {
        perror("Errore nell'invio del messaggio LIST");
        close(fd);
        return -1;
    }

    char line[MAX_LINE_LEN];
    size_t len = 0;
    parsed_line_t parsed;

    /* 1. Lettura intestazione: "FILES <N>" oppure "ERR ..." */
    if (recv_line_blocking(&conn, line, sizeof(line), &len) != LINE_OK) {
        fprintf(stderr, "Errore nella ricezione della risposta del coordinator\n");
        close(fd);
        return -1;
    }

    /* Controlliamo "ERR " prima di parse_control_line, perché questa spezza line con '\0' */
    if (strncmp(line, "ERR ", 4) == 0 && strlen(line) == len) {
        fprintf(stderr, "%s\n", line);
        close(fd);
        return -1;
    }

    uint64_t count = 0;
    if (parse_control_line(line, len, &parsed) < 0 ||
        parsed.token_count != 2 ||
        strcmp(parsed.tokens[0], "FILES") != 0 || parse_strict_uint64(parsed.tokens[1], &count) < 0) {
        fprintf(stderr, "Risposta del protocollo non valida dal coordinator\n");
        close(fd);
        return -1;
    }

    /* 2. Lettura e stampa delle N righe "FILE <name> <size> <storage_id>" */
    for (uint64_t i = 0; i < count; ++i) {
        if (recv_line_blocking(&conn, line, sizeof(line), &len) != LINE_OK) {
            fprintf(stderr, "Errore di connessione interrotta durante la lettura di elenco file\n");
            close(fd);
            return -1;
        }

        uint64_t file_size = 0;
        if (parse_control_line(line, len, &parsed) < 0 || parsed.token_count != 4 || strcmp(parsed.tokens[0], "FILE") != 0 || validate_remote_name(parsed.tokens[1]) < 0 || parse_strict_uint64(parsed.tokens[2], &file_size) < 0 || parsed.tokens[3][0] == '\0') {
            fprintf(stderr, "Riga inattesa o malformata dal coordinator\n");
            close(fd);
            return -1;
        }

        printf("%s %s %llu %s\n",parsed.tokens[0],parsed.tokens[1],(unsigned long long)file_size,parsed.tokens[3]);

    }

    /* 3. Verifica chiusura elenco con "END" */
    if (recv_line_blocking(&conn, line, sizeof(line), &len) != LINE_OK) {
        fprintf(stderr, "Errore di chiusura messaggio da parte del coordinator\n");
        close(fd);
        return -1;
    }

    if (parse_control_line(line, len, &parsed) < 0 || parsed.token_count != 1 || strcmp(parsed.tokens[0], "END") != 0) {
        fprintf(stderr, "Errore di protocollo del coordinator sulla chiusura del messaggio LIST\n");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}



//gestione GET
static int parse_found_line(char*line,size_t raw_len,uint16_t *out_port,uint64_t *out_size){
    //dobbiamo  qui scomporre e validare la risposta positiva del COORDINATOR
    if(line==NULL || raw_len==0 || out_port==NULL || out_size==NULL){
        return -1;
    }
    parsed_line_t parsed;
    if(parse_control_line(line,raw_len,&parsed)!=0){
        return -1;
    }
    if(parsed.token_count!=5){
        return -1;
    }

    //dobbiamo andare a validare i singoli token ora, non ci preoccupiamo se possono essere vuoti
    //il controllo esiste già a monte
    //1. [0] deve essere FOUND
    //2. [1] deve essere storage_id, qui mi devo preoccupare che sia non vuoto ma il controllo 
    // è garantito a monte
    //3. [2] deve essere IP 
    //4. [3] deve essere coord_port e fa passato al convertitore e verificatore della porta
    // parse_strict_port
    //5. [4] dimensione, va passato al convertitore di tipo uint64_t
    if(strcmp(parsed.tokens[0],"FOUND")!=0 || strcmp(parsed.tokens[2],"127.0.0.1")!=0 || parse_strict_port(parsed.tokens[3],out_port)!=0 || parse_strict_uint64(parsed.tokens[4],out_size)!=0){
        return -1;
    }

    return 0;
}

static int handle_get(uint16_t coord_port, const char*remote_name, const char*local_path){
    //prepariamo localmente il file local
    char part_path[PATH_MAX];
    if(build_part_path(local_path,part_path,sizeof(part_path))==-1){
        fprintf(stderr,"Errore di percorso, eccede la capacità del buffer");
        return -1;
    }

    int sd=connect_to_loopback(coord_port);
    if(sd<0){
        perror("Errore di connessione al coordinator");
        return -1;
    }
    conn_t ct;
    conn_init(&ct,sd);

    char line[MAX_LINE_LEN];
    int n=snprintf(line,sizeof(line),"GET %s\n",remote_name);
    if(n<0 || (size_t)n>=sizeof(line)){
        fprintf(stderr,"Errore: comando GET troppo lungo\n");
        close(sd);
        return -1;
    }
    
    if(send_all(sd,line,(size_t)n)<0){
        perror("Errore di invio comando GET al coordinator");
        close(sd);
        return -1;
    }

    size_t len=0;
    line_status_t lst=recv_line_blocking(&ct,line,sizeof(line),&len);
    close(sd); //non ci serve più intrattenere una connessione con il coordinator

    if(lst!=LINE_OK){
        fprintf(stderr,"Errore ricezione risposta dal coordinator\n");
        return -1;
    }

    //controlliamo ERR saturo prima di fare un parsing del control line
    if(strncmp(line,"ERR ",4)==0 && strlen(line) == len){
        fprintf(stderr,"%s\n",line);
        return -1;
    }

    uint16_t st_port=0;
    uint64_t expected_size=0;
    if(parse_found_line(line,len,&st_port,&expected_size)<0){
        fprintf(stderr,"Risposta FOUND non valida dal coordinator\n");
        return -1;
    }

    //fase di richiesta di download allo storage
    int st_sd=connect_to_loopback(st_port);
    if(st_sd<0){
        perror("Errore di connessione allo storage");
        return -1;
    }

    conn_t st_conn;
    conn_init(&st_conn,st_sd);

    n=snprintf(line,sizeof(line),"DOWNLOAD %s\n",remote_name);
    if(n<0 || (size_t)n>=sizeof(line)){
        fprintf(stderr,"Errore: comando DOWNLOAD troppo lungo\n");
        close(st_sd);
        return -1;
    }

    if(send_all(st_sd,line,(size_t)n)<0){
        perror("Errore di invio comando DOWNLOAD allo storage");
        close(st_sd);
        return -1;
    }

    lst=recv_line_blocking(&st_conn,line,sizeof(line),&len);
        if(lst!=LINE_OK){
            fprintf(stderr,"Errore di ricezione intestazione DATA dallo storage");
            close(st_sd);
            return -1;
        }
    

    //controlliamo se mi mando un errore e lo riporto in std err
    if(strncmp(line,"ERR ",4)==0 && strlen(line)==len){
        fprintf(stderr,"%s\n",line);
        close(st_sd);
        return -1;
    }

    //altrimenti mi risponde con DATA <size> e controlliamo se la size è uguale
    parsed_line_t parsed;
    uint64_t data_size=0;
    if(parse_control_line(line,len,&parsed)<0 || parsed.token_count!=2 || strcmp(parsed.tokens[0],"DATA")!=0 || parse_strict_uint64(parsed.tokens[1],&data_size)<0){
        fprintf(stderr,"Intestazione DATA non valida da parte dello storage");
        close(st_sd);
        return -1;
    }

    if(data_size!=expected_size){
        fprintf(stderr,"Errore: dimensione incoerente tra coordinator e storage dimension");
        close(st_sd);
        return -1;
    }


    //facciamo ricezione su un file temporaneo .part e quando finiscono 
    //tutte le modifiche facciamo il rename del file

    int part_fd=open(part_path,O_WRONLY | O_CREAT | O_TRUNC, 0644); //da capire
    // O_WRONLY-> ci scrive solo
    // O_CREAT-> se non esiste lo crea lui
    // O_TRUNC -> se c'è n'è già uno lo azzera a 0 byte prima di scriverci dentro
    // 0644 -> permessi POSIX al file se viene creato da 0, altrimenti non è permessa la scrittura
    // specialmente se implementiamo O_CREAT
    if (part_fd<0){
        perror("Errore apertura file temporaneo .part");
        close(st_sd);
        return -1;
    }

    if(recv_file_stream(&st_conn,part_fd,data_size)<0){
        perror("Errore durante la ricezione del file dallo storage");
        close(part_fd);
        unlink(part_path);
        close(st_sd);
        return -1;
    }

    close(st_sd);
    if(close(part_fd)<0){
        perror("Errore chiusura file temporaneo .part");
        unlink(part_path);
        return -1;
    }

    if(rename(part_path,local_path)<0){
        perror("Errore rinomina file scaricato");
        unlink(part_path);
        return -1;
    }
    return 0;
}

//gestione PUT
static int parse_store_line(char*line,size_t raw_len,uint16_t* out_port){
    //devo verificare la riga STORE
    if(line==NULL || raw_len<=0 || out_port==NULL){
        return -1;
    }

    parsed_line_t plt;
    if(parse_control_line(line,raw_len,&plt)<0 || plt.token_count!=4){
        return -1;
    }

    //dobbiamo controllare il layout del messaggio
    //ci aspettiamo 
    //STORE <storage_id> <ip> <port>
    //bisognerà poi capire lato coordinator come comportarsi nel caso in cui non ci sono 
    //attualmente storage attivi o che tutti gli storage attualmente presenti hanno dimensione 
    //finita e terminata
    if(strcmp(plt.tokens[0],"STORE")!=0 || strcmp(plt.tokens[2],"127.0.0.1") || parse_strict_port(plt.tokens[3],out_port)<0){
        return -1;
    }

    return 0;
}

static int parse_stored_line(char *line,size_t raw_len,const char*expected_name, uint64_t expected_size){
    //devo verificare la riga STORED
    if(line==NULL || raw_len<=0 || expected_name==NULL){
        return -1;
    }

    parsed_line_t plt;
    if(parse_control_line(line,raw_len,&plt)<0 || parsed.token_count!=3){
        return -1;
    }

    //validiamo i singoli token, ci aspettiamo 
    //STORED <remote_name> <size>
    uint64_t stored_size;
    if(strcmp(plt.tokens[0],"STORED")!=0 || strcmp(plt.tokens[1],expected_name)!=0 || parse_strict_uint64t(plt.tokens[2],&stored_size)<0 || stored_size!=excpted_size){
        return -1;
    }
    return 0;
}

static int handle_put(uint16_t coord_port,const char *local_path, const char *remote_name){
    //bisogna coordinare la verifica locale del file, l'assegnazione dello storage
    //da parte del coordinator, trasferimento binario e conferma finale
    uint64_t file_size;
    int fd=open_local_for_upload(local_path,&file_size);
    if(fd<0){
        return -1;
    }
    //connettiamoci al coordinator e chiediamo l'indirizzo di uno storage in 
    //cui prendiamo e inviamo il dato
    int sd=connect_to_loopback(coord_port);
    if(sd<0){
        perror("Errore di connessione al coordinator");
        close(fd);
        return -1;
    }
    conn_t ct;
    conn_init(&ct,sd);

    char line[MAX_LINE_LEN];
    int n=snprintf(line,sizeof(line),"PUT %s %llu\n",remote_name,(unsigned long long)file_size);
    if(n<0 || (size_t)n>=sizeof(line)){
        fprintf(stderr,"Errore: comando PUT troppo lungo\n");
        close(st_sd);
        return -1;
    }

    if(send_all(sd,&line,(size_t)n)<0){
        perror("Errore nella send al coordinator");
        close(sd);
        return -1;
    }

    parsed_line_t plt;
    plt=recv_line_blocking(&ct,line,sizeof(line),&len);
    if(plt!=LINE_OK){
        fprintf(stderr,"");
        return -1;
    }




}




int main(int argc, char **argv){
    if(signal(SIGPIPE,SIG_IGN)==SIG_ERR){
        perror("Errore nella configurazione di SIGPIPE");
        exit(EXIT_FAILURE);
    }
    //ignoriamo i sigpipe
    client_args_t args;
    if(parse_cli_args(argc,argv,&args)<0){
        //qui il modello suggerisce di usare un handler che dice al client una volta per tutte come va usato 
        //il programmino (mi rifiuto di farlo)
        exit(EXIT_FAILURE);
    }

    int rc=-1;
    switch(args.cmd){
        case CLIENT_CMD_LIST:
            rc=handle_list(args.coord_port);
            break;
        case CLIENT_CMD_PUT:
            rc=handle_put(args.coord_port,args.local_path,args.remote_name);
            break;
        case CLIENT_CMD_GET:
            rc=handle_get(args.coord_port,args.remote_name,args.local_path);
            break;
        default:
            exit(EXIT_FAILURE);
    }
    if(rc<0){
        exit(EXIT_FAILURE);
    }else{
        return 0;
    }
}