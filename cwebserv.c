#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "cwebserv.h"

struct chrbuf {
	//the buffer for incoming socket data, and associated info
	char *buf;
	char *buf_to_send;
	int bsize;
	int buflen;
	int sentbytes;
	int started_sending; //0 for no, 1 for yes
	int finished_sending; //0 for no, 1 for yes, 2 to close the connection
        int keepalive;
        time_t last_accessed;
};

int addfd(struct pollfd *fds, struct chrbuf *bufs, nfds_t *nfds,
        nfds_t *maxfds, int fd, short events) {
    //add the fd to the list of fds. events is the events that poll will be
    //listing for on the fd (as described in poll(2))
    //nfds the number of fds in *fds, maxfds is the allocated size of fds,
    //and fd is the file descriptor.
    //addfd returns the index in fds of the addition
    if (*maxfds <= *nfds) fds = realloc(fds, *maxfds += FD_LENGTH_INCREMENT);
    fds[*nfds].fd = fd;
    fds[*nfds].events = events;
    fds[*nfds].revents = 0; //just in case
    bufs[*nfds].buf_to_send = 0;
    bufs[*nfds].buf = malloc(bufs[*nfds].bsize = BUFSTEP);
    bufs[*nfds].buflen = bufs[*nfds].sentbytes = 0;
    bufs[*nfds].started_sending = bufs[*nfds].finished_sending = 0;
    bufs[*nfds].keepalive = 0;

    time_t now;
    time(&now);
    bufs[*nfds].last_accessed = now;
    return (*nfds)++;
}

void deletefd(struct pollfd *fds, struct chrbuf *bufs, nfds_t *nfds,
        int fd) {
    //remove fds matching fd from *fds
    //nfds the number of fds in *fds, and fd is the file descriptor.
    int i, offset = 0;

    //when a fd is removed, the other entries need to be slid down.
    //Just to be safe, every entry matching fd is removed, not just the first.
    for (i = 0; i < *nfds; i++) {
        if (fds[i].fd == fd) {
            free(bufs[i].buf);
            offset--;
        } else if (offset < 0) {
            fds[i + offset] = fds[i];
            bufs[i + offset] = bufs[i];
        }
    }
    *nfds = i + offset;
}

int removechar(char *string, int length, char exile) {
    //remove all occourances of exile from *string, and return the new length.
    int i, offset = 0;
    for (i = 0; i < length && string[i] != '\0'; i++) {
        if (string[i] == exile) {
            offset--;
        } else {
            string[i + offset] = string[i];
        }
    }
    string[i + offset] = '\0';
    return i + offset;
}

void sendall(int fd, struct chrbuf *buf) {
    //will start sending all of the string in buf_to_send, with length buflen
    //will also continue sending if sending has already been started
    int chars = send(fd, buf->buf_to_send + buf->sentbytes,
            buf->buflen - buf->sentbytes, 0);
    buf->started_sending = 1;
    if (chars == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            //actual error
#ifdef CWEBSERV_DEBUG
            printf("Error sending, errno = %s (%i)\n", strerror(errno), errno);
#endif
            buf->finished_sending = 2;
        }
    } else {
        buf->sentbytes += chars;
        //finished_sending will be 1 if finished, 0 if not
        buf->finished_sending = buf->sentbytes >= buf->buflen;
    }
}

void sendresponse(int fd, struct chrbuf *buf, char *path, char *query,
        char *headers, char *http_version,
        char *(*makeoutput)(char *query, char *path, int *final_length)) {
    //send the response, but don't necessarily wait for it to be fully sent
    char template[] = "HTTP/1.0 200 OK\r\n"
        "Content-Length: %i\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "%s\n";
    int output_length, chars, original_bsize;

    if (strcasestr(http_version, "HTTP/1.1")) {
        if (!strcasestr(headers, "Connection: close"))
            buf->keepalive = 1;
    } else if (strcasestr(headers, "Connection: keep-alive")) {
        //let's assume it's HTTP/1.0 if it's not HTTP/1.1
        buf->keepalive = 1;
    }

    char *output = makeoutput(query, path, &output_length);

    //make the HTTP response and put it in buf->buf, overwriting the recv'd data
    chars = snprintf(buf->buf, buf->bsize, template, output_length, output);
    buf->buflen = chars;
    original_bsize = buf->bsize;
    buf->buf = realloc(buf->buf, buf->bsize = 1 + chars); //make the buffer fit
    if (original_bsize <= chars) {
        //if the buf->buf string was too small, snprintf it again
        chars = snprintf(buf->buf, buf->bsize, template, output_length, output);
    }
    free(output);

    //send buf->buf
    buf->buf_to_send = buf->buf;
    sendall(fd, buf);
}

void process(int fd, struct chrbuf *buf,
        char *(*makeoutput)(char *query, char *path, int *final_length)) {
    //will start processing a HTTP request
    char HEAD_response[] = "HTTP/1.0 200 OK\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n";
    char bad_request[] = "HTTP/1.0 400 Bad Request\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: 114\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<head><title>400 Bad Request</title></head>\r\n"
        "<center><h1>400 Bad Request</h1></center>\r\n"
        "</body>\r\n"
        "</html>\r\n";
    char length_required[] = "HTTP/1.0 411 Length Required\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: 123\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<head><title>411 Length Required</title></head>\r\n"
        "<center><h1>411 Length Required</h1></center>\r\n"
        "</body>\r\n"
        "</html>\r\n";
    char not_allowed[] = "HTTP/1.0 405 Method Not Allowed\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: 128\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<head><title>405 Method Not Allowed</title></head>\r\n"
        "<center><h1>405 Method Not Allowed</h1></center>\r\n"
        "</body>\r\n"
        "</html>\r\n";
    char *headers, *query, *path, *http_version;
    int chars = 0, spaces = 0, i, buffer_full = 0;

    do {
        //recieve incoming request
        //put the incoming data into the next available part of the buf->buf
        errno = 0;
        chars = recv(fd, buf->buf + buf->buflen,
                (buf->bsize - 1) - buf->buflen, 0);
#ifdef CWEBSERV_DEBUG
        printf("Recv'd %i (maximum %i) ", chars, buf->bsize - 1);
#endif
        if (chars < 0) {
            printf("(err is %s (%i)) \n", strerror(errno), errno);
            if (errno == EAGAIN) {
#ifdef CWEBSERV_DEBUG
                printf("EAGAIN in recv\n");
#endif
                return; //no data was recieved just yet.
            } else {
                //real error
                printf("Chars < 0 = %i, errno = %s\n", chars, strerror(errno));
                buf->finished_sending = 2;
                return;
            }
        } else if (chars == 0) {
            //Either bsize is 0 (which it shouldn't be) or the HTTP client
            //gracefully closed the connection.
            printf("Chars == 0 (client disconnected), errno = %s\n",
                    strerror(errno));
            buf->finished_sending = 1;
            return;
        }
        buf->buflen += chars;
        buf->buf[buf->buflen] = '\0'; //cap the string with a null terminator
#ifdef CWEBSERV_DEBUG
        printf("of '%s'\n", buf->buf);
#endif
        buffer_full = (buf->bsize == buf->buflen + 1);
        //if the buf->buf is full, expand it and recieve some more.
        if (buffer_full) {
            //add to the buf->buf in BUFSTEP char increments
            buf->bsize += BUFSTEP;
            buf->buf = realloc(buf->buf, buf->bsize);
        }
    } while (buffer_full);

    for (i = 0; i < buf->buflen && buf->buf[i] != '\n'; i++) {
        if (buf->buf[i] == ' ') spaces++;
    }
    if (i == buf->buflen) {
        //buf is not fully recv'd.
        return;
    }

#ifdef CWEBSERV_DEBUG
    printf("spaces = %i\n", spaces);
#endif
    if (spaces == 1) {
        //request is the short "Simple-Request"
        if (!strncmp(buf->buf, "GET ", 4)) {
            buf->buflen = removechar(buf->buf, buf->buflen, '\r');
            //The HTTP rfc specifies \r\n as a line terminator, but \r is
            //removed and \n is looked for,
            //just in case someone uses \n instead.
#ifdef CWEBSERV_DEBUG
            printf("so far: '%s'\n", buf->buf);
#endif
            buf->buf[i] = '\0';
            char *firstq = strchr(buf->buf, '?');
            if (firstq && firstq < buf->buf + i) {
                query = firstq + 1;
                *firstq = '\0';
            } else {
                query = "";
            }
            path = buf->buf + 4;
            http_version = "HTTP/1.0";
            headers = "";
            sendresponse(fd, buf, path, query, headers, http_version,
                    makeoutput);
        } else {
            buf->buf_to_send = bad_request;
            buf->buflen = sizeof(bad_request) - 1;
            sendall(fd, buf);
#ifdef CWEBSERV_DEBUG
            printf("Sent bad_request #1; resplen: %i, errno: %i;\n",
                    sizeof(bad_request), errno);
#endif
            buf->finished_sending = 2;
            return;
        }
    } else if (spaces == 2) {
#ifdef CWEBSERV_DEBUG
        printf("buf->buf was '%s'", buf->buf);
#endif
        buf->buflen = removechar(buf->buf, buf->buflen, '\r');
#ifdef CWEBSERV_DEBUG
        printf("buf->buf is '%s'", buf->buf);
#endif
        if (strstr(buf->buf, "\n\n")) {
            //headers have arrived
            if (!strncmp(buf->buf, "HEAD ", 5)) {
#ifdef CWEBSERV_DEBUG
                printf("HEAD request detected!\n");
#endif
                buf->buf_to_send = HEAD_response;
                buf->buflen = sizeof(HEAD_response) - 1;
                sendall(fd, buf);
#ifdef CWEBSERV_DEBUG
                printf("Sent HEAD_response; len: %i\n", sizeof(HEAD_response));
#endif
            } else if (!strncmp(buf->buf, "GET ", 4)) {
#ifdef CWEBSERV_DEBUG
                printf("GET request detected!\n");
#endif
                //tmp becomes a pointer to buf after the space
                char *tmp = buf->buf + 4;
                //determine if there is a query string
                char *firstsp = strchr(tmp, ' ');
                if (!firstsp) {
                    buf->buf_to_send = bad_request;
                    buf->buflen = sizeof(bad_request) - 1;
                    sendall(fd, buf);
#ifdef CWEBSERV_DEBUG
                    printf("Sent bad_request #1; resplen: %i, errno: %i;\n",
                            sizeof(bad_request), errno);
#endif
                    return;
                }
                char *firstq = strchr(tmp, '?');
                path = strsep(&tmp, " ?");
                query = ((firstq && firstq < firstsp) ? strsep(&tmp, " ") : "");
                http_version = strsep(&tmp, "\n");
                headers = tmp;
                sendresponse(fd, buf, path, query, headers, http_version,
                        makeoutput);
            } else if (!strncmp(buf->buf, "POST ", 5)) {
#ifdef CWEBSERV_DEBUG
                printf("POST request detected!\n");
#endif
                if (strstr(buf->buf, "Transfer-Encoding: chunked")) {
                    /*Chunked transfer encoding is not supported, so we must
                      require content-length*/
                    buf->buf_to_send = length_required;
                    buf->buflen = sizeof(length_required) - 1;
                    sendall(fd, buf);
#ifdef CWEBSERV_DEBUG
                    printf("Sent length_required; resplen: %i, errno: %i;\n",
                            sizeof(length_required), errno);
#endif
                    return;
                } else {
                    char *tofree;
                    //tmp becomes a copy of buf after the space
                    char *tmp = tofree = strdup(buf->buf + 5);
                    /*It needs to be a copy, and not just a pointer because if
                      we might find out halfway though that the request isn't
                      done, we can't overwrite our buffer yet since we're
                      still using it for input*/
                    path = strsep(&tmp, " ");
                    http_version = strsep(&tmp, "\n");
                    headers = tmp;
                    query = strstr(tmp, "\n\n");
                    if (query) {
                        //make the \n\n part into \n\0, and set query to the
                        //char after
                        *++query = '\0';
                        query++;

                        char content_length[] = "Content-Length:";
                        char *cl = strcasestr(headers, content_length);
                        if (!cl) { //content-length not found
                            buf->buf_to_send = length_required;
                            buf->buflen = sizeof(length_required) - 1;
                            sendall(fd, buf);
#ifdef CWEBSERV_DEBUG
                            printf("Sent length_required #2; resplen: %i,"
                                    " errno: %i;\n", sizeof(length_required),
                                    errno);
#endif
                            free(tofree);
                            return;
                        } else {
                            cl += sizeof(content_length) - 1;
                            char **endptr = 0;
                            long length = strtol(cl, endptr, 10);
                            if (strlen(query) < length) {
                                //not done recieving
                                free(tofree);
                                return;
                            }
                        }
                    } else {
                        query = "";
                    }
                    sendresponse(fd, buf, path, query, headers, http_version,
                            makeoutput);
                    free(tofree);
                }
            } else {
                chars = send(fd, not_allowed, sizeof(not_allowed), 0);
#ifdef CWEBSERV_DEBUG
                printf("Sent not_allowed; chars: %i; resplen: %i, errno: %i;\n",
                        chars, sizeof(not_allowed), errno);
#endif
                buf->finished_sending = 1;
                return;
            }
        } else {
#ifdef CWEBSERV_DEBUG
            printf("NO \\n\\n detected!\n");
#endif
        }
    } else {
        buf->buf_to_send = bad_request;
        buf->buflen = sizeof(bad_request) - 1;
        sendall(fd, buf);
#ifdef CWEBSERV_DEBUG
        printf("Sent bad_request #3; spaces: %i; resplen: %i, errno: %i;\n",
                spaces, sizeof(bad_request), errno);
#endif
        return;
    }
}

void cwebserv(char *port,
        char *(*makeoutput)(char *query, char *path, int *final_length)) {

    struct sockaddr_storage their_addr;
    socklen_t addr_size;
    struct addrinfo hints, *res;
    int sockfd, new_fd;

    //start setting up our server on its port
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6 automatically
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;     // fill in my IP for me

    // Connects on port PORT, otherwise the first argument
    getaddrinfo(NULL, port, &hints, &res);

    // make a socket, bind it, and listen on it:

    errno = 0;
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    fcntl(sockfd, F_SETFL, O_NONBLOCK);
    if (sockfd == -1) {
        printf("Socket failed, error %s, errno %i.\n", strerror(errno), errno);
        exit(1);
    }
    if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        printf("Bind failed, error %s, errno %i.\n", strerror(errno), errno);
        exit(1);
    }

    struct timeval timeout;      
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    if (listen(sockfd, BACKLOG) == -1) {
        printf("Error listening, errno is %i\n", errno);
        exit(1);
    }

    struct pollfd fds[INITIAL_FD_LENGTH]; //array of file descriptors
    nfds_t nfds = 0, maxfds = INITIAL_FD_LENGTH; //number of file descriptors
    struct chrbuf bufs[INITIAL_FD_LENGTH];
    int i, fdindex;

    if (getuid() == 0) {
        /* process is running as root, drop privileges */
        if (setgid(DROP_TO_GROUP) != 0) {
            printf("setgid: Unable to drop group privileges: %s",
                    strerror(errno));
            exit(1);
        }
        if (setuid(DROP_TO_USER) != 0) {
            printf("setuid: Unable to drop user privileges: %s",
                    strerror(errno));
            exit(1);
        }
    }

    printf("Server started on port %s\n", port);

    //done setting up, now let's accept incoming connections
    addr_size = sizeof(their_addr);
    while (1) { //main loop
        //check open connections
        poll(fds, nfds, 50); //poll for updates with 50 ms timeout
        time_t now;
        for (i = 0; i < nfds; i++) {
            if (bufs[i].finished_sending == 2) {
#ifdef CWEBSERV_DEBUG
                printf("bufs[%i] forced closed\n", i);
#endif
                close(fds[i].fd);
                deletefd(fds, bufs, &nfds, fds[i].fd);
            } else if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                //if there is some sort of error, just give up
#ifdef CWEBSERV_DEBUG
                printf("fds[%i] (POLLERR | POLLHUP | POLLNVAL)\n", i);
#endif
                close(fds[i].fd);
                deletefd(fds, bufs, &nfds, fds[i].fd);
            } else {
                if (fds[i].revents & POLLOUT) { //if the data has gone out
#ifdef CWEBSERV_DEBUG
                    printf("fds[%i] POLLOUT\n", i);
#endif
                    if (bufs[i].finished_sending) {
                        if (bufs[i].keepalive) {
#ifdef CWEBSERV_DEBUG
                            printf("bufs[%i] keep-alive-ing\n", i);
#endif
                            //prepare bufs[i] for another go.
                            bufs[i].buf[0] = '\0';
                            bufs[i].buflen = 0;
                            bufs[i].started_sending = 0;
                            bufs[i].finished_sending = 0;
                            bufs[i].keepalive = 0;
                            bufs[i].sentbytes = 0;
                            bufs[i].buf_to_send = 0;
                            time(&now);
                            bufs[i].last_accessed = now;
                        } else {
#ifdef CWEBSERV_DEBUG
                            printf("bufs[%i] finished sending\n", i);
#endif
                            close(fds[i].fd);
                            deletefd(fds, bufs, &nfds, fds[i].fd);
                            continue;
                        }
                    } else if (bufs[i].started_sending) {
#ifdef CWEBSERV_DEBUG
                        printf("bufs[%i] continuing to send\n", i);
#endif
                        //continue sending
                        sendall(fds[i].fd, bufs + i);
                        time(&now);
                        bufs[i].last_accessed = now;
                    }
                }
                if ((fds[i].revents & POLLIN) && !bufs[i].started_sending) {
                    //get and use new data from a connection
#ifdef CWEBSERV_DEBUG
                    printf("fds[%i] POLLIN\n", i);
#endif
                    process(fds[i].fd, bufs + i, makeoutput);
                    time(&now);
                    bufs[i].last_accessed = now;
                }
            }
            time(&now);
            if (now - 10 > bufs[i].last_accessed) {
                if (bufs[i].buflen) {
                    //If we started recieving, we can send a timeout.
                    char timed_out[] = "HTTP/1.0 408 Request Timeout\r\n"
                        "Content-Type: text/html; charset=UTF-8\r\n"
                        "Content-Length: 123\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "<html>\r\n"
                        "<head><title>408 Request Timeout</title></head>\r\n"
                        "<center><h1>408 Request Timeout</h1></center>\r\n"
                        "</body>\r\n"
                        "</html>\r\n";
                    //socket timed out with timeout of 10 seconds
#ifdef CWEBSERV_DEBUG
                    printf("bufs[%i] timed out\n", i);
#endif
                    bufs[i].buf_to_send = timed_out;
                    bufs[i].buflen = sizeof(timed_out) - 1;
                    sendall(fds[i].fd, bufs + i);
#ifdef CWEBSERV_DEBUG
                    printf("Sent timed_out; resplen: %i, errno: %i;\n",
                            sizeof(timed_out), errno);
#endif
                }
                close(fds[i].fd);
                deletefd(fds, bufs, &nfds, fds[i].fd);
            }
        }

#ifdef CWEBSERV_DEBUG
        printf("nfds is %i; will accept in 1 second\n", nfds);
        sleep(1);
#endif

        //check for new connections
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &addr_size);
        if (new_fd == -1) {
            //if there is an error
            if (errno == EAGAIN) {
                //nothing was recieved just yet.
                continue;
            } else {
                //actual error
                printf("Accept encountered an error with errno '%i'\n", errno);
                continue;
            }
        } else {
#ifdef CWEBSERV_DEBUG
            printf("accepted with new_fd = %i\n", new_fd);
#endif
        }

        //allocate buffers for new_fd
        fdindex = addfd(fds, bufs, &nfds, &maxfds, new_fd,
                POLLIN | POLLOUT | POLLHUP);


        //recv initial data.
        process(fds[fdindex].fd, bufs + fdindex, makeoutput);
    }
    close(sockfd);
}
