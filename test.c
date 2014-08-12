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

int read_from_socket(int sock,unsigned char *buffer,int *count,int buffer_size,
		     int timeout)
{
  fcntl(sock,F_SETFL,fcntl(sock, F_GETFL, NULL)|O_NONBLOCK);


  int t=time(0)+timeout;
  if (*count>=buffer_size) return 0;
  int r=read(sock,&buffer[*count],buffer_size-*count);
  while(r!=0) {
    if (r>0) {
      (*count)+=r;
    }
    r=read(sock,&buffer[*count],buffer_size-*count);
    if (r==-1&&errno!=EAGAIN) {
      perror("read() returned error. Stopping reading from socket.");
      return -1;
    } else usleep(100000);
    // timeout after a few seconds of nothing
    if (time(0)>=t) break;
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
    write(sock,"QUIT\n",5);
    close(sock);
    printf("\rMade %d/1000 connections",i); fflush(stdout);
    // allow upto 5 minutes to handle the 1,000 connections.
    if (time(0)-start_time>300) break;
  }
  printf("\n");
  
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

int test_next_response_is(int code,char *mynick,char *buffer,int *bytes)
{
  if ((*bytes)<10) {
    printf("FAIL: Too few bytes from server (%d) when looking for"
	   " server message %03d\n",
	   *bytes,code);
    return -1;
  } else {
    printf("SUCCESS: There are at least 10 bytes when looking for server message %03d\n",code);
    success++;
  }
  int n=0;
  int thecode;
  char serverid[*bytes];
  char thenick[*bytes];
  char themessage[*bytes];
  int r=sscanf(buffer,":%[^ ] %*[0]%d %s %[^\n]%*[\n\r]%n",
	       serverid,&thecode,thenick,themessage,&n);
  if (n>0&&(n<=(*bytes))) {
    bcopy(&buffer[n],&buffer[0],(*bytes)-n);
    (*bytes) = (*bytes) - n;
    printf("SUCCESS: Server message was a sensible length.\n");
    success++;
  } else {
    printf("FAIL: Server message was not a sensible length.\n");
    *bytes=0;
    return -1;
  }

  if (r!=4) {
    printf("FAIL: Could not parse server message\n");
    return -1;
  } else {
    printf("SUCCESS: Could parse server message (saw code %03d)\n",thecode);
  }
  if (strcasecmp(mynick,thenick)) {
    printf("FAIL: Server message contains wrong nick name (saw '%s' instead of '%s').\n",thenick,mynick);
    return -1;
  } else {
    printf("SUCCESS: Server message contains correct nick name.\n");
  }
  if (code!=thecode) {
    printf("FAIL: Server message contains wrong code (saw %03d instead of %03d).\n",
	   thecode,code);
    return -1;
  } else {
    printf("SUCCESS: Server message contains correct nick name.\n");
  }
  return 0;    
}

int test_next_response_is_error(char *message,char *buffer,int *bytes)
{
  if ((*bytes)<10) {
    printf("FAIL: Too few bytes from server (%d) when looking for server error '%s'\n",
	   *bytes,message);
    return -1;
  } else {
    printf("SUCCESS: There are at least 10 bytes when looking for server error '%s'\n",message);
    success++;
  }
  int n=0;
  char themessage[*bytes];
  int r=sscanf(buffer,"ERROR :%[^:]: %*[^\n]%*[\n\r]%n",
	       themessage,&n);
  if (n>0&&(n<=(*bytes))) {
    bcopy(&buffer[n],&buffer[0],(*bytes)-n);
    (*bytes) = (*bytes) - n;
    printf("SUCCESS: Server ERROR message was a sensible length.\n");
    success++;
  } else {
    printf("FAIL: Server message was not a sensible length.\n");
    *bytes=0;
    return -1;
  }

  if (r!=2) {
    printf("FAIL: Could not parse server error message\n");
    return -1;
  } else {
    printf("SUCCESS: Saw correct server error (saw '%s')\n",themessage);
  }
  if (strcasecmp(message,themessage)) {
    printf("FAIL: Server error contains wrong message (saw '%s' instead of '%s').\n",themessage,message);
    return -1;
  } else {
    printf("SUCCESS: Server error contains correct message.\n");
  }
  return 0;    
}


int failif(int failuretest,char *failmsg,char *successmsg)
{
  if (failuretest) {
    printf("FAIL: %s\n",failmsg);
    return -1;
  } else {
    printf("SUCCESS: %s\n",successmsg); success++;
    return 0;
  }
}

char *channel_names[]={"#deutsch","#koeln","#leipzig","#bonn"};
char *greetings[]={"Hallo alle","Guten abend","Wie geht's alle?","moin",
		   "Gibt es jemand hier?","Ich bin eine Kartoffel",
		   "Pausenzeit","Kann mir jemand helfen mit meinem Auftragt?"};

int test_servergreeting()
{
  /* Test that student programme accepts 1,000 successive connections.
     Further test that it can do so within a minute. */
  int sock=connect_to_port(student_port);
  if (sock==-1) {
    printf("FAIL: Could not connect to server\n");
      return -1;
  }
  else
    { printf("SUCCESS: Connected to server\n"); success++; }
  
  char buffer[8192];
  int bytes=0;
  int r;
  char cmd[8192];

  // Check for server response
  r=read_from_socket(sock,(unsigned char *)buffer,&bytes,sizeof(buffer),2);
  if (r||(bytes<1)) {
    close(sock);
    printf("FAIL: No greeting received from server.\n");
    return -1;
  } else {
    printf("SUCCESS: Server said something\n"); success++; 
  } 
  if (bytes>8191) bytes=8191;
  if (bytes>=0&&bytes<8192) buffer[bytes]=0;
  // Check for initial server greeting
  test_next_response_is(20,"*",buffer,&bytes);
  // check that there is nothing more in there    
  if (failif(bytes>0,
	     "Extraneous server message(s)",
	     "Server said nothing else before registration")) {
    printf("FAIL: There are %d extra bytes: '%s'\n",bytes,buffer);
    return -1;
  }
  r=read_from_socket(sock,(unsigned char *)buffer,&bytes,sizeof(buffer),5);
  test_next_response_is_error("Closing Link",buffer,&bytes);
  if (failif(bytes>0,
	     "Extraneous server message(s) after timeout ERROR message",
	     "Server said nothing else before registration")) {
    printf("FAIL: There are %d extra bytes: '%s'\n",bytes,buffer);
    return -1;
  }

  // connections should have timed out, so get a fresh one
  close(sock);
  sock=connect_to_port(student_port);
  if (sock==-1) {
    printf("FAIL: Could not connect to server\n");
    return -1;
  }
  else
    { printf("SUCCESS: Connected to server\n"); success++; }
  r=read_from_socket(sock,(unsigned char *)buffer,&bytes,sizeof(buffer),2);
  test_next_response_is(20,"*",buffer,&bytes);
  
  // Confirm that we can't send JOIN or MSG before registering
  sprintf(cmd,"JOIN %s\n\r",channel_names[getpid()&3]);
  int w=write(sock,cmd,strlen(cmd));
  // expect a 241 complaint message
  r=read_from_socket(sock,(unsigned char *)buffer,&bytes,sizeof(buffer),2);
  test_next_response_is(241,"*",buffer,&bytes);
  sprintf(cmd,"PRIVMSG %s :%s\n\r",
	  channel_names[getpid()&3],
	  greetings[time(0)&7]);
  w=write(sock,cmd,strlen(cmd));
  // expect a 241 complaint message
  r=read_from_socket(sock,(unsigned char *)buffer,&bytes,sizeof(buffer),2);
  test_next_response_is(241,"*",buffer,&bytes);

  return 0;
}


int main(int argc,char **argv)
{
  if (argc!=2) {
    fprintf(stderr,"usage: test <example program>\n");
    exit(-1);
  }

  if (atoi(argv[1])==0)
    launch_student_programme(argv[1]);
  else {
    student_port=atoi(argv[1]);
    student_pid=99999;
  }

  if (student_pid<0) {
    perror("Failed to launch student programme.");
    return -1;
  }

  test_listensonport();
  test_acceptmultipleconnections();
  test_servergreeting();

  int score=success*84/25;
  printf("Passed %d of 25 tests.\n"
	 "Score for functional aspects of assignment 1 will be %02d%%.\n"
	 "Score for style (0%% -- 16%%) will be assessed manually.\n"
	 "Therefore your grade for this assignment will be in the range %02d%% -- %02d%% (%s -- %s)\n",
	 success,score,score,score+16,gradeOf(score),gradeOf(score+15));

  if (student_pid>100&&student_pid!=99999) {
    fprintf(stderr,"About to kill student process %d\n",(int)student_pid);
    int r=kill(student_pid,SIGKILL);
    fprintf(stderr,"Seeing how that went.\n");
    if (r) perror("failed to kill() student process.");
  } else {
    fprintf(stderr,"Successfully cleaned up student process.\n");
  }

  return 0;
}
