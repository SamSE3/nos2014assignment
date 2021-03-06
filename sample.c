/*
  Solution for NOS 2014 assignment: implement a simple multi-threaded 
  IRC-like chat service.
 
  (C) Samuel Deane and Paul Gardner-Stephen 2014.
  
 * main code path:
 * Main ... Populates the id stack (populate_stack), listens for connections 
 *          (create_listen_socket), accepts incoming connections 
 *          (accept_incoming) and if accepted calls handle_connection
 * handle_connection ... Gets an unused thread id(trypop_stack) and creates a 
 *                       thread of that id based on the client_thread struct.  
 *                       The thread on creation calls client_thread_entry
 * client_thread_entry ... Declares the thread alive and calls connection_main
 * connection_main ... Handles the requests of the client by reading off the 
 *                     socket (read_from_socket), performing operations such as
 *                     logging-in, sending messages, joining groups and handling
 *                     message timeouts. Which it does by getting the recipient 
 *                     client thread (et_client_thread_by_nickname) and writing 
 *                     to its message buffer and setting its has_message to 
 *                     1. The recipient thread knowing it has a message, sends 
 *                     out the message in its own time, though currently this 
 *                     process is undefined for multiple simultaneous messages 
 *                     as the buffer could be overwritten before the message is 
 *                     sent. On a QUIT message or timeout closes the connection.
 * client_thread_entry ... Declares the thread dead
 * handle_connection ... Places the id back onto the stack (push_stack) for reuse 
 * Main ... Closes the listening socket
  
 * problems, possible issues, limitations etc. 
 * - Does not handle global messages
 * - Multiple threads sending private messages to a single thread at the same 
 *   time is undefined as the current system relies on each thread having its own
 *   unsynchronised message buffer with a delayed write output.
 *      this can be solved by implementing a linked list of messages
 * - Legitimate joins are not handled 
 * - Recipient clients are found through iterating over an array (avg O(n/2)) 
 *   whereas a synchronised hashtable will have better performance (avg O(1)).
 *   the thread_client is of a set size and will thus be wasteful
 * - No check is performed to see if a clients nickname is unique
 * - Private messages not preceeded by a pong have an extra byte than expected.
 *   So a hack is used to set the method length to 1 less on such a condition
 * - Commenting is deliberately excessive for demonstration of understanding
 
 * Features
 * - Uses a stack to hold available client thread ids
 * - Passes all tests of test.c 
 
  Contains sections directly taken from test.c, (C) Paul Gardner-Stephen 
  made available under the GNU General Public License.
 
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

//IRC references
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
 * a structure for each message node, 
 */
struct node {
  char message[1024];
  int messagelength;
  struct node *n1ext;
};

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
    // 3 username supplied == registered
    int mode;

    // the timeout length of the structure
    time_t timeout;

    char line[1024];
    int line_length;

    unsigned char buffer[8192];
    int buffer_length;

    int has_next_message;
    int messagelength;
    char message[1024];
    struct node *first_message_node;
    struct node *next_message_node; // pointer to the next message node to add
    pthread_rwlock_t next_message_node_lock; // can only add one message to the linked
    // list at one time, perhaps create a thread to wait if locked? so it dosen't
    // hold up the thread that sent the message.
    // non blocking i/o does not wait on a blocked thread
    
};

//defines the maximum client threads
#define MAX_CLIENTS 100

//define the dead and alive thread states (no longer relied on) and some timeouts
#define DEAD 1
#define ALIVE 2
#define REG_TIMEOUT 120
#define NICK_TIMEOUT 30 //else 5

// create an array of client threads
struct client_thread threads[MAX_CLIENTS];

// a stack to hold the unused threads
int aval_thread_stack[MAX_CLIENTS];
int aval_thread_stack_size = 0; // also the connection count as MAX_CLIENTS-aval_thread_stack_size

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
 * @param has_next_message, pointer used to stop the read as it has a write to perform 
 * (probably would give better performance is handled here but would mean re-writing the give function ... so no)
 * @return 0 if data is returned to the buffer otherwise -1 as a read error occured
 */
int read_from_socket(int sock, unsigned char *buffer, int *count, int buffer_size, int timeout, int* has_next_message) {

    //set the socket flags to non blocking 
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, NULL) | O_NONBLOCK);

    int t = time(0) + timeout; // set the timeout time (timeout seconds from now)
    if (*count >= buffer_size) { // got some data return zero
        return 0;
    }

    // else read and continue reading from the socket until data is found
    int r = read(sock, &buffer[*count], buffer_size - *count);
    //address of the 0th elem in the array buffer
    while (r != 0) { // while no data continue to try and read
        if (r > 0) {
            (*count) += r;
            break;
        }
        r = read(sock, &buffer[*count], buffer_size - *count);
        if (r == -1 && errno != EAGAIN) { // no double error
            perror("read() returned error. Stopping reading from socket.");
            return -1;
        } else if (*has_next_message) {
            break;
        } else { // sleep 
            usleep(100000);
        }
        // timeout after a few seconds of nothing
        if (*has_next_message || time(0) >= t) {
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
 * Try to accept an incoming socket
 * @param sock, the socket to try to accept
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
 * find which thread structure the nickname belongs to
 * @param nickname, a nickname of the user to find
 * @param nicknamelength, the length of the nickname (speeds up searching)
 * @return a pointer to the client thread or NULL if not found
 */
struct client_thread* get_client_thread_by_nickname(char* nickname, int nicknamelength) {

    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        struct client_thread *t = &threads[i];
        if (t->mode == 3) {//only check registered threads
            //printf("ALIVETHREAD username %s with tlen %d and want %.*s with unlen %d\n", t->username, t->usernamelength, usernamelength, username, usernamelength);
            if (t->nicknamelength == nicknamelength) {
                if (strncmp(nickname, (char *) t->nickname, t->nicknamelength) == 0) {
                    return t;
                }
            }
        } else {
            //printf("DEADTHREAD username %s with tlen %d and want %.*s with unlen %d\n", t->username, t->usernamelength, usernamelength, username, usernamelength);
        }
    }
    return NULL;
}

/**
 * Populate the thread stack with MAX_CLIENT thread_ids
 */
void populate_stack() {
    for (aval_thread_stack_size = 0; aval_thread_stack_size < MAX_CLIENTS; aval_thread_stack_size++) {//0-MAX_CLIENTS-1
        aval_thread_stack[aval_thread_stack_size] = MAX_CLIENTS - 1 - aval_thread_stack_size; //MAX_CLIENTS-1-0 999 998 ... 1 0 //the top of the stake will be 
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
        aval_thread_stack_size--;
        return_val = aval_thread_stack[aval_thread_stack_size];
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

/**
 * Handle the client thread connections request messages and responses 
 * @param t the client thread structure to handle
 * @return 0 the close of a connection
 */
int connection_main(struct client_thread* t) {
    //printf("I have now seen %d connections so far.\n",++connection_count);

    // initial setup
    snprintf(t->nickname, 1024, "*");
    snprintf(t->username, 1024, "*");
    t->nicknamelength = 1;
    t->usernamelength = 1;
    t->mode = 1; // make sure the mode (is set to unregistered  NICK or USER) ... pass is ignored ... so 1
    t->timeout = 5; // give 5 seconds to live
    t->has_next_message = 0; //make sure there are no messages to be sent yet ... probably not required

    snprintf(t->line, 28, ":myserver.com 020 * :hello\n\r"); // write output message to the line
    write(t->fd, t->line, strlen(t->line)); // write the line to the file descriptor

    while (1) {
        //printf(" fd for id %d is %d\n", t->thread_id, t->fd);
        t->buffer_length = 0;

        //read the response from the socket, waiting until input or timeout
        read_from_socket(t->fd, t->buffer, &t->buffer_length, 8192, t->timeout, &t->has_next_message);
        char * buffer = (char*) t->buffer;
        int bufferlength = t->buffer_length;

        if (t->has_next_message) {
            write(t->fd, t->message, strlen(t->message));
            t->has_next_message = 0;
            //printf("reply printed '%.*s'\n", strlen(t->message), t->message);
            if (t->buffer_length == 0) { // continue reading
                continue;
            }
        } else if (t->buffer_length == 0) { // nothing in the buffer ... must have timed out
            snprintf(t->line, 1024, "ERROR :Closing Link: Connection timed out (bye bye)\n\r");
            write(t->fd, t->line, strlen(t->line));
            close(t->fd); //
            return 0;
        }

        //remove the end line char, couldn't do it if the buffer was empty
        t->buffer[t->buffer_length - 1] = 0;
        //t->buffer[t->buffer_length - 2] = 0;

        if (strncasecmp("QUIT", buffer, 4) == 0) {
            // client has said they are going away
            // needed to avoid SIGPIPE and the program will be killed on socket read
            snprintf(t->line, 1024, "ERROR :Closing Link: %s[%s@client.example.com] (I Quit)\n\r", t->nickname, t->username);
            write(t->fd, t->line, strlen(t->line));
            close(t->fd);
            return 0;
        } else if (strncasecmp("PONG", buffer, 4) == 0) {
            if (t->buffer_length == 6) {
                continue;
            } else { // adjust for any pong
                buffer = buffer + 6;
                bufferlength -= 6;
                //... continue to process
            }
            //keep alive message received ... do nothing
        }
        if (strncasecmp("JOIN", buffer, 4) == 0) {
            if (t->mode == 3) {
                //@todo handle legit join here
                // of form JOIN #twilight_zone
            } else {
                snprintf(t->line, 1024, ":myserver.com 241 %s :JOIN command sent before registration\n\r", t->nickname);
                write(t->fd, t->line, strlen(t->line));
            }
        } else if (strncasecmp("PRIVMSG", buffer, 7) == 0) {
            if (t->mode == 3) {

                int unstart = (int) buffer + 7 + 1; // address + 8
                int nicknamelength = (int) strchr((char*) unstart, ' ') - unstart; //length of the first word post space

                int mstart = unstart + nicknamelength + 1 + 1; //skip over the username, a space and the colon
                int messagelength = bufferlength - nicknamelength - 12; //get the remaining buffer size (start includes the buffer address)
                /*
                                printf("buffer is %s\n\
                nickname to send to %.*s, length is %d\n\
                message to send %.*s, length is %d\n"
                                        , (char*) buffer
                                        , nicknamelength, (char*) unstart, nicknamelength
                                        , messagelength, (char*) mstart, messagelength);
                 */

                struct client_thread* ct = get_client_thread_by_nickname((char*) unstart, nicknamelength);
                if (ct == NULL) {
                    //printf("got null?\n");
                    snprintf(t->line, 1024, ":myserver.com 241 %s :PRIVMSG unknown username %.*s \n\r", t->nickname, nicknamelength, (char*) unstart);
                    write(t->fd, t->line, strlen(t->line));
                } else {
                    //printf("nickname is %.*s of length %d\n", ct->nicknamelength, ct->nickname, ct->nicknamelength);
                    messagelength = 22 + ct->nicknamelength + 2 + messagelength + ((strncasecmp("PONG", (char*) t-> buffer, 4) == 0) ? 3 : 2);
                    snprintf(ct->message, messagelength, ":myserver.com PRIVMSG %s :%s\n\r", ct->nickname, (char*) mstart);
                    ct->messagelength = messagelength; // copy its length                    
                    ct->has_next_message = 1; //say it has to send a message
                }
            } else { // not a registered user
                snprintf(t->line, 1024, ":myserver.com 241 %s :PRIVMSG command sent before registration\n\r", t->nickname);
                write(t->fd, t->line, strlen(t->line));
            }
        } else if (strncasecmp("NICK", buffer, 4) == 0) {
            /* // not required? as no PASS message
                        if (t->mode == 0) {
                            snprintf(t->line, 1024, ":myserver.com 241 * :NICK command sent before password (PASS *)\n");
                            write(t->fd, t->line, strlen(t->line));
                        }
             */
            t->mode = 2;
            t->timeout = NICK_TIMEOUT;
            // copy the nickname off the buffer to the client_thread struct
            t->nicknamelength = t->buffer_length - 7;
            memcpy(t->nickname, t->buffer + 5, t->nicknamelength);
            //printf("nickname for thread %d set to %s and should be %s\n", t->thread_id, t->nickname, t->buffer + 5);
        } else if (strncasecmp("USER", buffer, 4) == 0) {
            if (t->mode == 2) {
                t->mode = 3;
                t->timeout = REG_TIMEOUT;
                // @todo check username in unique ... add to list
                // copy the nickname off the buffer to the client_thread struct
                t->usernamelength = t->buffer_length - 7;
                memcpy(t->username, t->buffer + 5, t->usernamelength);
                //printf("username for thread %d set to %s and should be %s\n", t->thread_id, t->username, t->buffer + 5);
                //send welcome messages
                snprintf(t->line, 1024,
                        ":myserver.com 001 %s :Welcome to the Internet Relay Network %s!~%s@client.myserver.com\n\
:myserver.com 002 %s :Your host is myserver.com, running version 1.0\n\
:myserver.com 003 %s :This server was created a few seconds ago\n\
:myserver.com 004 %s :Your host is myserver.com, running version 1.0\n\
:myserver.com 253 %s :I have %d users\n\
:myserver.com 254 %s :I have %d connections %d\n\
:myserver.com 255 %s :even some more statistics\n\r"
                        , t->nickname, t->nickname, t->username
                        , t->nickname
                        , t->nickname
                        , t->nickname
                        , t->nickname, reg_users
                        , t->nickname, aval_thread_stack_size, MAX_CLIENTS
                        , t->nickname
                        );
                write(t->fd, t->line, strlen(t->line));
            } else if (t->mode == 1) { // password set but not nickname
                snprintf(t->line, 1024, ":myserver.com 241 * :USER command sent before nickname (NICK aNickName)\n\r");
                write(t->fd, t->line, strlen(t->line));
            } else if (t->mode == 0) { // password set but not nickname
                snprintf(t->line, 1024, ":myserver.com 241 * :USER command sent before password (PASS *) and nickname (NICK aNickName)\n\r");
                write(t->fd, t->line, strlen(t->line));
            }
        } else if (strncasecmp("PASS", buffer, 4) == 0) {
            if (t->mode == 0) {
                t->mode++;
                t->timeout = 30;
                // there is no password ... continue
            }
            // already logged in ... do nothing        
        } else {
            printf("some other message received: %s", t->buffer);
            //@todo handle unknown message
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
    int thread_id = trypop_stack();

    // check if got a thread id
    if (thread_id == -1) {
        // couldn't get a thread id
        write(fd, "QUIT: too many connections:\n", 29);
        close(fd);
        return -1;
    } // else got a thread id

    //wipe out the structure before reusing it    
    bzero(&threads[thread_id], sizeof (struct client_thread));
    // set the threads file description & id
    threads[thread_id].fd = fd;
    threads[thread_id].thread_id = thread_id;
    //create the thread
    pthread_create(&threads[thread_id].thread, NULL,
            client_thread_entry, &threads[thread_id]);

    return 0;
}

int main(int argc, char **argv) {
    // ignore sigpipe errors such as writing to a closed pipe
    signal(SIGPIPE, SIG_IGN); 

    // check that has 2 and only two input arguments
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
        } else {
            usleep(10000);   
        }
        //close(client_sock);        
    }

    //destroy the available thread stack lock
    pthread_rwlock_destroy(&aval_thread_stack_lock);

    return 0;
}
