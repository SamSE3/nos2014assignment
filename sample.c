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

#define SERVER_GREETING ":irc.nos2014.net 020 * :Please register.\n"
#define TIMEOUT_MESSAGE "ERROR :Closing link: %s (Timeout).\n"

pthread_rwlock_t message_log_lock;

struct client_thread {
  pthread_t thread;
  int thread_id;
  int fd;

  char nickname[32];

  int state;
  time_t timeout;

  char line[1024];
  int line_len;
};

int create_listen_socket(int port)
{
  int sock = socket(AF_INET,SOCK_STREAM,0);
  if (sock==-1) return -1;

  int on=1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) == -1) {
    close(sock); return -1;
  }
  if (ioctl(sock, FIONBIO, (char *)&on) == -1) {
    close(sock); return -1;
  }
  
  /* Bind it to the next port we want to try. */
  struct sockaddr_in address;
  bzero((char *) &address, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  if (bind(sock, (struct sockaddr *) &address, sizeof(address)) == -1) {
    close(sock); return -1;
  } 

  if (listen(sock, 20) != -1) return sock;

  close(sock);
  return -1;
}

int accept_incoming(int sock)
{
  struct sockaddr addr;
  unsigned int addr_len = sizeof addr;
  int asock;
  if ((asock = accept(sock, &addr, &addr_len)) != -1) {
    return asock;
  }

  return -1;
}

int server_reply(struct client_thread *t,int n,char *m)
{
  char msg[1024];
  snprintf(msg,1024,":irc.nos2014.net %d %s :%s\n",
	   n,t->nickname,m);
  write(t->fd,msg,strlen(msg));
  return 0;
}

int user_not_registered(struct client_thread *t)
{
  char msg[1024];
  snprintf(msg,1024,":irc.nos2014.net 241 %s :You must register with USER before using this command.\n",
	   t->nickname);
  // XXX should submit quit message to shared log
  write(t->fd,msg,strlen(msg));
  return 0;
}

int process_line(struct client_thread *t,char *line)
{
  char thecommand[1024]="";
  char thefirstarg[1024]="";
  char therest[1024]="";
  char msg[1024];

  // Accept "CMD :stuff" and "CMD thing :stuff"
  if ((sscanf(line,"%[^ ] :%[^\n]",thecommand,thefirstarg)>0)
      ||(sscanf(line,"%[^ ] %[^ ] :%[^\n]",thecommand,thefirstarg,therest)>0))
    {
      // got something that looks like a command
      if (!strcasecmp(thecommand,"JOIN")) {
	if (!t->state) return user_not_registered(t);
	// join channel named in thefirstarg
      }
      else if (!strcasecmp(thecommand,"PRIVMSG")) {
	if (!t->state) return user_not_registered(t);
	// join send private message to party named in thefirstarg
      }
      else if (!strcasecmp(thecommand,"QUIT")) {
	// Quit, leaving optional quit message
	if (!thefirstarg[0]) strcpy(thefirstarg,"Goodbye");
	snprintf(msg,1024,"ERROR :Closing link %s (%s).\n",
		 t->nickname,thefirstarg);
	// XXX should submit quit message to shared log
	write(t->fd,msg,strlen(msg));
	close(t->fd);
	fflush(stderr);
	pthread_exit(0);
      }
      else {
	// unknown command
	//	fprintf(stderr,"Saw unknown command '%s'\n",thecommand);
	server_reply(t,299,"Unknown command.");
      }      
    }
  else
    {
      // got some rubbish
      fprintf(stderr,"Could not parse line\n");
    }

  return 0;
}

int parse_byte(struct client_thread *t, char c)
{
  // Parse next byte read on socket.
  // If a complete line, then parse the line
  if (c=='\n'||c=='\r') {
    if (t->line_len<0) t->line_len=0;
    if (t->line_len>1023) t->line_len=1023;
    t->line[t->line_len]=0;
    if (t->line_len>0) process_line(t,t->line);
    t->line_len=0;
  } else {
    if (t->line_len<1024) 
      t->line[t->line_len++]=c;
  }
  return 0;
}

void *client_connection(void *data)
{
  int i;
  int bytes=0;
  char buffer[8192];
  
  struct client_thread *t=data;
  t->state=0;
  t->timeout=time(0)+5;
  strcpy(t->nickname,"*");
  
  fcntl(t->fd,F_SETFL,fcntl(t->fd, F_GETFL, NULL)|O_NONBLOCK);

  int r=write(t->fd,SERVER_GREETING,strlen(SERVER_GREETING));
  if (r<1) perror("write");
  
  // Main socket reading loop
  while(1) {
    bytes=read(t->fd,(unsigned char *)buffer,sizeof(buffer));
    if (bytes>0) {
      //      fprintf(stderr,"Read %d bytes on fd %d\n",bytes,t->fd);
      t->timeout=time(0)+5;
      if (t->state) t->timeout+=55; // 60 second timeout once registered
      for(i=0;i<bytes;i++) parse_byte(t,buffer[i]);
    } else {      
      // close connection on timeout
      if (time(0)>=t->timeout) {
	write(t->fd,TIMEOUT_MESSAGE,strlen(TIMEOUT_MESSAGE));
	break;      
      } else
	usleep(50000);
    }
  }

  close(t->fd);
  // pthread_exit(0);
  return 0;
}

int main(int argc,char **argv)
{
  // because the test program shuts connections down immediately, it is quite possible for us to find a connection that has been closed before we can properly accept it.  Thus we must ignore broken pipes
  signal(SIGPIPE, SIG_IGN);

  if (argc!=2) {
  fprintf(stderr,"usage: sample <tcp port>\n");
  exit(-1);
  }
  
  int master_socket = create_listen_socket(atoi(argv[1]));
  
  if (pthread_rwlock_init(&message_log_lock,NULL))
    {
      fprintf(stderr,"Failed to create rwlock for message log.\n");
      exit(-1);
    }
  
  while(1) {
    int client_sock = accept_incoming(master_socket);
    if (client_sock!=-1) {
      // Got connection -- do something with it.
      struct client_thread *t=calloc(sizeof(struct client_thread),1);
      if (t!=NULL) {
	t->fd = client_sock;
	if (pthread_create(&t->thread, NULL, client_connection, 
			   (void*)t))
	  {
	    // Thread creation failed
	    close(client_sock);
	  }
      }
    }
  }
}
