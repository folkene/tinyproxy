/*
 * #ifdef REMOTE_SOCKET
 * For some reason the COMPILER FLAG is not set :/
 */

#if 1

#include <pthread.h>
#include <stdint.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "sock_proxy.h"
#include "sock.h"
#include "log.h"
#include "buffer.h"

#define ENCODE_UINT16( uint16, buff, offset) { \
    buff[offset + 0] = (uint16 >> 8); \
    buff[offset + 1] = (uint16); \
}

#define ENCODE_UINT32( uint32, buff, offset) { \
    buff[offset + 0] = (uint32 >> 24); \
    buff[offset + 1] = (uint32 >> 16); \
    buff[offset + 2] = (uint32 >> 8); \
    buff[offset + 3] = (uint32); \
}

#define DECODE_UINT32( buff ) \
    ( (int) ((buff)[0] << 24)  + \
      (int) ((buff)[1] << 16)  + \
      (int) ((buff)[2] << 8)  + \
      (int) ((buff)[3] ) )

#define DECODE_UINT16( buff ) (\
    ( (int) ((buff)[0] << 8)  + \
    ( (int) (buff) [1] ) )

#define MAX_CLIENT 50

#define MAXIMUM_BUFFER_LENGTH (128 * 1024)

typedef struct __tCond{
    pthread_cond_t cnd;
    pthread_mutex_t mtx;
} tCond;

static void init_cond(tCond * cond){
    pthread_cond_init( &cond->cnd, NULL);
    pthread_mutex_init( &cond->mtx, NULL);
}

static void prepareForCondition(tCond * cond){
    pthread_mutex_lock(&cond->mtx);
}

static void waitForCondition(tCond * cond){
    pthread_cond_wait(&cond->cnd, &cond->mtx);
    pthread_mutex_unlock(&cond->mtx);
}

static void triggerCondition(tCond * cond){
    pthread_cond_signal(&cond->cnd);
    pthread_mutex_unlock(&cond->mtx);
}

#define CRC_LEN sizeof(uint32_t)

static unsigned int crc32b(uint8_t * message, uint32_t len) {
   int i, j;
   unsigned int byte, crc, mask;

   i = 0;
   crc = 0xFFFFFFFF;
   while (i < len) {
      byte = message[i];            // Get next byte.
      crc = crc ^ byte;
      for (j = 7; j >= 0; j--) {    // Do eight times.
         mask = -(crc & 1);
         crc = (crc >> 1) ^ (0xEDB88320 & mask);
      }
      i = i + 1;
   }
   return ~crc;
}

#define SELECT_TIMEOUT_USEC 5000
#define SELECT_TIMEOUT_SEC 1

#define CLOSING_TIMEOUT 5 * 1000000
#define RUNNING_TIMEOUT 10 * 1000000

typedef struct __tRemoteSocket{
    int fds[2];
    int remote_socket;

    //TODO : Add state
    long timeout;

    uint8_t transparent;
}tRemoteSocket;

typedef struct __tRemoteSocketContext{
    int connected;
    int remote_sd;
    tRemoteSocket sockets[MAX_CLIENT];
    ssize_t nb_sockets;
    int exit;

    uint8_t rx[MAXIMUM_BUFFER_LENGTH];
    uint8_t tx[MAXIMUM_BUFFER_LENGTH];

    pthread_mutex_t mutex_open;

    tCond cond_open;
    tCond cond_close;
    pthread_t thread_recv;

}tRemoteSocketContext;

typedef enum __eRemoteAction{
    OPEN = 0,
    OPEN_RESPONSE = 1,
    CLOSE = 2,
    DATA = 3,
    NB_ACTION = 4
}eRemoteAction;

typedef struct __tRemoteOperation{
    eRemoteAction action;
    union {
        struct {
            uint16_t port;
            uint32_t hostname_len;
            const char * hostame;
        } open;

        struct {
            int32_t fd;
        } open_response;

        struct {
            int32_t fd;
        } close;

        struct {
            int32_t fd;
            uint32_t data_len;
            const char *  data;
        } data;

        struct {
            int32_t fd;
            uint8_t blocking;
        } set_blocking;
    } action_details;
}tRemoteOperation;

tRemoteSocketContext g_RemoteSocket;

static int send_remote_operation( tRemoteOperation * operation);

static void handle_remote_server_closed(){
    log_message(LOG_INFO, "sock_proxy.c | handle_remote_server_closed : set Connected to false / kill app");
    g_RemoteSocket.connected = 0;
    g_RemoteSocket.exit = 1;
}

static void close_connection(int remote_fd){
    int idx = 0;
    int fd = remote_fd;

    log_message(LOG_INFO, "sock_proxy.c | close_connection : nbSocket =  %d", g_RemoteSocket.nb_sockets);

    for(idx = 0; idx < g_RemoteSocket.nb_sockets ; ++ idx){
        log_message(LOG_INFO, "sock_proxy.c | close_connection :  fd %d", fd);
        if( g_RemoteSocket.sockets[idx].remote_socket == fd)
            break;
    }

    if( idx == g_RemoteSocket.nb_sockets){
        log_message(LOG_ERR, "sock_proxy.c | close_connection : Invalid received fd %d", fd);
    } else {

        log_message(LOG_INFO, "sock_proxy.c | close_connection : socket found %d", fd);
        close(g_RemoteSocket.sockets[idx].fds[CLIENT_OFFSET]);
        close(g_RemoteSocket.sockets[idx].fds[SERVER_OFFSET]);

        g_RemoteSocket.sockets[idx] = g_RemoteSocket.sockets[g_RemoteSocket.nb_sockets - 1];
    }

    if( g_RemoteSocket.nb_sockets > 0 )
    {
        g_RemoteSocket.nb_sockets -= 1;
    }
}

static size_t sizeLinePos(const char * buff, size_t len){
    int idx;
    while(idx < len){
        if(buff[idx] == '\n')
            return idx + 1;

        // next offset
        idx += 1;
    }

    return idx;
}

int g_cpt = 0;

static void process_incoming_data_server(void){
    int offset = 0;
    int size = 0;

    int recv_data = read(g_RemoteSocket.remote_sd,
                        g_RemoteSocket.rx,
                        MAXIMUM_BUFFER_LENGTH);

    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : recv_data == %d", recv_data);

    if( recv_data <= 0){
        /* error remote_server closed */

        handle_remote_server_closed();

        log_message(LOG_ERR, "sock_proxy.c | process_incoming_data_server : recv_data == -1");
    } else if( recv_data > 0){

        while( offset < (recv_data)){

            size = DECODE_UINT32(&g_RemoteSocket.rx[offset]);
            offset += 4;

            log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : decoded size == %x", size);

            while (recv_data < (offset + size + CRC_LEN) )
            {
                int tmp = read(g_RemoteSocket.remote_sd,
                            &g_RemoteSocket.rx[recv_data],
                            ((size + offset + CRC_LEN) - recv_data));

                log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : read == %d bytes / ", tmp, size);

                if(tmp == -1){
                    //TODO
                    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : ERRNO == %s", strerror(errno));
                    recv_data = size + CRC_LEN;
                }else{
                    recv_data += tmp;
                }
            }


            // checksum verification

            uint32_t crc = 0;

            uint32_t crc_received = DECODE_UINT32( &(g_RemoteSocket.rx[offset + size]));
            crc = crc32b(&g_RemoteSocket.rx[offset], size);

            log_message(LOG_INFO, "sock_proxy.c | CRC VERIFICATION : %08x / %08x", crc, crc_received);

            if(crc != crc_received){
                log_message(LOG_ERR, "sock_proxy.c | CRC VERIFICATION FAILED : %08x / %08x", crc, crc_received);
            }




            char action = g_RemoteSocket.rx[offset ++];

            log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : action == %d", action);


            switch(action){
                case OPEN_RESPONSE:
                {
                    tRemoteSocket * sock = &g_RemoteSocket.sockets[ g_RemoteSocket.nb_sockets ++ ];
                    int ret = 0;

                    sock->remote_socket = DECODE_UINT32( &g_RemoteSocket.rx[offset]);
                    offset += 4;

                    socketpair(AF_UNIX, SOCK_STREAM, 0, sock->fds);

                    sock->timeout = RUNNING_TIMEOUT;
                    sock->transparent = FALSE;

                    prepareForCondition(&g_RemoteSocket.cond_open);

                    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : fds[SERVER_OFFSET] == %d", sock->fds[SERVER_OFFSET]);
                    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : fds[CLIENT_OFFSET] == %d", sock->fds[CLIENT_OFFSET]);

                    triggerCondition(&g_RemoteSocket.cond_open);

                    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : new nd_sockets == %d", g_RemoteSocket.nb_sockets);
                    break;
                }

                case DATA :
                {
                    struct buffer_s tmp;
                    int idx = 0;
                    int data_len = 0;
                    uint8_t * buff = NULL;
                    int fd = DECODE_UINT32( &g_RemoteSocket.rx[offset]);
                    offset += 4;

                    data_len = DECODE_UINT32( &g_RemoteSocket.rx[offset]);
                    offset += 4;

                    buff = &g_RemoteSocket.rx[offset];

                    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server tid(%d) : DATA from %d, size = %d, string -> %s, buff = 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x", pthread_self(), fd, data_len, buff, buff[0], buff[1], buff[2], buff[data_len - 3], buff[data_len - 2], buff[data_len - 1]);

                    for(idx = 0; idx < g_RemoteSocket.nb_sockets ; ++ idx){
                        if( g_RemoteSocket.sockets[idx].remote_socket == fd)
                            log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : DATA idx for fd found");
                            break;
                    }

                    if( idx == g_RemoteSocket.nb_sockets){
                        log_message(LOG_ERR, "sock_proxy.c | DATA : Invalid received fd %d", fd);
                    } else {

                        clear_buffer(&tmp);

                        idx = 0;

                        while(data_len > 0){
                            int tmp_len = data_len;
                            if(! g_RemoteSocket.sockets[idx].transparent ){
                                tmp_len = sizeLinePos(&g_RemoteSocket.rx[offset], data_len);
                            }

                            add_to_buffer(&tmp, &g_RemoteSocket.rx[offset], tmp_len);

                            log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server tid(%d): DATA write nb %d (%d bytes) data to fd : %d / transparent = %d", pthread_self(), g_cpt++, tmp_len, g_RemoteSocket.sockets[idx].fds[SERVER_OFFSET], g_RemoteSocket.sockets[idx].transparent );

                            write( g_RemoteSocket.sockets[idx].fds[SERVER_OFFSET], &tmp, sizeof(tmp));

                            clear_buffer(&tmp);

                            data_len -= tmp_len;
                            offset += tmp_len;
                        }

                        log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_server : After WRITING DATA");
                    }

                    offset += data_len;
                    break;
                }

                case CLOSE:
                {
                    int idx = 0;
                    int fd = DECODE_UINT32( &g_RemoteSocket.rx[offset]);
                    offset += 4;

                    for(idx = 0; idx < g_RemoteSocket.nb_sockets ; ++ idx) {
                        if( g_RemoteSocket.sockets[idx].remote_socket == fd)
                        {
                            g_RemoteSocket.sockets[idx].timeout = CLOSING_TIMEOUT;
                            break;
                        }
                    }

                    break;
                }

                default :
                    log_message(LOG_ERR, "sock_proxy.c | Invalid received action %d", action);
                    break;
            }

            offset += 4;
        }
    }
}



static void process_incoming_data_client(tRemoteSocket * remote_socket){

    tRemoteOperation ope;

    struct buffer_s tmp;

    int len = read(remote_socket->fds[SERVER_OFFSET], &tmp, sizeof( tmp));

    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_client : fd == %d", remote_socket->fds[SERVER_OFFSET]);

    log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_client (tid = %d): size = %d", pthread_self(), tmp.size);

    if(len > 0){

        while(tmp.size > 0){


            ope.action = DATA;
            ope.action_details.data.fd = remote_socket->remote_socket;
            ope.action_details.data.data_len = extract_buffer(&tmp, g_RemoteSocket.tx, MAXIMUM_BUFFER_LENGTH);

            ope.action_details.data.data = (const char *) g_RemoteSocket.tx;

            log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_client : size = %d / string data -> %s,  3 = 0x%02x 0x%02x 0x%02x", ope.action_details.data.data_len, g_RemoteSocket.tx, g_RemoteSocket.tx[len - 3], g_RemoteSocket.tx[len - 2],g_RemoteSocket.tx[len - 1]);
        }
    }else{

        log_message(LOG_INFO, "sock_proxy.c | process_incoming_data_client : size = %d  / close connection ", len);
        ope.action = CLOSE;
        ope.action_details.close.fd = remote_socket->remote_socket;

        close_connection(remote_socket->remote_socket);
    }

    send_remote_operation(&ope);
}

static long getCurrentTimeMicro(){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (long) (1000000 * tv.tv_sec) + tv.tv_usec;
}

static void * remote_socket_recv_thread(void * args){

    fd_set set;
    struct timeval timeout;
    int idx;
    int maxFds;
    int ret;
    long time_before;
    long time_after;
    long time_diff = 0;

    for(;;){

        if(g_RemoteSocket.connected == 0){
            usleep(500000);
            printf("Noy Connected Ahaha\n");
            continue;
        }

        if(g_RemoteSocket.exit == 1){
            break;
        }

        /* Initialize the file descriptor set. */
        FD_ZERO (&set);

        /* Initialize the timeout data structure. */
        timeout.tv_sec = SELECT_TIMEOUT_SEC;
        timeout.tv_usec = SELECT_TIMEOUT_USEC;

        FD_SET (g_RemoteSocket.remote_sd, &set);
        maxFds = g_RemoteSocket.remote_sd;

        /* closing stuff :) */

        idx = g_RemoteSocket.nb_sockets -1 ;

        while( ( g_RemoteSocket.nb_sockets > 0 ) && (idx >= 0 ) )
        {

            g_RemoteSocket.sockets[idx].timeout -= time_diff;

            log_message(LOG_INFO, "sock_proxy.c | g_RemoteSocket.sockets[%d].timeout = %d", idx, g_RemoteSocket.sockets[idx].timeout);

            if( g_RemoteSocket.sockets[idx].timeout < 0 ){
                log_message(LOG_INFO, "sock_proxy.c | g_RemoteSocket.sockets[%d] is getting closed / socket : %d", idx, g_RemoteSocket.sockets[idx].remote_socket);
                close_connection(g_RemoteSocket.sockets[idx].remote_socket);
            }
            else{
                int clientFd = g_RemoteSocket.sockets[idx].fds[SERVER_OFFSET];

                FD_SET (clientFd, &set);
                log_message(LOG_INFO, "sock_proxy.c | remote_socket_recv_thread FD_SET(%d, &set)", clientFd);
                maxFds = max(maxFds, clientFd);
            }
            --idx;
        }

        log_message(LOG_INFO, "sock_proxy.c | exit loop %d", g_RemoteSocket.nb_sockets);

        time_before = getCurrentTimeMicro();

        ret = select(maxFds + 1, &set, NULL, NULL, &timeout);

        time_after = getCurrentTimeMicro();

        time_diff = time_after - time_before;

        if(ret == -1){
            log_message(LOG_ERR, "sock_proxy.c | select returned -1 errno : %s", strerror(errno));

            idx = g_RemoteSocket.nb_sockets - 1;

            while( ( g_RemoteSocket.nb_sockets > 0 ) && (idx >= 0 ) )
            {
                int clientFd = g_RemoteSocket.sockets[idx].fds[SERVER_OFFSET];

                if( FD_ISSET(clientFd, &set) ){
                    int n = 0;
                    ioctl(clientFd, FIONREAD, &n);

                    if(n == 0){
                        log_message(LOG_ERR, "sock_proxy.c | idx = %d closed", clientFd);
                        close_connection(g_RemoteSocket.sockets[idx].remote_socket);
                    }
                }

                idx -- ;
            }

        }else if(ret == 0){

        }else{
            if(FD_ISSET(g_RemoteSocket.remote_sd, &set)){
                process_incoming_data_server();
            }

            for(idx = 0; idx < g_RemoteSocket.nb_sockets ; ++idx){
                int clientFd = g_RemoteSocket.sockets[idx].fds[SERVER_OFFSET];

                if( FD_ISSET(clientFd, &set) ){

                    log_message(LOG_INFO, "sock_proxy.c | remote_socket_recv_thread FD_ISSET(%d, &set) == true", clientFd);
                    process_incoming_data_client(&g_RemoteSocket.sockets[idx]);
                }
            }
        } /* if ret */
    }

    for(idx = 0; idx < g_RemoteSocket.nb_sockets ; ++idx){
        close(g_RemoteSocket.sockets[idx].fds[0]);
        close(g_RemoteSocket.sockets[idx].fds[1]);
        //close(g_RemoteSocket.sockets[idx].remote_socket);
    }

    g_RemoteSocket.nb_sockets = 0;

    exit(0);

    return NULL;
}

int init_remote_socket(void){

    int ret;

    memset(&g_RemoteSocket, 0, sizeof(tRemoteSocket));

    g_RemoteSocket.connected = 0;
    g_RemoteSocket.nb_sockets = 0;
    g_RemoteSocket.exit = 0;
    g_RemoteSocket.remote_sd = 0;

    init_cond(&g_RemoteSocket.cond_open);
    init_cond(&g_RemoteSocket.cond_close);

    pthread_mutex_init( &g_RemoteSocket.mutex_open, NULL);

    /* TODO start a thread for handling incoming */
    /* messages from the proxy agent */

    ret = pthread_create(&g_RemoteSocket.thread_recv,
                          NULL,
                          remote_socket_recv_thread,
                          NULL);

    return 0;
}

void end_remote_socket(void){
    g_RemoteSocket.exit = 1;
    log_message(LOG_ERR, "end_remote_socket");
    close(g_RemoteSocket.remote_sd);
}

int start_remote_socket_server(int port){

    int socket_desc  , c, ret;
    struct sockaddr_in server , client;
    const int on = 1;



    printf("creating socket\n");
    socket_desc = socket(AF_INET , SOCK_STREAM , 0);
    if (socket_desc == -1)
    {
        log_message(LOG_ERR,
                        "sock_proxy.c | Create socket failed: %s",
                        strerror(errno));
        close(socket_desc);
        return -1;
    }

    printf("reusing address");

    ret = setsockopt(socket_desc, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (ret != 0) {
            printf(
                        "setsockopt failed to set SO_REUSEADDR: %s",
                        strerror(errno));
            close(socket_desc);
            return -1;
    }


    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons( port );

    printf("binding");

    if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
    {
        printf(
                        "bind failed: %s",
                        strerror(errno));
        close(socket_desc);
        return -1;
    }

    printf( "listening");

    ret = listen(socket_desc , 50);
    if( ret < 0){
        printf(
                        "listen failed: %s",
                        strerror(errno));
        return -1;
    }

    c = sizeof(struct sockaddr_in);

    printf("accepting");

    g_RemoteSocket.remote_sd = accept(socket_desc, (struct sockaddr *)&client, (socklen_t*)&c);
    printf( "accepted");
    if (g_RemoteSocket.remote_sd < 0)
    {
        printf(
                        "accept failed: %s",
                        strerror(errno));
        return -1;
    }

    g_RemoteSocket.connected = 1;

    return 0;
}

int g_open = 0;

int remote_open(const char * hostname, int port, int * fd){
    tRemoteOperation ope;
    int currentNb = 0;
    ope.action = OPEN;
    ope.action_details.open.hostame = hostname;
    ope.action_details.open.hostname_len = strlen(hostname);
    ope.action_details.open.port = port;

    log_message(LOG_INFO, "sock_proxy.c | remote_open nb_socket = %d / MAX = %d", g_RemoteSocket.nb_sockets, MAX_CLIENT);

    if( g_RemoteSocket.nb_sockets == MAX_CLIENT ){
        return -1;
    }

    pthread_mutex_lock( &g_RemoteSocket.mutex_open );

    currentNb = g_RemoteSocket.nb_sockets;

    log_message(LOG_ERR, "sock_proxy.c | entering g_open = %d / current nb = %d", ++g_open, currentNb);

    prepareForCondition(&g_RemoteSocket.cond_open);

    send_remote_operation(&ope);

    waitForCondition(&g_RemoteSocket.cond_open);

    if(currentNb == g_RemoteSocket.nb_sockets){
        log_message(LOG_ERR, "sock_proxy.c | No new connection was opened");
    }

    *fd = g_RemoteSocket.sockets[currentNb].fds[CLIENT_OFFSET];

    log_message(LOG_ERR, "sock_proxy.c | leaving g_open = %d / fd = %d", g_open, *fd);

    pthread_mutex_unlock( &g_RemoteSocket.mutex_open );

    return 0;
}

void remote_close(int client_sd){
    tRemoteOperation ope;
    int idx;

    printf("remote_close called\n");

    for(idx = 0; idx < g_RemoteSocket.nb_sockets ; ++idx){
        if(g_RemoteSocket.sockets[idx].fds[CLIENT_OFFSET] == client_sd){
            break;
            printf("remote_close remote socket found at index %d\n", idx);
        }
    }

    if(idx == g_RemoteSocket.nb_sockets){
        printf("remote_close remote socket NOT found\n");
        return;
    }

    g_RemoteSocket.sockets[idx].timeout = CLOSING_TIMEOUT;
}

static int send_remote_operation( tRemoteOperation * operation){
    uint8_t buff[2048];

    uint32_t i = 0;
    uint32_t offset = 4;
    uint32_t crc = 0;

    buff[offset++] = operation->action;
    switch (operation->action)
    {
        case OPEN:
            ENCODE_UINT16(operation->action_details.open.port, buff, offset);
            offset += 2;
            ENCODE_UINT32( operation->action_details.open.hostname_len, buff, offset);
            offset += 4;

            for( i = 0; i < operation->action_details.open.hostname_len ; ++i ){
                buff[offset ++] = operation->action_details.open.hostame[i];
            }

            ENCODE_UINT32(offset - 4, buff, 0 )

            crc = crc32b(&buff[4], offset - 4);

            ENCODE_UINT32(crc, buff, offset);

            return write(g_RemoteSocket.remote_sd, buff, offset + 4);
            break;

        case CLOSE:
            ENCODE_UINT32( operation->action_details.close.fd, buff, offset);
            offset += 4;

            ENCODE_UINT32(offset - 4, buff, 0 )

            crc = crc32b(&buff[4], offset - 4);

            ENCODE_UINT32(crc, buff, offset);

            offset += CRC_LEN;

            return write(g_RemoteSocket.remote_sd, buff, offset);
            break;

        case DATA:
            {
                uint32_t offset_data = 0;
                int ret = 0;
                ENCODE_UINT32( operation->action_details.data.fd, buff, offset);
                offset += 4;
                ENCODE_UINT32( operation->action_details.data.data_len, buff, offset);
                offset += 4;

                ENCODE_UINT32(offset - 4 + operation->action_details.data.data_len, buff, 0 )

                do{
                    int nbCpy = operation->action_details.data.data_len - offset_data;
                    if((nbCpy + offset + CRC_LEN) > sizeof(buff) )
                        nbCpy = sizeof(buff) - offset - CRC_LEN;

                    memcpy( &buff[offset],
                            &operation->action_details.data.data[offset_data],
                            nbCpy);

                    offset += nbCpy;

                    crc = crc32b(&buff[4], offset - 4);

                    ENCODE_UINT32(crc, buff, offset);

                    offset += CRC_LEN;

                    ret = write(g_RemoteSocket.remote_sd, buff, offset);

                    offset_data += nbCpy;
                    offset = 0;

                }while ( (offset_data < operation->action_details.data.data_len ) && (ret >= 0) );

                return ret;
            }
            break;


        default:
            break;
    }
    return -1;
}


size_t safe_remote_write (int fd, const void *buf, size_t count){
    struct buffer_s tmp;
    clear_buffer(&tmp);

    add_to_buffer(&tmp, buf, count);

    write(fd, &tmp, sizeof(tmp));

    return count;
}

int write_remote_message (int fd, const char *fmt, ...){
    struct buffer_s tmp;
    clear_buffer(&tmp);

    add_variable_buffer(&tmp, fmt);

    write(fd, &tmp, sizeof(tmp));

    return tmp.size;
}

static tRemoteSocket * fetchByClientSocket(int sd){
    int i = 0;

    for( i = 0; i < g_RemoteSocket.nb_sockets; ++i){
        if(g_RemoteSocket.sockets[i].fds[CLIENT_OFFSET] == sd){
            return &g_RemoteSocket.sockets[i];
        }
    }

    return NULL;

}

size_t readremoteline (int fd, char **whole_buffer){
    struct buffer_s tmp;

    struct bufline_s * line_ptr = NULL;
    int len;

    tRemoteSocket * remoteSocketCtx = fetchByClientSocket(fd);

    if(remoteSocketCtx == NULL)
        return -1;

    clear_buffer(&tmp);

    len = read(fd, &tmp, sizeof(tmp));

    if( len <= 0 )
        return -1;

    // extract first line

    line_ptr = remove_from_buffer(&tmp);
    *whole_buffer = line_ptr->string;
    len = line_ptr->length;

    log_message(LOG_INFO, "sock_proxy.c | remotereadline Received line : %s", line_ptr->string);

    free (line_ptr);


    // write the rest on the socket if remaining len > 0

    if(tmp.size > 0)
    {
        log_message(LOG_ERR, "sock_proxy.c | readRemoteLine failed");
        write(remoteSocketCtx->fds[SERVER_OFFSET], &tmp, sizeof(tmp));
    }

    return len;
}

void remote_set_transparent(int fd){
    tRemoteSocket * remoteSocketCtx = fetchByClientSocket(fd);
    remoteSocketCtx->transparent = 1;
}

void remote_set_nontransparent(int fd){
    tRemoteSocket * remoteSocketCtx = fetchByClientSocket(fd);
    remoteSocketCtx->transparent = 0;
}



#endif