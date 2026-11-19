#include "net_utils.h"
#include "protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

void conn_init(conn_t*c,int fd){
    //inizializziamo la struttua conn_t, chiamiamo 
    //la funzione dopo aver ottenuto un socket connesso
    //quindi solo dopo una connect/accept
    if(c==NULL) return;
    c->fd=fd;
    c->start=0;
    c->end=0;
}

void conn_compact(conn_t *c){
    if(c==NULL) return;
    //compattiamo il buffer togliendo i caratteri già consumati
    //effettuiamo una memmove verso l'inizio del buffer aggiornando 
    //gli indici
    if(c->end<=c->start){
        //se tutti i byte sono stati consumati
        c->start=0;
        c->end=0;
        return;
    }
    size_t numbyte=c->end-c->start;
    memmove(&c->buf[0],&c->buf[c->start],numbyte);
    c->start=0;
    c->end=numbyte;
}


int create_listening_socket(uint16_t port,int backlog){
    //creazione di un socket di ascolto, valore di ritorno il 
    //descrittore del socket, propaga -1 in caso di errore al chiamante
    //che deciderà cosa dovere fare

    if(backlog<=0){
        //al chiamante questo errore non è riconosciuto in errno,
        //si può impostare a mano
        errno=EINVAL;
        return -1;
    }
    int sd,ret;
    struct sockaddr_in my_addr;
    sd=socket(AF_INET,SOCK_STREAM,0);
    if(sd<0){
        perror("Errore nella creazione del socket");
        return -1;
    }
    //bisogna che io imposti il SO_REUSEADDR
    memset(&my_addr,0,sizeof(my_addr));
    my_addr.sin_family=AF_INET;
    my_addr.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&my_addr.sin_addr);
    ret=bind(sd,(struct sockaddr*)&my_addr,sizeof(my_addr));
    int saved_errno;
    if(ret<0){
        saved_errno=errno;
        perror("Errore nella bind");
        close(sd);
        errno=saved_errno;
        return -1;
    }
    ret=listen(sd,backlog);
    if(ret<0){
        saved_errno=errno;
        perror("Errore nella listen");
        close(sd);
        errno=saved_errno;
        return -1;
    }
    return sd;
}

int connect_to_loopback(uint16_t port){
    if(port==0){
        errno=EINVAL;
        return -1;
    }
    //creazione socket lato "client" e gestione della connect
    int sd,ret;
    struct sockaddr_in sv_addr;
    
    sd=socket(AF_INET,SOCK_STREAM,0);
    if(sd<0){
        perror("Errore nella creazione del socket");
        return -1;
    }
    memset(&sv_addr,0,sizeof(sv_addr));
    sv_addr.sin_family=AF_INET;
    sv_addr.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&sv_addr.sin_addr);
    ret=connect(sd,(struct sockaddr*)&sv_addr,sizeof(sv_addr));
    int saved_errno;
    if(ret<0){
        saved_errno=errno;
        perror("Errore nella connect");
        close(sd);
        errno=saved_errno;
        return -1;
    }
    return sd;
}

int set_nonblocking(int fd){
    //impostiamo il flag non bloccante di un socket, propaghiamo 
    //l'errore verso l'alto tramite errno
    if(fd<0){
        errno=EINVAL;
        return -1;
    }
    int flags=fcntl(fd,F_GETFL,0);
    if(flags==-1){
        return -1;
    }
    if(fcntl(fd,F_SETFL,flags | O_NONBLOCK)==-1){
        return -1;
    }
    return 0;
}

//da commentare
ssize_t send_all(int sd,const void*buf,size_t len){
    size_t sent=0;
    while(sent<len){
        ssize_t n=send(sd,(const char*)buf+sent,len-sent,0);
        if(n<0){
            if(errno==EINTR){
                continue; //interrotto il trasferimento da sigint
                //ignoriamo e procediamo
            }
            return -1; //altro segnale, c'è un errore e quindi 
            //lo propaghiamo
        }
        if(n==0){
            return -1;
        }
        sent+=(size_t)n;
    }
    return (ssize_t)sent;
}

line_status_t conn_try_extract_line(conn_t*c,char*line_out,size_t max_len,size_t *out_len){
    if(c==NULL || line_out==NULL || max_len==0 || out_len==NULL){
        return LINE_ERR;
    }
    size_t count=0;
    //leggiamo al massimo 512 byte di buffer non consumato e 
    //restituiamo un feedback di quello che ha ricevuto nel suo buffer
    for(size_t i=c->start;i<c->end && i<CHUNK_SIZE;++i){
        if(c->buf[i]=='\n'){
            memcpy(line_out,&c->buf[c->start],count);
            line_out[count]='\0';
            *out_len=count;
            c->start=i+1;
            return LINE_OK; //trovato un \n entro i 512, riga buona
        }
        count++;
        if(count>=max_len){
            return LINE_OVERFLOW; //superato i 512 senza \n
        }
    }
    return LINE_INCOMPLETE; //sotto i 512 disponibili ma senza un terminatore
}

ssize_t conn_fill(conn_t *c){
    if(c==NULL || c->fd<0){
        errno=EINVAL;
        return -1;
    }
    //libera in coda lo spazio delle righe già consumate
    conn_compact(c);
    //se il buffer è pieno bisogna distinguere il messaggio di errore
    //dal peer che si disconnette
    if(c->end>=CHUNK_SIZE){
        errno=ENOBUFS;
        return -1;
    }

    size_t avail=CHUNK_SIZE-c->end;
    //spazio ancora disponibile, lo dobbiamo andare a riempire
    while(1){
        ssize_t n=recv(c->fd,&c->buf[c->end],avail,0);
        if(n>0){
            c->end+=(size_t)n;
            return n;
        }
        if(n==0){
            return 0; //EOF; il peer ha chiuso la connessione
        }
        if(errno==EINTR){
            continue; //segnale di sigint, continuiamo 
        }
        return -1;
    }
}


//da commentare
line_status_t recv_line_blocking(conn_t *c, char *line_out, size_t max_len, size_t *out_len) {
    if (c == NULL || line_out == NULL || max_len == 0 || out_len == NULL) {
        return LINE_ERR;
    }

    while (1) {
        line_status_t lst = conn_try_extract_line(c, line_out, max_len, out_len);
        if (lst != LINE_INCOMPLETE) {
            return lst;
        }
        ssize_t ret = conn_fill(c);
        if (ret == 0) {
            return LINE_EOF;
        }
        if (ret < 0) {
            return LINE_ERR;
        }
    }
}


int send_file_stream(int sock_fd,int file_fd,uint64_t size){
    if(size==0 || sock_fd<0 || file_fd<0){
        errno=EINVAL;
        return 0;
    }
    char buf[CHUNK_SIZE];
    uint64_t remaining=size;
    while(remaining>0){
        size_t to_read=(remaining<CHUNK_SIZE)? (size_t)remaining:CHUNK_SIZE;
        ssize_t n=read(file_fd,buf,to_read);
        if(n<0){
            if(errno==EINTR){
                //interrotto ma continuo
                continue;
            }
            return -1;
        }
        if(n==0){
            //EOF prematuro, il file su disco contiene meno di size bite
            errno=EIO;
            return -1;
        }
        if(send_all(sock_fd,buf,(size_t)n)<0){
            return -1;
        }
        remaining-=(uint64_t)n;
    }
    return 0;
}


//sposta il buffer all'interno del file
//(commentare il secondo caso di errore)
static int write_all_fd(int fd,const void*buf,size_t len){
    size_t written=0;
    const char *ptr=(const char*)buf;
    while(written<len){
        ssize_t n=write(fd,ptr+written,len-written);
        if(n<0){
            if(errno=EINTR){
                continue;
                //ignoriamo i sigint
            }
            return -1;
        }
        if(n==0){
            errno=EIO;
            return -1;
        }
        written+=(size_t)n;
    }
    return 0;
}

int recv_file_stream(conn_t *c, int file_fd, uint64_t size) {
    if (c == NULL || c->fd < 0 || file_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t remaining = size;
    if (c->end > c->start && remaining > 0) {
        size_t buffered = c->end - c->start;
        size_t take = ((uint64_t)buffered < remaining) ? buffered : (size_t)remaining;

        if (write_all_fd(file_fd, &c->buf[c->start], take) < 0) {
            return -1;
        }

        c->start += take;
        remaining -= (uint64_t)take;

        if (c->start == c->end) {
            c->start = 0;
            c->end = 0;
        }
    }
    char buf[CHUNK_SIZE];

    while (remaining > 0) {
        size_t to_recv = (remaining < CHUNK_SIZE) ? (size_t)remaining : CHUNK_SIZE;
        ssize_t n = recv(c->fd, buf, to_recv, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }

        if (write_all_fd(file_fd, buf, (size_t)n) < 0) {
            return -1;
        }

        remaining -= (uint64_t)n;
    }

    return 0;
}