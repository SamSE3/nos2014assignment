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

/**
 * a structure for each thread
 */
struct client_thread {
    pthread_t thread;
    int thread_id;
    int fd;

    int nicknamelength;
    char nickname[32];
    int usernamelength;
    char username[32];

    int state;

    // the mode of the connection, modes are:
    // 0 unregistered
    // 1 password supplied // skipped here?
    // 2 nickname supplied
    // 3 username supplied ... registered
    int mode;
    time_t timeout;

    char line[1024];
    int line_length;

    unsigned char buffer[8192];
    int buffer_length;


    int next_message;
};

//defines the maximum client threads
#define MAX_CLIENTS 50

//define the dead and alive thread states ... no longer relied on
#define DEAD 1
#define ALIVE 2

// create an array of client threads
struct client_thread threads[MAX_CLIENTS];

// a stack to hold the unused threads
int aval_thread_stack[MAX_CLIENTS];
int thread_stack_size = 0; // also the connection count

// a lock for the shared thread stack
pthread_rwlock_t aval_thread_stack_lock;

// a lock for messages?
pthread_rwlock_t message_log_lock;

int connection_main(struct client_thread* t);

/**
 * 
 * @param sock
 * @param buffer
 * @param count
 * @param buffer_size
 * @param timeout
 * @return 
 */
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

/**
 * 
 * @param port
 * @return 
 */
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

    // Bind it to the next port we want to try. 
    struct sockaddr_in address;
    bzero((char *) &address, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *) &address, sizeof (address)) == -1) {
        // bind failed ... close the socket
        close(sock);
        return -1;
    }

    // if can listen to the socket return it
    if (listen(sock, 20) != -1) {
        return sock;
    }
    //otherwise close the socket
    close(sock);
    return -1;
}

/**
 * try to accept an in coming socket
 * @param sock the socket to try to accept
 * @return file descriptor of the accepted socket or -1 if an error occurred
 */
int accept_incoming(int sock) {
    struct sockaddr addr;
    unsigned int addr_len = sizeof addr;
    int asock;
    if ((asock = accept(sock, &addr, &addr_len)) != -1) {
        return asock;
    }

    return -1;
}

/**
 * populate the thread stack with MAX_CLIENT thread_ids
 */
void populate_stack() {
    for (thread_stack_size = 0; thread_stack_size < MAX_CLIENTS; thread_stack_size++) {//0-49
        aval_thread_stack[thread_stack_size] = MAX_CLIENTS - 1 - thread_stack_size; //49-0
    }
}

/**
 * try to pop the first available thread of the available thread stack
 * @return an available thread_id or -1 if none are available
 */
int trypop_stack() {
    int return_val;
    pthread_rwlock_wrlock(&aval_thread_stack_lock);
    if (thread_stack_size == 0) { // all in use nothing to pop
        return_val = -1;
    } else { // pop the top value
        return_val = aval_thread_stack[thread_stack_size--];
    }
    pthread_rwlock_unlock(&aval_thread_stack_lock);
    return return_val;
}

/**
 * pushes a released thread_id onto the available thread stack
 * @param thread_id, the thread id to add to the stack 
 * @return 0 if thread id was added to the stack
 */
int push_stack(int thread_id) {
    int return_val;
    pthread_rwlock_wrlock(&aval_thread_stack_lock);
    //if (thread_stack_size == MAX_CLIENTS) { // stack is full? impossible adding to many to the stack?
    //    return_val = -1;
    //} else { //set the top value in the stack
    aval_thread_stack[thread_stack_size++] = thread_id;
    return_val = 0;
    //}
    pthread_rwlock_unlock(&aval_thread_stack_lock);
    return return_val;
}

//int connection_main(int fd);

/**
 * the entry point on the creation of a client thread
 * @param arg, the client thread structure
 * @return null on exit of the thread
 */
void* client_thread_entry(void * arg) {
    struct client_thread *t = arg;

    //run the thread
    t->state = DEAD; // mark it as dead ? ... doesn't really matter
    push_stack(t->thread_id); // push the thread id back onto the stack
    connection_main(t); // 
    return NULL;
}

/**
 * try to turn the connection into a connection thread
 * @param fd the file descriptor of the accepted socket
 * @return 0 if the connection was successfully created or -1 if it was not
 */
int handle_connection(int fd) {

    // get an available thread
    int thread_no = trypop_stack();

    // no threads were available ... so quit
    if (thread_no == -1) {
        write(fd, "QUIT: too many connections:\n", 29);
        close(fd);
        return -1;
    }

    //wipe out the structure before reusing it    
    bzero(&threads[thread_no], sizeof (struct client_thread));
    // set the threads file description
    threads[thread_no].fd = fd;
    threads[thread_no].thread_id = thread_no;
    //create the thread
    pthread_create(&threads[thread_no].thread, NULL,
            client_thread_entry, &threads[thread_no])

    return 0;
}

/**
 * handles the client thread connections request messages and responses 
 * @param t the client thread structure to handle
 * @return 0 on the closer of a connection
 */
int connection_main(struct client_thread* t) {
    //printf("I have now seen %d connections so far.\n",++connection_count);
    // the server connects
    /*
        char t->line[1024];
     */

    snprintf(t->line, 1024, ":myserver.com 020 * :hello\n"); // wright output message to the line
    //printf("the line is %s\n", t->line);
    write(t->fd, t->line, strlen(t->line)); // write the line to the handler
    //puts(t->line);
    /*
        int state = 0;
        unsigned char nickname[32];
        int nicknamelength = 0;
        unsigned char username[32];
        int usernamelength = 0;
     */
    /*
        memset(t->nickname, '\0', 32);
        memset(t->username, '\0', 32);
        t->nickname[0] = '*';
        t->username[0] = '*';
     */
    snprintf(t->nickname, 1024, "*");
    //printf("output %s", t->nickname);
    snprintf(t->username, 1024, "*");
    //memset(t->line, '\0', 1024);
    /*
     * states as defined by this program
     * 0 unregistered
     * 1 password received
     * 2 nickname set
     * 3 registered (username set)
     */

    while (1) {
        t->buffer_length = 0;
        //t->line_length = 0;
        memset(t->line, '\0', 1024);
        //read the response from the socket
        read_from_socket(t->fd, t->buffer, &t->buffer_length, 8192, 5);
        //if an empty response (nothing in the buffer) reply with connection timed out
        if (t->buffer_length == 0) {
            snprintf(t->line, 1024, "ERROR :Closing Link: Connection timed out (bye bye)\n");
            write(t->fd, t->line, strlen(t->line));
            close(t->fd); //
            return 0;
        }
        t->buffer[t->buffer_length - 1] = 0; //remove the end line char

        if (strncasecmp("QUIT", (char*) t->buffer, 4) == 0) {
            // client has said they are going away
            // needed to avoid SIGPIPE and the program will be killed on socket read
            snprintf(t->line, 1024, "ERROR :Closing Link: %s[%s@client.example.com] (I Quit)\n", t->nickname, t->username);
            write(t->fd, t->line, strlen(t->line));
            close(t->fd);
            return 0;
        }

        //printf("the command is %s with length %d\n", t->buffer, strlen(t->buffer));

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
                printf("lineout :%s\n", t->line);
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

/*
void handleBadState(int fd, char line[], int state, int expectedState) {
    if (state == 1) {
        snprintf(line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *)\n");
    } else if (state == 0) {
        snprintf(line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *) and nickname (NICK aNickName)\n");
    }
    write(fd, line, strlen(line));
}
 */

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "usage: sample <tcp port>\n");
        exit(-1);
    }

    int master_socket = create_listen_socket(atoi(argv[1]));

    //set the file status flags, checking for blocking
    fcntl(master_socket, F_SETFL, fcntl(master_socket, F_GETFL, NULL)&(~O_NONBLOCK));

    // initialise the available thread stack lock
    pthread_rwlock_init(&aval_thread_stack_lock, NULL);
    // populate the stack with available threads 
    populate_stack();
    
    while (1) {
        // try to accept an incoming connection
        int client_sock = accept_incoming(master_socket);
        // if there is a connection ... handle it
        if (client_sock != -1) {
            // close(client_sock);
            handle_connection(client_sock);
        }
    }
    //destroy the available thread stack lock
    pthread_rwlock_destroy(&aval_thread_stack_lock);

    return 0;
}
