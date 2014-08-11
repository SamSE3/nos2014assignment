/*
  Test program for NOS 2014 assignment: implement a simple multi-threaded 
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

pid_t student_pid=-1;
int student_port;
int success=0;

char *gradeOf(int score)
{
  if (score<50) return "F";
  if (score<65) return "P";
  if (score<75) return "CR";
  if (score<85) return "DN";
  return "HD";
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

int connect_to_port(int port)
{
  struct hostent *hostent;
  hostent = gethostbyname("127.0.0.1");
  if (!hostent) {
    return -1;
  }

  struct sockaddr_in addr;  
  addr.sin_family = AF_INET;     
  addr.sin_port = htons(port);   
  addr.sin_addr = *((struct in_addr *)hostent->h_addr);
  bzero(&(addr.sin_zero),8);     

  int sock=socket(AF_INET, SOCK_STREAM, 0);
  if (sock==-1) {
    perror("Failed to create a socket.");
    return -1;
  }

  if (connect(sock,(struct sockaddr *)&addr,sizeof(struct sockaddr)) == -1) {
    perror("connect() to port failed");
    close(sock);
    return -1;
  }
  return sock;
}

int read_from_socket(int sock,unsigned char *buffer,int *count,int buffer_size)
{
  int t=time(0);
  if (*count>=buffer_size) return 0;
  int r=read(sock,&buffer[*count],buffer_size-*count);
  while(r!=0) {
    if (r>0) {
      (*count)+=r;
      t=time(0);
    }
    r=read(sock,&buffer[*count],buffer_size-*count);
    if (r==-1&&errno!=EAGAIN) {
      perror("read() returned error. Stopping reading from socket.");
      return -1;
    }
    // timeout after a few seconds of nothing
    if (time(0)-t>10) break;
  }
  buffer[*count]=0;
  return 0;
}

int launch_student_programme(const char *executable)
{
  // Find a free TCP port for the student programme to listen on
  // that is not currently in use.
  student_port=(getpid()|0x8000)&0xffff;
  int portclear=0;
  while(!portclear&&(student_port<65536)) {
    int sock=connect_to_port(student_port);
    if (sock==-1) portclear=1; 
    else close(sock);
  }
  fprintf(stderr,"Port %d is available for use by student programme.\n",
	  student_port);

  pid_t child_pid = fork();

  if (child_pid==-1) {
    perror("fork"); return -1;
  }
  char port[128];
  snprintf(port,128,"%d",student_port);
  const char *const args[]={executable,port,NULL,NULL};

  if (!child_pid) {
    // as the child: so exec() to the student's program
    execv(executable,(char **)args);
    /* execv doesn't return if it is successful */
    perror("execv");
    return -1;
  }
  /*
    parent: just remember the PID so that we can kill it later
  */
  student_pid=child_pid;
  return 0;
}

int test_listensonport()
{
  /* Test that student programme accepts a connection on the port */
  int i;
  for(i=0;i<10;i++) {
    int sock=connect_to_port(student_port);
    if (sock>-1) {
      close(sock);
      printf("SUCCESS: Accepts connections on specified TCP port\n");
      success++;
      return 0;
    }
    // allow some time for the student programme to get sorted.
    // 100ms x 10 times should be enough
    usleep(100000);
  }
  printf("FAIL: Accepting connection on a TCP port.\n");
  return -1;
}

int test_acceptmultipleconnections()
{
  /* Test that student programme accepts 1,000 successive connections.
     Further test that it can do so within a minute. */
  int start_time=time(0);
  int i;
  for(i=0;i<1000;i++) {
    int sock=connect_to_port(student_port);
    // Be merciful with student programs that are too slow to take 1,000 connections
    // coming in really fast.
    if (sock==-1) {
      if (i<10) usleep(100000); else usleep(1000);
      sock=connect_to_port(student_port);
    }
    if (sock==-1) {
      printf("FAIL: Accepting multiple connections on a TCP port (failed on attempt %d).\n",i);
      return -1;
    }
    close(sock);
    // allow upto 5 minutes to handle the 1,000 connections.
    if (time(0)-start_time>300) break;
  }
  
  int end_time=time(0);
  
  if (i==1000)
    { printf("SUCCESS: Accepting multiple connections on a TCP port\n"); success++; }
  else
    printf("FAIL: Accepting multiple connections on a TCP port. Did not complete 1,000 connections in less than 5 minutes.\n");

  if (end_time-start_time>60)
    printf("FAIL: Accept 1,000 connections in less than a minute.\n");
  else 
    { printf("SUCCESS: Accepted 1,000 connections in less than a minute.\n");
      success++; }

  return 0;
}

int main(int argc,char **argv)
{
  if (argc!=2) {
    fprintf(stderr,"usage: test <example program>\n");
    exit(-1);
  }

  launch_student_programme(argv[1]);
  if (student_pid<0) {
    perror("Failed to launch student programme.");
    return -1;
  }

  test_listensonport();
  test_acceptmultipleconnections();

  int score=success*84/25;
  printf("Passed %d of 25 tests.\n"
	 "Score for functional aspects of assignment 1 will be %02d%%.\n"
	 "Score for style (0%% -- 16%%) will be assessed manually.\n"
	 "Therefore your grade for this assignment will be in the range %02d%% -- %02d%% (%s -- %s)\n",
	 success,score,score,score+16,gradeOf(score),gradeOf(score+15));

  if (student_pid>100) {
    fprintf(stderr,"About to kill student process %d\n",(int)student_pid);
    int r=kill(student_pid,SIGKILL);
    fprintf(stderr,"Seeing how that went.\n");
    if (r) perror("failed to kill() student process.");
  } else {
    fprintf(stderr,"Successfully cleaned up student process.\n");
  }

  return 0;
}
