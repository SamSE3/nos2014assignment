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

    // the timeout length of the structure ... not required yet all are 5seconds
    time_t timeout;

    char line[1024];
    int line_length;

    unsigned char buffer[8192];
    int buffer_length;

    int next_message;
};

//defines the maximum client threads
#define MAX_CLIENTS 5000

//define the dead and alive thread states ... no longer relied on
#define DEAD 1
#define ALIVE 2

// create an array of client threads
struct client_thread threads[MAX_CLIENTS];

// a stack to hold the unused threads
int aval_thread_stack[MAX_CLIENTS];
int aval_thread_stack_size = 0; // also the connection count

// a lock for the shared thread stack
pthread_rwlock_t aval_thread_stack_lock;
// a lock for the message
pthread_rwlock_t message_log_lock;

// the number of registered users 
int reg_users = 0; //not used will make atomic

/**
 * Attempt to read data from a socket (sock) into a buffer (buffer)
 * @param sock, the socket to read from
 * @param buffer, pointer to a buffer to write data into
 * @param count, pointer to the length of the buffer
 * @param buffer_size, the total size or space avaliable for the buffer
 * @param timeout, the amount of time the socket can be read whilst idle
 * @return 0 if data is returned to the buffer otherwise -1 as a read error occured
 */
int read_from_socket(int sock, unsigned char *buffer, int *count, int buffer_size, int timeout) {

    //set the socket flags to true if they already are or if not blocking i.e. set flags to non blocking 
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, NULL) | O_NONBLOCK); 

    int t = time(0) + timeout; // set the timeout time (timeout seconds from now)
    if (*count >= buffer_size) { // got some data return zero
        return 0;
    }

    // else read and continue reading from the socket util data is found
    int r = read(sock, &buffer[*count], buffer_size - *count);
    //address of the 0th elem in the array buffer
    while (r != 0) {
        if (r > 0) {
            (*count) += r;
            break;
        }
        r = read(sock, &buffer[*count], buffer_size - *count);
        if (r == -1 && errno != EAGAIN) { // no double error
            perror("read() returned error. Stopping reading from socket.");
            return -1;
        } else { // sleep 
            usleep(100000);
        }
        // timeout after a few seconds of nothing
        if (time(0) >= t) {
            break;
        } //check the timeout
    }
    buffer[*count] = 0; //null the end of the string
    return 0;
}

/**
 * create a POSIX socket (a type of Berkeley socket) to listen in on a particular port
 * @param port, the number of the port
 * @return -1 if can't create a socket, can't set re-use addresses, 
 *  can't set the file descriptor to nonblocking I/O, binding failed or can't listen to it
 */
int create_listen_socket(int port) {
    //open a socket stream
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        return -1;
    }

    int on = 1;
    // try to set the sockets's options at the SOL_SOCKET api level so that 
    // address like 0.0.0.0:21 and 192.168.0.1:21 are not the same (0's not local wildcards?)
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof (on)) == -1) {
        close(sock);
        return -1;
    }

    // try to set the file descriptor to nonblocking I/O
    if (ioctl(sock, FIONBIO, (char *) &on) == -1) {
        close(sock);
        return -1;
    }

    // Bind it to the next port we want to try. 
    struct sockaddr_in address;
    //zero it
    bzero((char *) &address, sizeof (address));
    address.sin_family = AF_INET; // set to use AF_INET internet protocol address
    address.sin_addr.s_addr = INADDR_ANY; // receive all incomming packets
    address.sin_port = htons(port); // set the port to the specified port in network byte order. 

    if (bind(sock, (struct sockaddr *) &address, sizeof (address)) == -1) {
        // bind failed ... close the socket
        close(sock);
        return -1;
    }

    if (listen(sock, 20) != -1) {
        // can listen to the socket ... return it
        return sock;
    }
    //otherwise close the socket
    close(sock);
    return -1;
}

/**
 * Try to accept an in coming socket
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
 * Populate the thread stack with MAX_CLIENT thread_ids
 */
void populate_stack() {
    for (aval_thread_stack_size = 0; aval_thread_stack_size < MAX_CLIENTS; aval_thread_stack_size++) {//0-MAX_CLIENTS-1
        aval_thread_stack[aval_thread_stack_size] = MAX_CLIENTS - 1 - aval_thread_stack_size; //MAX_CLIENTS-1-0 999 998 ... 1 0
    }
    printf("aval_thread_stack_size is now %d\n", aval_thread_stack_size); 
}

/**
 * Try to pop the first available thread of the available thread stack
 * @return an available thread_id or -1 if none are available
 */
int trypop_stack() {
    int return_val;
    pthread_rwlock_wrlock(&aval_thread_stack_lock);
    if (aval_thread_stack_size == 0) { // all in use nothing to pop
        return_val = -1;
    } else { // pop the top value
        return_val = aval_thread_stack[aval_thread_stack_size];
        aval_thread_stack_size--;
    }
    pthread_rwlock_unlock(&aval_thread_stack_lock);
    return return_val;
}

/**
 * Pushes a released thread_id onto the available thread stack
 * @param thread_id, the thread id to add to the stack 
 * @return 0 if thread id was added to the stack
 */
int push_stack(int thread_id) {
    pthread_rwlock_wrlock(&aval_thread_stack_lock);    
    aval_thread_stack[aval_thread_stack_size] = thread_id;
    aval_thread_stack_size++;
    pthread_rwlock_unlock(&aval_thread_stack_lock);
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

/**
 * Handle the client thread connections request messages and responses 
 * @param t the client thread structure to handle
 * @return 0 the close of a connection
 */
int connection_main(struct client_thread* t) {
    //printf("I have now seen %d connections so far.\n",++connection_count);

    snprintf(t->line, 1024, ":myserver.com 020 * :hello\n"); // wright output message to the line
    write(t->fd, t->line, strlen(t->line)); // write the line to the file descriptor
    /*
     * // old pre-threaded stuff
        int state = 0;
        unsigned char nickname[32];
        int nicknamelength = 0;
        unsigned char username[32];
        int usernamelength = 0;
        memset(t->nickname, '\0', 32);
        memset(t->username, '\0', 32);
        t->nickname[0] = '*';
        t->username[0] = '*';
     */
    snprintf(t->nickname, 1024, "*");
    snprintf(t->username, 1024, "*");

    while (1) {
        t->buffer_length = 0;
        //t->line_length = 0;
        //memset(t->line, '\0', 1024);
        //read the response from the socket
        read_from_socket(t->fd, t->buffer, &t->buffer_length, 8192, 5);
        //if an empty response (nothing in the buffer) reply with connection timed out
        if (t->buffer_length == 0) {
            snprintf(t->line, 1024, "ERROR :Closing Link: Connection timed out (bye bye)\n");
            write(t->fd, t->line, strlen(t->line));
            close(t->fd); //
            return 0;
        }

        //remove the end line char, couldn't do it if the buffer was empty
        t->buffer[t->buffer_length - 1] = 0;

        if (strncasecmp("QUIT", (char*) t->buffer, 4) == 0) {
            // client has said they are going away
            // needed to avoid SIGPIPE and the program will be killed on socket read
            snprintf(t->line, 1024, "ERROR :Closing Link: %s[%s@client.example.com] (I Quit)\n", t->nickname, t->username);
            write(t->fd, t->line, strlen(t->line));
            close(t->fd);
            return 0;
        } else if (strncasecmp("JOIN", (char*) t->buffer, 4) == 0) {
            if (t->mode == 3) {
                //@todo handle legit join here
            } else {
                snprintf(t->line, 1024, ":myserver.com 241 %s :JOIN command sent before registration\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
            }
        } else if (strncasecmp("PRIVMSG", (char*) t->buffer, 7) == 0) {
            if (t->mode == 3) {
                //@todo handle legit private message here
                // of form JOIN #twilight_zone
            } else {
                snprintf(t->line, 1024, ":myserver.com 241 %s :PRIVMSG command sent before registration\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
            }
        } else if (strncasecmp("NICK", (char*) t->buffer, 4) == 0) {
            /*
                        if (t->mode == 0) {
                            snprintf(t->line, 1024, ":myserver.com 241 * :NICK command sent before password (PASS *)\n");
                            write(t->fd, t->line, strlen(t->line));
                        }
             */
            t->mode += 2;
            // copy the nickname off the buffer to the client_thread struct
            t->nicknamelength = t->buffer_length - 7;
            memcpy(t->nickname, &t->buffer[5], t->nicknamelength);

        } else if (strncasecmp("USER", (char*) t->buffer, 4) == 0) {
            if (t->mode == 2) {
                t->mode++;
                // @todo check username in unique ... add to list
                // copy the nickname off the buffer to the client_thread struct
                t->usernamelength = t->buffer_length - 7;
                memcpy(t->username, &t->buffer[5], t->usernamelength);

                //send welcome messages
                snprintf(t->line, 1024, ":myserver.com 001 %s :Welcome to the Internet Relay Network %s!~%s@client.myserver.com\n", t->nickname, t->nickname, t->username);
                //printf("lineout :%s\n", t->line);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 002 %s :Your host is myserver.com, running version 1.0\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 003 %s :This server was created a few seconds ago\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 004 %s :Your host is myserver.com, running version 1.0\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 253 %s :I have %d users\n", t->nickname, reg_users);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 254 %s :I have %d connections %d\n", t->nickname, aval_thread_stack_size, MAX_CLIENTS);
                write(t->fd, t->line, strlen(t->line));
                snprintf(t->line, 1024, ":myserver.com 255 %s :even some more statistics\n", t->nickname);
                write(t->fd, t->line, strlen(t->line));
                // :MyNickname MODE MyNickname :+i also?                
            } else if (t->mode == 1) {
                snprintf(t->line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *)\n");
                write(t->fd, t->line, strlen(t->line));
            } else if (t->mode == 0) {
                snprintf(t->line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *) and nickname (NICK aNickName)\n");
                write(t->fd, t->line, strlen(t->line));
            }
            // already set user name ... do nothing
        } else if (strncasecmp("PASS", (char*) t->buffer, 4) == 0) {
            if (t->mode == 0) {
                t->mode++;
                // there is no password ... continue
            }
            // already logged in ... do nothing        
        } else {
            printf("some other message received: %s", t->buffer);
            //@todo handle message
        }
    }

    // unreachable but here anyway
    close(t->fd);
    return 0;
}

/**
 * The entry point on the creation of a client thread
 * @param arg, the client thread structure
 * @return null on exit of the thread
 */
void* client_thread_entry(void * arg) {
    struct client_thread *t = arg;

    t->state = ALIVE; // mark it as alive ? ... doesn't really matter        
    connection_main(t); // interact with the thread
    t->state = DEAD; // mark it as dead ? ... doesn't really matter   
    push_stack(t->thread_id); // push the thread id back onto the available thread ids stack

    return NULL;
}

/**
 * Try to turn the connection into a connection thread
 * @param fd the file descriptor of the accepted socket
 * @return 0 if the connection was successfully created or -1 if it was not
 */
int handle_connection(int fd) {

    // get an available thread
    int thread_no = trypop_stack();

    // check if got a thread id
    if (thread_no == -1) {
        // couldn't get a thread id
        write(fd, "QUIT: too many connections:\n", 29);
        close(fd);
        return -1;
    }

    //wipe out the structure before reusing it    
    bzero(&threads[thread_no], sizeof (struct client_thread));
    // set the threads file description & id
    threads[thread_no].fd = fd;
    threads[thread_no].thread_id = thread_no;
    //create the thread
    pthread_create(&threads[thread_no].thread, NULL,
            client_thread_entry, &threads[thread_no]);

    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "usage: sample <tcp port>\n");
        exit(-1);
    }

    // set the master listening socket
    int master_socket = create_listen_socket(atoi(argv[1]));

    //set the file status flags, checking for blocking
    fcntl(master_socket, F_SETFL, fcntl(master_socket, F_GETFL, NULL)&(~O_NONBLOCK));

    // initialise the available thread stack lock
    pthread_rwlock_init(&aval_thread_stack_lock, NULL);

    // populate the available thread id stack with ids
    populate_stack();

    while (1) {
        // try to accept an incoming connection
        int client_sock = accept_incoming(master_socket);
        // if there is a connection ... handle it
        if (client_sock != -1) {
            handle_connection(client_sock);
        }
        // close(client_sock);
    }
    
    //destroy the available thread stack lock
    pthread_rwlock_destroy(&aval_thread_stack_lock);

    return 0;
}
