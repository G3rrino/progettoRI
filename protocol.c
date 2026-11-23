#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int parse_control_line(char *line,size_t raw_len,parsed_line_t *out){
    //controlliamo la linea di comando e la scomponiamo in pezzi
    //qui la riga arriva già senza carattere di terminazione (\n) finale
    if(line==NULL|| out==NULL || raw_len==0){
        //controllo di null pointer 
        return -1;
    }
    //controllo se una riga si trova nello stato di LIST\0
    //in questo caso abbiamo che la lunghezza reale non corrisponde 
    //a quella dichiarata
    if(strlen(line)!=raw_len){
        return -1;
    }

    //controlliamo che non ci sia uno spazio iniziale 

    if(line[0]==' '){
        return -1;
    }
    out->tokens[0]=&line[0];
    out->token_count=1;
    for(size_t i=0;i<raw_len;++i){
        if(line[i]=='\r'){
            //ho trovato un carattere \r, stringa malformata
            return -1;
        }
        unsigned char c=(unsigned char)line[i];
        if(c<32 || c>=127){
            //carattere non testuale
            return -1;
        }

        if(line[i]==' '){
            //ho incontrato uno spazio, devo verificare quello che c'è dopo
            if(i+1>=raw_len || line[i+1]==' '){
                return -1;
            }
            if(out->token_count>=5){
                return -1;
            }
            out->tokens[out->token_count++]=&line[i+1];
            line[i]='\0';
        }
    }
    return 0;
}


int validate_remote_name(const char*nome){
    //funzione di validazione remote name di un file
    if(nome==NULL){
        return -1;
    }
    size_t lunghezza=0;
    while(nome[lunghezza]!='\0'){
        //mi segno il singolo carattere della mia stringa, me lo metto unsigned
        //perchè su linux i char sono straordinariamente signed
        unsigned char c=(unsigned char)nome[lunghezza];
        //validiamo intervallo possibile di appartenenza
        //[A-Za-z] [0-9]
        if(c>122){
            return -1;
        }else{
            int lettere_piccole=(c>='a' && c<='z');
            int lettere_grandi=(c>='A' && c<='Z');
            int numeri=(c<='9' && c>='0');
            int speciali=(c=='.' || c=='_' || c=='-');

            if(!lettere_piccole && !lettere_grandi && !numeri && !speciali){
                return -1;
            }
        }
        lunghezza++;
        //comando fin troppo lungo 
        if(lunghezza>MAX_NAME_LEN){
            return -1;
        }
    }

    //stringa cortissima
    if(lunghezza<1){
        return -1;
    }

    //facciamo il caso in cui io abbiamo digitato esclusivamente .
    if(lunghezza==1 && nome[0]=='.'){
        return -1;
    }

    //controlliamo se per caso esiste la sottostringa .. all'interno del remote name
    //uso la funzione strstr di string.h
    char*sottostringa="..";
    char *occorrenza=strstr(nome,sottostringa);
    //str torna il puntatore al pirmo carattere della prima occorrebza oppure 
    //NULL se non viene trovato
    if(occorrenza!=NULL){
        return -1;
    }
    //controllo sul divieto di file che finiscono con .part
    //bisogna supporre che ci sia una dimensione più grande di 5
    if(lunghezza>=5){
        const char*estensione=&nome[lunghezza-5];
        if(strcmp(estensione,".part")==0 || strcmp(estensione,".PART")==0){
            return -1;
        }
    }
    return 0;

}

int parse_strict_uint64(const char*str,uint64_t*val){
    //i semplici validatori non funzionano, abbiamo bisogno di controllare
    //che effettivamente le stringhe numeriche passate abbiano un senso 
    //lo faremo usare anche come sotto chiamata per quello della porta
    //stringa nulla
    if(str==NULL || val==NULL || str[0]=='\0'){
        return -1;
    }
    uint64_t acc=0;
    size_t i=0;
    //iteriamo su ogni carattere per trasformare tutto
    while(str[i]!='\0'){
        unsigned char c=(unsigned char)str[i];
        if('0'<=c && c<='9'){
            //calcoliamo il valore della signola cifra
            uint64_t digit=c-'0';
            if(acc<=(UINT64_MAX - digit)/10){
                acc=acc*10+digit;
            }else{
                //numero già troppo grosso
                return -1;
            }
        }else{
            //formato non rispettato (vogliamo solo numeri, non segni o spazi o caratteri speciali)
            return -1;
        }
        i++;
    }
    //tutto andato bene, il numero è buono e quindi aggiorno tutto quello
    //che devo aggiornare e restituisco 0
    *val=acc;
    return 0;
}

int parse_strict_port(const char*str,uint16_t*port){
    //controlliamo la correttezza della porta
    if(str==NULL || port==NULL){
        return -1;
    }

    uint64_t temp=0;
    //controlliamo che sia un formato valido e trasferiamolo a uint64 tramite 
    //parse_strict_uint64
    if(parse_strict_uint64(str,&temp)<0){
        return -1;
    }

    if(temp<1 || temp>65535){
     //massimo numero di porte superato
        return -1;
    }

    //la conversione funziona perchè ho verificato che la porta sta nel range 
    //dei possibili, 65535 è la massima porta che posso ottenere usando un
    //massimo numero su formato uint16_t
    *port=(uint16_t) temp;
    //porta buona, la rimetto nella uibt16 vuota passata dal chiamante
    return 0;

}


//funzione di invio di una riga di errore al client
static void send_err_line(int fd, const char *err_code) {
    char buf[MAX_LINE_LEN];
    int n = snprintf(buf, sizeof(buf), "%s\n", err_code);
    //controlliamo sempre che la riga di errore non superi i 512 byte stabiliti da protocollo
    if (n > 0 && (size_t)n < sizeof(buf)) {
        (void)send_all(fd, buf, (size_t)n);
    }
}