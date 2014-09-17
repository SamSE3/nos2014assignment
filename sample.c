/*
  Sample solution for NOS 2014 assignment: implement a simple multi-threaded 
  IRC-like chat service.

  (C) Paul Gardner-Stephen 2014.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

 */

//http://www.anta.net/misc/telnet-troubleshooting/irc.shtml

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <netdb.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>

struct client_thread {
    pthread_t thread;
    int thread_id;
    int fd;

    int nicknamelength;
    char nickname[32];
    int usernamelength;
    char username[32];

    int state;
    int mode; //
    time_t timeout;

    int line_length;
    char line[1024];

    int buffer_length;
    unsigned char buffer[8192];

    int next_message;
};

//holds the client threads
#define MAX_CLIENTS 50
#define DEAD 1
#define ALIVE 2

int connection_count = 0;

struct client_thread threads[MAX_CLIENTS];
pthread_rwlock_t message_log_lock;

int connection_main(struct client_thread* t);

int read_from_socket(int sock, unsigned char *buffer, int *count, int buffer_size,
        int timeout) {
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, NULL) | O_NONBLOCK); // make sure sock wont block

    int t = time(0) + timeout;
    if (*count >= buffer_size) return 0; // got some data return zero
    int r = read(sock, &buffer[*count], buffer_size - *count);
    //address of the 0th elem in the array t->buffer
    while (r != 0) {
        if (r > 0) {
            (*count) += r;
            break;
        }
        r = read(sock, &buffer[*count], buffer_size - *count);
        if (r == -1 && errno != EAGAIN) { // no double error
            perror("read() returned error. Stopping reading from socket.");
            return -1;
        } else usleep(100000); // sleep 
        // timeout after a few seconds of nothing
        if (time(0) >= t) break; //check the timeout
    }
    buffer[*count] = 0; //null the end of the string
    return 0;
}

int create_listen_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) return -1;

    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof (on)) == -1) {
        close(sock);
        return -1;
    }
    if (ioctl(sock, FIONBIO, (char *) &on) == -1) {
        close(sock);
        return -1;
    }

    /* Bind it to the next port we want to try. */
    struct sockaddr_in address;
    bzero((char *) &address, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *) &address, sizeof (address)) == -1) {
        close(sock);
        return -1;
    }

    if (listen(sock, 20) != -1) return sock;

    close(sock);
    return -1;
}

int accept_incoming(int sock) {
    struct sockaddr addr;
    unsigned int addr_len = sizeof addr;
    int asock;
    if ((asock = accept(sock, &addr, &addr_len)) != -1) {
        return asock;
    }

    return -1;
}



//int connection_main(int fd);

void* client_thread_entry(void * arg) {
    struct client_thread *t = arg;

    //run the thread
    t->state = DEAD;
    connection_main(t);
    return NULL;
}

int client_count = 0;

int handle_connection(int fd) {

    int i;
    for (i = 0; i < client_count; i++) {
        if (threads[i].state == DEAD) break;
    }

    if (i >= MAX_CLIENTS) {
        write(fd, "QUIT: too many connections:\n", 29);
        close(fd);
        return -1;
    }

    //wipe out the structure before reusing it
    bzero(&threads[i], sizeof (struct client_thread));
    threads[i].fd = fd;
    threads[i].thread_id = i;
    if (pthread_create(&threads[i].thread, NULL,
            client_thread_entry, &threads[i])) {

    }
    //connection_main(fd);
    return 0;
}

int connection_main(struct client_thread* t) {
    //printf("I have now seen %d connections so far.\n",++connection_count);
    // the server connects
    /*
        char t->line[1024];
     */

    memset(t->line, 0, 1024);
    memset(t->buffer, 0, 8192);

    snprintf(t->line, 1024, ":myserver.com 020 * :hello\n");
    //printf("the line is %s\n", t->line);
    write(t->fd, t->line, strlen(t->line));
    //puts(t->line);
    /*
        int state = 0;
        unsigned char nickname[32];
        int nicknamelength = 0;
        unsigned char username[32];
        int usernamelength = 0;
     */
    /*
        memset(t->nickname, 0, 32);
        memset(t->username, 0, 32);
        t->nickname[0] = '*';
        t->username[0] = '*';
     */
    snprintf(t->nickname, 1024, "*");
    snprintf(t->username, 1024, "*");

    /*
     * states as defined by this program
     * 0 unregistered
     * 1 password received
     * 2 nickname set
     * 3 registered (username set)
     */

    while (1) {
        t->buffer_length = 0;
        read_from_socket(t->fd, t->buffer, &t->buffer_length, 8192, 5);
        if (t->buffer_length == 0) {
            snprintf(t->line, 1024, "ERROR :Closing Link: Connection timed out (bye bye)\n");
            write(t->fd, t->line, strlen(t->line));
            close(t->fd);
            return 0;
        }
        //t->buffer[t->buffer_length - 1] = 0; //remove the end line char

        /*
        int possComLength = (int) strchr(line, ' ');
        if (possComLength != 0) {
            possComLength += 1 - (int) &line[0];
            //printf("the t->buffer_length is %d\n",possComLength);   
            //printf("the command is %.*s\n", possComLength, line);
        }
         */

        if (strncasecmp("QUIT", (char*) t->buffer, 4) == 0) {
            // client has said they are going away
            // needed to avoid SIGPIPE and the program will be killed on socket read
            snprintf(t->line, 1024, "ERROR :Closing Link: %s[%s@client.example.com] (I Quit)\n", t->nickname, t->username);
            write(t->fd, t->line, strlen(t->line));
            close(t->fd);
            return 0;
        }
        printf("the command is %s\n", t->buffer);
        if (strncasecmp("JOIN", (char*) t->buffer, 4) == 0) {
            if (t->mode == 3) {
                //@todo handle legit join here
            } else {
                snprintf(t->line, 1024, ":myserver.com 241 %s :JOIN command sent before registration\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
            }

        } else if (strncasecmp("PRIVMSG", (char*) t->buffer, 7) == 0) {
            if (t->mode == 3) {
                //@todo handle legit private message here
            } else {
                snprintf(t->line, 1024, ":myserver.com 241 %s :PRIVMSG command sent before registration\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
            }
        } else if (strncasecmp("PASS", (char*) t->buffer, 4) == 0) { // t->mode is ignored?
            if (t->mode == 0) {
                t->mode++;
                // there is no password ... continue
            }
            //@todo handle legit private message here            
        } else if (strncasecmp("NICK", (char*) t->buffer, 4) == 0) {
            /*
                        if (t->mode == 0) {
                            snprintf(t->line, 1024, ":myserver.com 241 * :NICK command sent before password (PASS *)\n");
                            write(t->fd, t->line, strlen(t->line));
                        }
             */
            t->mode += 2;
            t->nicknamelength = t->buffer_length - 7;
            memcpy(t->nickname, &t->buffer[5], t->nicknamelength);
            //printf("the t->buffer_length is %s\n", nickname);

        } else if (strncasecmp("USER", (char*) t->buffer, 4) == 0) {
            if (t->mode == 2) {
                t->mode++;
                // check username in unique
                t->usernamelength = t->buffer_length - 7;
                memcpy(t->username, &t->buffer[5], t->usernamelength);
                //send welcome message

                snprintf(t->line, 1024, ":myserver.com 001 %s :Welcome to the Internet Relay Network %s!~%s@client.myserver.com\n", t->nickname, t->nickname, t->username);
                printf(":myserver.com 001 %s :Welcome to the Internet Relay Network %s!~%s@client.myserver.com\n", t->nickname, t->nickname, t->username);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 002 %s :Your host is myserver.com, running version 1.0\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 003 %s :This server was created a few seconds ago\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 004 %s :Your host is myserver.com, running version 1.0\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 253 %s :some statistics\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 254 %s :some more statistics\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 255 %s :even some more statistics\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));

            }
            if (t->mode == 1) {
                snprintf(t->line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *)\n");
                write(t->fd, t->line, strlen(t->line));
            } else if (t->mode == 0) {
                snprintf(t->line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *) and nickname (NICK aNickName)\n");
                write(t->fd, t->line, strlen(t->line));
            }
        }
    }

    close(t->fd);
    return 0;
}

void handleBadState(int fd, char line[], int state, int expectedState) {

    if (state == 1) {
        snprintf(line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *)\n");
    } else if (state == 0) {
        snprintf(line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *) and nickname (NICK aNickName)\n");
    }
    write(fd, line, strlen(line));
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "usage: sample <tcp port>\n");
        exit(-1);
    }

    int master_socket = create_listen_socket(atoi(argv[1]));

    fcntl(master_socket, F_SETFL, fcntl(master_socket, F_GETFL, NULL)&(~O_NONBLOCK));

    while (1) {
        int client_sock = accept_incoming(master_socket);
        if (client_sock != -1) {
            // close(client_sock);
            handle_connection(client_sock);
        }
    }
}
