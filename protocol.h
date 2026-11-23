//costanti di trasmissione
#define MAX_LINE_LEN 512
#define MAX_NAME_LEN 64
#define MAX_FILE_SIZE 10485760
#define CHUNK_SIZE 4096

//implementazione degli errori 

#define ERR_BAD_REQUEST "ERR BAD_REQUEST"
#define ERR_INVALID_NAME "ERR INVALID_NAME"
#define ERR_INVALID_SIZE "ERR INVALID_SIZE"
#define ERR_INVALID_PORT "ERR INVALID_PORT"
#define ERR_NO_STORAGE "ERR NO_STORAGE"
#define ERR_FILE_EXISTS "ERR FILE_EXISTS"
#define ERR_NOT_FOUND "ERR NOT_FOUND"
#define ERR_IO "ERR IO"


struct parsed_line_t{
    char*tokens[5];
    int token_count;
}
//funzioni di uso comune a tutti e 3 i moduli
int parse_control_line(char *line,int raw_len,parsed_line_t *out);
int validate_remote_name(const char*nome);
int parse_strict_uint64(const char*str,uint64_t*val);
int parse_strict_port(const char*str,uint16_t*port);
void send_err_line(int fd, const char *err_code);