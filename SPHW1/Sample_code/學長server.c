#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define ERR_EXIT(a) { perror(a); exit(1); }

typedef struct {
    char hostname[512];  // server's hostname
    unsigned short port;  // port to listen
    int listen_fd;  // fd to wait for a new connection
} server;

typedef struct {
    char host[512];  // client's host
    int conn_fd;  // fd to talk with client
    char buf[512];  // data sent by/to client
    size_t buf_len;  // bytes used by buf
    // you don't need to change this.
    char* filename;  // filename set in header, end with '\0'.
    int header_done;  // used by handle_read to know if the header is read or not.
} request;

server svr;  // server
request* requestP = NULL;  // point to a list of requests
int maxfd;  // size of open file descriptor table, size of request list

const char* accept_header = "ACCEPT\n";
const char* reject_header = "REJECT\n";

// Forwards

static void init_server(unsigned short port);
// initailize a server, exit for error

static void init_request(request* reqP);
// initailize a request instance

static void free_request(request* reqP);
// free resources used by a request instance

static int handle_read(request* reqP);
// return 0: socket ended, request done.
// return 1: success, message (without header) got this time is in reqP->buf with reqP->buf_len bytes. read more until got <= 0.
// It's guaranteed that the header would be correctly set after the first read.
// error code:
// -1: client connection error
char str[1030][1005] = {0};
int cnt[1030] = {0};
struct flock fl;
int main(int argc, char** argv) {
    int i, ret;

    struct sockaddr_in cliaddr;  // used by accept()
    int clilen;

    int conn_fd;  // fd for a new connection with client
    int file_fd;  // fd for file that we open for reading
    char buf[512];
    int buf_len;

    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }

    // Initialize server
    init_server((unsigned short) atoi(argv[1]));

    // Get file descripter table size and initize request table
    maxfd = getdtablesize();
    requestP = (request*) malloc(sizeof(request) * maxfd);
    if (requestP == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (i = 0; i < maxfd; i++) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requestP[svr.listen_fd].host, svr.hostname);

    // Loop for handling connections
    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n", svr.hostname, svr.port, svr.listen_fd, maxfd);

    int check[maxfd + 5];
    memset(check, 0, sizeof(check));
    check[3] = 1;
    while (1) {
        // TODO: Add IO multiplexing
        // Check new connection
        clilen = sizeof(cliaddr);

        // check file descriptor
        fd_set readset;
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        //printf("%d\n", svr.listen_fd);
        int i;
        FD_ZERO(&readset);
        for(i = 0; i < maxfd; i++) {
            if(check[i]) FD_SET(i, &readset);
        }

        if(select(maxfd, &readset, NULL, NULL, &timeout)<0) return 0 ;

        printf("%d....\n", 222);
        if(FD_ISSET(3, &readset)) {
            conn_fd = accept(svr.listen_fd, (struct sockaddr*)&cliaddr, (socklen_t*)&clilen);
            if (conn_fd < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;  // try again
                if (errno == ENFILE) {
                    (void) fprintf(stderr, "out of file descriptor table ... (maxconn %d)\n", maxfd);
                    continue;
                }
                ERR_EXIT("accept")
            }
            check[conn_fd] = 1;
            requestP[conn_fd].conn_fd = conn_fd;
            strcpy(requestP[conn_fd].host, inet_ntoa(cliaddr.sin_addr));    
            fprintf(stderr, "getting a new request... fd %d from %s\n", conn_fd, requestP[conn_fd].host);
        }
        
        
        
        file_fd = -1;
        conn_fd = -1;

        for(i = 4; i < maxfd; i++){
            if(!check[i] || !FD_ISSET(i, &readset)) continue;
            conn_fd = i;
        }

        if(conn_fd == -1) continue;
#ifdef READ_SERVER
        ret = handle_read(&requestP[conn_fd]);
        if (ret < 0) {
            fprintf(stderr, "bad request from %s\n", requestP[conn_fd].host);
            continue;
        }
        // requestP[conn_fd]->filename is guaranteed to be successfully set.
        if (file_fd == -1) {
            // open the file here.
            fprintf(stderr, "Opening file [%s]\n", requestP[conn_fd].filename);
            // TODO: Add lock
            // TODO: check if the request should be rejected.
            file_fd = open(requestP[conn_fd].filename, O_RDONLY, 0);
            //lock
            fl.l_type   = F_RDLCK;  /* F_RDLCK, F_WRLCK, F_UNLCK    */
            fl.l_whence = SEEK_SET; /* SEEK_SET, SEEK_CUR, SEEK_END */
            fl.l_start  = 0;        /* Offset from l_whence         */
            fl.l_len    = 0;        /* length, 0 = to EOF           */
            fl.l_pid    = getpid();

            if(fcntl(file_fd, F_SETLK, &fl) < 0){
                write(requestP[conn_fd].conn_fd, reject_header, sizeof(reject_header));
                goto rd_end;
            }

            write(requestP[conn_fd].conn_fd, accept_header, sizeof(accept_header));
        }
        if (ret == 0) break;
        while (1) {
            ret = read(file_fd, buf, sizeof(buf));
            if (ret < 0) {
                fprintf(stderr, "Error when reading file %s\n", requestP[conn_fd].filename);
                break;
            } else if (ret == 0) break;
            write(requestP[conn_fd].conn_fd, buf, ret);
        }
        fprintf(stderr, "Done reading file [%s]\n", requestP[conn_fd].filename);  
        fl.l_type   = F_UNLCK;  /* tell it to unlock the region */
        fcntl(file_fd, F_SETLK, &fl); /* set the region to unlocked */
        rd_end:;
#endif

#ifndef READ_SERVER
        do {
            FD_ZERO(&readset);
            FD_SET(conn_fd, &readset);
            select(maxfd, &readset, NULL, NULL, &timeout);
            if(FD_ISSET(conn_fd, &readset) == 0) goto next;

            ret = handle_read(&requestP[conn_fd]);
	    fprintf(stderr,"conn_fd:%d ret:%d\n",conn_fd,ret);
            int is = 1;
            for(i = 0; i < maxfd; i++){
                if(!check[i] || i == conn_fd) continue;
                if(strcmp(str[i], requestP[conn_fd].filename) == 0) is = 0;
            }
            if(is == 0){
	      fprintf(stderr,"It is opening!\n");
                write(requestP[conn_fd].conn_fd, reject_header, sizeof(reject_header));
                goto close;
            }
            if (ret < 0) {
                fprintf(stderr, "bad request from %s\n", requestP[conn_fd].host);
                continue;
            }
            // requestP[conn_fd]->filename is guaranteed to be successfully set.
            file_fd = cnt[conn_fd];
            if (file_fd == 0) {
                // open the file here.
	      fprintf(stderr, "conn_fd %d Opening file [%s]\n",conn_fd, requestP[conn_fd].filename);
                sprintf(str[conn_fd], "%s", requestP[conn_fd].filename);
                // TODO: Add lock
                // TODO: check if the request should be rejected.
                file_fd = open(requestP[conn_fd].filename, O_WRONLY | O_CREAT | O_TRUNC,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
                cnt[conn_fd] = file_fd;
                //lock

                fl.l_type   = F_WRLCK;  /* F_RDLCK, F_WRLCK, F_UNLCK    */
                fl.l_whence = SEEK_SET; /* SEEK_SET, SEEK_CUR, SEEK_END */
                fl.l_start  = 0;        /* Offset from l_whence         */
                fl.l_len    = 0;        /* length, 0 = to EOF           */
                fl.l_pid    = getpid();

                if(fcntl(file_fd, F_SETLK, &fl) < 0){
		  fprintf(stderr,"it is oprning now\n");
                    write(requestP[conn_fd].conn_fd, reject_header, sizeof(reject_header));
                    goto wd_end;
                }
		fprintf(stderr,"accept\n");
                write(requestP[conn_fd].conn_fd, accept_header, sizeof(accept_header));
            }
            if (ret == 0) break;
            write(file_fd, requestP[conn_fd].buf, requestP[conn_fd].buf_len);
        } while (ret > 0);


        fprintf(stderr, "conn_fd %d Done writing file [%s]\n",conn_fd, requestP[conn_fd].filename);
        fl.l_type   = F_UNLCK;  /* tell it to unlock the region */
        fcntl(file_fd, F_SETLK, &fl); /* set the region to unlocked */
        wd_end:;
        sprintf(str[conn_fd], "%s", "");
        cnt[conn_fd] = 0;
#endif

        if (file_fd >= 0) close(file_fd);
        close:;
        check[conn_fd] = 0;
        close(requestP[conn_fd].conn_fd);
	fprintf(stderr,"Close:%d\n",requestP[conn_fd].conn_fd);
        free_request(&requestP[conn_fd]);    
        next:;
    }

    free(requestP);
    return 0;
}


// ======================================================================================================
// You don't need to know how the following codes are working
#include <fcntl.h>

static void* e_malloc(size_t size);


static void init_request(request* reqP) {
    reqP->conn_fd = -1;
    reqP->buf_len = 0;
    reqP->filename = NULL;
    reqP->header_done = 0;
}

static void free_request(request* reqP) {
    if (reqP->filename != NULL) {
        free(reqP->filename);
        reqP->filename = NULL;
    }
    init_request(reqP);
}

// return 0: socket ended, request done.
// return 1: success, message (without header) got this time is in reqP->buf with reqP->buf_len bytes. read more until got <= 0.
// It's guaranteed that the header would be correctly set after the first read.
// error code:
// -1: client connection error
static int handle_read(request* reqP) {
    int r;
    char buf[512];

    // Read in request from client
    r = read(reqP->conn_fd, buf, sizeof(buf));
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (reqP->header_done == 0) {
        char* p1 = strstr(buf, "\015\012");
        int newline_len = 2;
        // be careful that in Windows, line ends with \015\012
        if (p1 == NULL) {
            p1 = strstr(buf, "\012");
            newline_len = 1;
            if (p1 == NULL) {
                // This would not happen in testing, but you can fix this if you want.
                ERR_EXIT("header not complete in first read...");
            }
        }
        size_t len = p1 - buf + 1;
        reqP->filename = (char*)e_malloc(len);
        memmove(reqP->filename, buf, len);
        reqP->filename[len - 1] = '\0';
        p1 += newline_len;
        reqP->buf_len = r - (p1 - buf);
        memmove(reqP->buf, p1, reqP->buf_len);
        reqP->header_done = 1;
    } else {
        reqP->buf_len = r;
        memmove(reqP->buf, buf, r);
    }
    return 1;
}

static void init_server(unsigned short port) {
    struct sockaddr_in servaddr;
    int tmp;

    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) ERR_EXIT("socket");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    tmp = 1;
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (bind(svr.listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }
}

static void* e_malloc(size_t size) {
    void* ptr;

    ptr = malloc(size);
    if (ptr == NULL) ERR_EXIT("out of memory");
    return ptr;
}

