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
#include <ctype.h>

struct client_thread {
  pthread_t thread;
  int thread_id;
  int fd;

  char nickname[32];

  int state;
  int user_command_seen;
  int user_has_registered;
  time_t timeout;

  char line[1024];
  int line_len;

  int next_message;
};

pthread_rwlock_t message_log_lock;

int read_from_socket(int sock,unsigned char *buffer,int *count,int buffer_size,
		     int timeout)
{
  fcntl(sock,F_SETFL,fcntl(sock, F_GETFL, NULL)|O_NONBLOCK); // make sure sock wont block

  int t=time(0)+timeout;
  if (*count>=buffer_size) return 0; // got some data return zero
  int r=read(sock,&buffer[*count],buffer_size-*count);
  //address of the 0th elem in the array buffer
  while(r!=0) {
    if (r>0) {
      (*count)+=r;
      break;
    }
    r=read(sock,&buffer[*count],buffer_size-*count);
    if (r==-1&&errno!=EAGAIN) { // no double error
      perror("read() returned error. Stopping reading from socket.");
      return -1;
    } else usleep(100000); // sleep 
    // timeout after a few seconds of nothing
    if (time(0)>=t) break; //check the timeout
  }
  buffer[*count]=0; //null the end of the string
  return 0;
}

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

int connection_count=0;

int handle_connection(int fd)
{
  //printf("I have now seen %d connections so far.\n",++connection_count);
  char msg[1024];
  snprintf(msg,1024,":myserver.com 020 * :hello\n");
  write(fd,msg,strlen(msg));
  unsigned char buffer[8192];
  int length=0;
  int registered=0;


  while(1){
    length=0;
    read_from_socket(fd,buffer,&length,8192,5);
    if(length==0){
      snprintf(msg,1024,"ERROR :Closing Link: Connection timed out (bye bye)\n");
      write(fd,msg,strlen(msg));
      close(fd);
      return 0;
    }
    
    buffer[length]=0;
    //printf("the test program sent '%s",buffer);
  
    char channel[8192];
    int  r=sscanf((char*) buffer,"JOIN %s", channel);
    if(r==1){
      if(!registered){
	snprintf(msg,1024,":myserver.com 241 * :JOIN command sent before registration\n");
	write(fd,msg,strlen(msg));
      }
    }

    if(!strncasecmp("PRIVMSG",(char*) buffer,7)){
      snprintf(msg,1024,":myserver.com 241 * :PRIVMSG command sent before registration\n");
      write(fd,msg,strlen(msg));
    }

    if(!strncasecmp("QUIT",(char*) buffer,4)){
      // client has said they are going away
      // needed to avoid SIGPIPE and the program will be killed on socket read
      snprintf(msg,1024,"ERROR :Closing Link: Connection timed out (bye bye)\n");
      write(fd,msg,strlen(msg));
      close(fd);
      return 0;
    }
  }


  close(fd);
  return 0;
}

int main(int argc,char **argv)
{
  signal(SIGPIPE, SIG_IGN);

  if (argc!=2) {
  fprintf(stderr,"usage: sample <tcp port>\n");
  exit(-1);
  }
  
  int master_socket = create_listen_socket(atoi(argv[1]));
  
  fcntl(master_socket,F_SETFL,fcntl(master_socket, F_GETFL, NULL)&(~O_NONBLOCK));  
  
  while(1) {
    int client_sock = accept_incoming(master_socket);
    if (client_sock!=-1) {
      // close(client_sock);
      handle_connection(client_sock);
    }
  }
}
