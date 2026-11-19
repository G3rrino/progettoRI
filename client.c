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
static int parse_found_line(char*line,uint16_t *out_port,uint64_t *out_size){

}
static int handle_get(uint16_t coord_port){

}

//gestione PUT
static int parse_store_line(char*line,uint16_t port){

}

static int handle_put(uint16_t coord_port,const char *local_path, const char *remote_name){

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
            rc=handle_get(args.coord_port);
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