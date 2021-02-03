#ifndef SOCK_PROXY

#define SOCK_PROXY

#if 1

#define CLIENT_OFFSET 0
#define SERVER_OFFSET 1

#define READ_OFFSET  0
#define WRITE_OFFSET 1

int init_remote_socket(void);
int start_remote_socket_server(int port);
void end_remote_socket(void);
int remote_open(const char * hostname, int port, int * fd);
void remote_close(int remote_sd);
/*int remote_write(int remote_sd, const char * buff, int size);
int remote_read(int remote_sd, char * buff, int buff_size);
*/

size_t safe_remote_write (int fd, const void *buf, size_t count);
extern int write_remote_message (int fd, const char *fmt, ...);
size_t readremoteline (int fd, char **whole_buffer);

void remote_set_transparent(int fd);
void remote_set_nontransparent(int fd);

#else
#define safe_remote_write safe_write
#define write_remote_message write_message
#define readremoteline readline

#endif /* #ifdef REMOTE_SOCKET */

#endif /* #ifndef SOCK_PROXY */