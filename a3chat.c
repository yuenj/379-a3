/*****************************************************
 * Author: Jennifer Yuen                             *
 * CMPUT 379 Assignment 3: Chat program with sockets *
 * Acknowledgements: APUE book, lecture notes,       *
 *   stack overflow                                  *
 * Date: November 16, 2017                           *
 * Professor: Ehab Elmallah                          *
 *****************************************************/

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#define MAXBUF 200             // max buffer size for messages
#define MAXLINE 4096           // max line length errors
#define CPU_LIMIT 600          // lifespan of the chat program: 600 seconds
#define KAL_char 0x6           // A non-printable character (e.g., ACK)
#define KAL_length 5           // Number of KAL_char in one keepalive message
#define KAL_interval 1.5       // Client sends a keepalive message every 1.5 seconds
#define KAL_count 5            // Number of consecutive keepalive messages that needs
                               // to be missed for the server to consider that the client
                               // has terminated unexpectedly

static void keep_alive(int signo);
static void err_doit(int errnoflag, int error, const char *fmt, va_list ap);
void err_quit(const char *fmt, ...);
void err_sys(const char *fmt, ...);

static jmp_buf return_pt;

int main(int argc, char *argv[])
{
  // check for at least three arguments
  if (argc < 4) {
    err_quit("Usage: %s -s portnumber nclient\n"
             "e.g., %s -s 9757 5\n"
             "Usage: %s -c portnumber serverAddress\n"
             "e.g., %s -c 9757 ui07.cs.ualberta.ca",
             argv[0], argv[0], argv[0], argv[0]);
  }
  
  // set a cpu limit
  struct rlimit cpuLimit;
  cpuLimit.rlim_cur = cpuLimit.rlim_max = CPU_LIMIT;
  if (setrlimit(RLIMIT_CPU, &cpuLimit) < 0) {
    err_sys("%s: can't set cpu limit", argv[0]);
  }
  getrlimit(RLIMIT_CPU, &cpuLimit);
  printf("cpuLimit: current (soft)= %lu, maximum (hard)= %lu \n",
         cpuLimit.rlim_cur, cpuLimit.rlim_max);
  
  // shared global variables
  int i;
  char buf[MAXBUF];
  
  //================================ C H A T - C L I E N T =====================================

  // chat client
  if (strcmp(argv[1], "-c") == 0) {

    // lookup the specified host
    struct hostent *hp = gethostbyname(argv[3]); // host entity
    if (hp == (struct hostent *)NULL) {
      err_quit("%s: unknown host '%s'", argv[0], argv[3]);
    }
    char ip[30];
    if (inet_ntop(AF_INET, hp->h_addr_list[0], ip, sizeof ip) == NULL) {
      err_sys("%s: could not convert network byte order to ascii", argv[0]);
    }
    printf("Chat client begins (server '%s' [%s], port %s)\n",
           hp->h_name, ip, argv[2]);
    printf("a3chat_client: ");
    fflush(stdout);
    
    // fill the socket structure with the information at which
    // the remote server listens
    struct sockaddr_in server;
    memset((char *) &server, 0, sizeof server);
    server.sin_family = hp->h_addrtype;
    memcpy((char *) &server.sin_addr, hp->h_addr, hp->h_length);
    server.sin_port = htons(atoi(argv[2]));
    
    // create the socket
    int s;
    if ((s = socket(hp->h_addrtype, SOCK_STREAM, 0)) < 0) {
      err_sys("%s: failed to create socket", argv[0]);
    }
    
    // prepare for nonblocking I/O polling from the keyboard and socket
    struct pollfd pfd[2];
    pfd[0].fd = STDIN_FILENO; pfd[1].fd = s;
    pfd[0].events = pfd[1].events = POLLIN;
    pfd[0].revents = pfd[1].revents = 0;
    int timeout = 0;
    int rval;
    int N;

    int connected = 0;  // flag is 1 if client is in a connected state
    int prompt = 0;     // flag is 1 if the prompt should be issued 
    time_t keepalive;   // keepalive timer
    
    // client loop
    N = 1; // initially, only poll stdin until client is in a connected state
    while (1) {
      if ((rval = poll(pfd, N, timeout)) < 0) {
        err_sys("%s: poll error", argv[0]);
      }
      // send a keepalive message to the server every KAL_interval seconds
      if ((connected == 1) && (difftime(time(NULL), keepalive) >= KAL_interval)) {
        time(&keepalive);
        memset(buf, 0, sizeof buf);
        for (i = 0; i < KAL_length; ++i) {
          buf[i] = KAL_char;
        }
        buf[i++] = '\n'; buf[i] = 0;
        if (send(s, buf, strlen(buf), 0) < 0) {
          err_sys("%s: failed to send", argv[0]);
        }
      }
      // if client is in a connected state, check for commands from the server
      if (N > 1 && pfd[1].revents & POLLIN) { 
        memset(buf, 0, sizeof buf);
        if (read(s, buf, sizeof buf) < 0) {
          err_sys("%s: failed to read", argv[0]);
        }
        // handle server commands
        printf("%s", buf); // echo server command
        fflush(stdout);
        // if server has reported an error, or client has requested to
        // exit, reset the socket
        if ((strstr(buf, "Error") != NULL) ||
            (strstr(buf, "done") != NULL)) {
          close(s);
          connected = 0;
          if ((s = socket(hp->h_addrtype, SOCK_STREAM, 0)) < 0) {
            err_sys("%s: failed to create socket", argv[0]);
          }
          // prepare for polling the socket but do not poll it yet
          --N;
          pfd[1].fd = s;
        }
        prompt = 1;
      }
      // prompt chat user for next command
      if (prompt == 1) {
        printf("a3chat_client: ");
        fflush(stdout);
        prompt = 0;
      }
      // check for commands from the chat user
      if (pfd[0].revents & POLLIN) { 
        memset(buf, 0, sizeof buf);
        if (fgets(buf, MAXBUF, stdin) != NULL) {
          // handle stdin commands
          
          //===========================================
          if (strstr(buf, "open ") != NULL) {
            // open a TCP connection to the server
            if (connect(s, (struct sockaddr *) &server, sizeof server) < 0) {
              if (errno == EISCONN) {
                printf("You already have a chat session going on.\n");
              } else if (errno == ECONNREFUSED) {
                printf("The server is not running or has reached the client limit.\n");
              } else {
                err_sys("%s: failed to connect", argv[0]);
              }
            } else {
              connected = 1;    // client is in a connected state
              time(&keepalive); // start the timer for periodic keepalive messages
              ++N;              // begin polling of the socket
              if (send(s, buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
              // wait for the server to respond
              sleep(1);
            }
          }

          //===========================================
          else if (strstr(buf, "who") != NULL) {
            // get a list of the logged in users
            if (connected) {
              if (send(s, buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
              // wait for the server to respond
              sleep(1);
            } else {
              printf("Please connect to a chat session first: open <username>.\n");
            }
          }
          
          //===========================================
          else if (strstr(buf, "to ") != NULL) {
            // Add the specified users to the list of recipients
            if (connected) {
              if (send(s, buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
              // wait for the server to respond
              sleep(1);
            } else {
              printf("Please connect to a chat session first: open <username>.\n");
            }
          }
          
          //===========================================
          else if (strstr(buf, "< ") != NULL) {
            // Send chat_line to all specified recipients
            if (connected) {
              if (send(s, buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
              // wait for the server to respond
              sleep(1);
            } else {
              printf("Please connect to a chat session first: open <username>.\n");
            }
          }
          
          //===========================================
          else if (strstr(buf, "close") != NULL) {
            // close the current chat session without terminating the client program
            if (connected) {
              if (send(s, buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
              // wait for the server to respond
              sleep(1);
            } else {
              printf("Please connect to a chat session first: open <username>.\n");
            }
          }
          
          //===========================================
          else if (strstr(buf, "exit") != NULL) {
            // terminate the client program
            if (connected) {
              if (send(s, buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
            }
            // allow some buffer time before exiting
            sleep(1);
            // break from client loop
            break;
          }
        }
        prompt = 1;
      }
    }
    // clean up
    close(s);
    return 0;
  }
  
  //================================ C H A T - S E R V E R =====================================
  
  // chat server
  if (strcmp(argv[1], "-s") == 0) {

    printf("Chat server begins [port= %s] [nclient= %s]\n",
           argv[2], argv[3]);
    int nclient = atoi(argv[3]);

    // containers for client resources
    int newsock[nclient+1];               // socket file descriptors 
    FILE *sfpin[nclient+1];               // socket file pointers for reading 
    char *usernames[nclient+1];           // username
    int recipients[nclient+1][nclient+1]; // each client has a list of recipients
    time_t keepalive[nclient+1];          // last keepalive time 
    time_t lastcmd[nclient+1];            // the last time the client sent a command to the server 
                                          // not to be confused with keepalive messages 
                                          // (used to keep track of idle clients)

    // initial state of the client resources
    for (i = 1; i < nclient+1; ++i) {
      usernames[i] = NULL;
      keepalive[i] = 0;
      for (int j = 1; j < nclient+1; ++j) {
        recipients[i][j] = 0;
      }
    }
    
    // create a managing (listening) socket
    int s;
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      err_sys("%s: failed to create socket", argv[0]);
    }
    // bind the managing socket to a name
    struct sockaddr_in sin;
    memset((char *) &sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(atoi(argv[2]));
    if (bind(s, (struct sockaddr *)&sin, sizeof sin) < 0) {
      err_sys("%s: failed to bind socket", argv[0]);
    }
    
    // indicate how many connection requests can be queued
    if (listen(s, nclient) < 0) {
      err_sys("%s: failed to listen", argv[0]);
    }
    
    // prepare for nonblocking I/O polling from the master socket and the keyboard
    struct pollfd pfd[nclient+1], pstdin[1];
    pfd[0].fd = s; pstdin[0].fd = STDIN_FILENO;
    pfd[0].events = pstdin[0].events = POLLIN;
    pfd[0].revents =  pstdin[0].revents = 0;
    int timeout = 0;
    int rval;
    int N = 1; // initially just one descriptor to poll
    
    // set timer for activity reporting and monitoring of terminated clients
    if (signal(SIGALRM, keep_alive) < 0) {
      err_sys("%s: failed to signal", argv[0]);
    }
    alarm(15); // check every 15 seconds
    
    // server loop
    while (1) {

      // generate a activity report of all logged in clients and their recorded times 
      if (setjmp(return_pt) == 1) {
        alarm(15); // reset the alarm
        
        printf("activity report:\n");
        
        for (i = 1; i < N; ++i) {

          char datetime[MAXBUF];
          struct tm *tmp;

          // check for clients that have terminated unexpectedly
          if ((keepalive[i] != 0) && (difftime(time(NULL), keepalive[i]) >= (KAL_interval * KAL_count))) {
            // report the last time a keepalive message was detected
            tmp = localtime(&keepalive[i]);
            strftime(datetime, MAXBUF, "%c", tmp);
            printf("'%s' [sockfd= %d]: loss of keepalive messages detected at %s, connection closed\n", usernames[i], newsock[i], datetime);
            fflush(stdout);

            // free resources that have been allocated to the client
            fclose(sfpin[i]);
            close(newsock[i]);
            free(usernames[i]);

            // shift container elements one to the left since we are decrementing N
            for (int j = 1; j < N; ++j) {
              if (j != i) {
                for (int k = i; k < N-1; ++k) {
                  recipients[j][k] = recipients[j][k+1];
                }
              }
            }
            for (int j = i; j < N-1; ++j) {
              newsock[j] = newsock[j+1];
              usernames[j] = usernames[j+1];
              keepalive[j] = keepalive[j+1];
              lastcmd[j] = lastcmd[j+1];
              pfd[j].fd = newsock[j];
              for (int k = 1; k < N-1; ++k) {
                recipients[j][k] = recipients[j+1][k];
              }
            }
            // clear data for the last element since we have shifted elements
            usernames[N-1] = NULL;
            keepalive[N-1] = 0;
            lastcmd[N-1] = 0;
            for (int j = 1; j < N; ++j) {
              recipients[N-1][j] = 0;
              recipients[j][N-1] = 0;
            }
            --N;
          } else {
            // if the client is logged in, 
            // display the last time that they sent a command to the server
            tmp = localtime(&lastcmd[i]);
            strftime(datetime, MAXBUF, "%c", tmp);
            printf("'%s' [sockfd= %d]:%s\n", usernames[i], newsock[i], datetime);
            fflush(stdout);
          }
        }
      }
      // check stdin for commands
      if ((rval = poll(pstdin, 1, timeout)) < 0) {
        err_sys("%s: poll error", argv[0]);
      }
      // handle the exit command to shut down the server
      if (pstdin[0].revents & POLLIN) {
        memset(buf, 0, sizeof buf);
        fgets(buf, MAXBUF, stdin);
        if (strstr(buf, "exit") != NULL) {
          break;
        }
      }
      // check sockets for commands
      if ((rval = poll(pfd, N, timeout)) < 0) {
        err_sys("%s: poll error", argv[0]);
      }
      // handle a new connection request from the listening socket
      if ((N <= nclient) && (pfd[0].revents & POLLIN)) {
        // accept a new connection
        struct sockaddr_in from;
        memset((char *) &from, 0, sizeof from);
        unsigned int fromlen = sizeof((struct sockaddr *) &from);
        newsock[N] = accept(s, (struct sockaddr *)&from, &fromlen);
        
        // we may also want to perform STREAM I/O
        if ((sfpin[N] = fdopen(newsock[N], "r")) < 0) {
          err_sys("%s: failed converting socket to FILE *", argv[0]);
        }
        
        // start the keepalive timer
        time(&keepalive[N]);

        // prepare for nonblocking I/O polling from the new socket
        pfd[N].fd = newsock[N];
        pfd[N].events = POLLIN;
        pfd[N].revents = 0;
        ++N;
      } 
      // check connected client sockets for commands
      for (i = 1; i < N; ++i) {
        if (pfd[i].revents & POLLIN) { 
          // handle client commands
          memset(buf, 0, sizeof buf);
          if (fgets(buf, MAXBUF, sfpin[i]) != NULL) {

            //===========================================
            if (strstr(buf, "open ") != NULL) {
              // client has requested to open a TCP connection

              // update the time of the last command from the client
              time(&lastcmd[i]);

              // extract username
              strtok(buf, " ");                     // get the second word
              char *username = strtok(NULL, " \n"); // get the username
              int dup = 0;                          // flag for duplicate username

              // search for duplicate usernames
              for (int j = 1; j < N; ++j) {
                if ((usernames[j] != NULL) && (strcmp(usernames[j], username) == 0)) {
                  dup = 1;
                  break;
                }
              }
              // if the username exists, reject the clients request to connect
              if (dup == 1) { 
                memset(buf, 0, sizeof buf);
                strcpy(buf, "[server] Error: there is another client that uses the specified username for chatting\n");
                if (send(newsock[i], buf, strlen(buf), 0) <0) {
                  err_sys("%s: failed to send", argv[0]);
                }
                // close the socket
                close(newsock[i]);
                --N;
              } 
              // the username is unique, safe to proceed forward
              else {
                // save the username
                usernames[i] = (char *) malloc(strlen(username) * sizeof(char));
                strcpy(usernames[i], username);

                // send the server response
                memset(buf, 0, sizeof buf);
                strcpy(buf, "[server] connected\n");
                if (send(newsock[i], buf, strlen(buf), 0) < 0) {
                  err_sys("%s: failed to send", argv[0]);
                };
                memset(buf, 0, sizeof buf);
                sprintf(buf, "[server] User '%s' logged in\n", usernames[i]);
                if (send(newsock[i], buf, strlen(buf), 0) < 0) {
                  err_sys("%s: failed to send", argv[0]);
                }
              }
            }
            
            //===========================================
            else if (strstr(buf, "who") != NULL) {
              // client wishes to get a list of logged in users 

              // update the time of the last command 
              time(&lastcmd[i]);

              // retrieve a list of logged in users
              memset(buf, 0, sizeof buf);
              strcpy(buf, "[server] Current users: ");
              for (int j = 1; j < N; ++j) {
                if (usernames[j] != NULL) {
                  strcat(buf, usernames[j]);
                  strcat(buf, ", ");
                }
              }
              // remove trailing characters
              if (buf[strlen(buf)-2] == ',') {
                buf[strlen(buf)-2] = '\n';
                buf[strlen(buf)-1] = 0;
              }
              // send the server response
              if (send(newsock[i], buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
            }
            
            //===========================================
            else if (strstr(buf, "to ") != NULL) {
              // client wishes to add recipients

              // update the time of the last command 
              time(&lastcmd[i]);

              // create the server response message
              char res[MAXBUF];
              strcpy(res, "[server] recipients added: ");
              
              char *r;
              r = strtok(buf, " \n");  // remove the "to" from string
              r = strtok(NULL, " \n"); // get the first recipient in the list
              while (r != NULL) {
                // check if the recipient exists
                for (int j = 1; j < N; ++j) {
                  if ((usernames[j] != NULL) && (strcmp(r, usernames[j]) == 0)) {
                    // guard against duplicate recipients in the to list
                    if (!recipients[i][j]) {
                      recipients[i][j] = 1;
                      strcat(res, r);
                      strcat(res, ", ");
                      break;
                    }
                  }
                }
                r = strtok(NULL, " \n"); // get the next recipient in the list
              }
              // remove trailing characters
              if (res[strlen(res)-2] == ',') {
                res[strlen(res)-2] = '\n';
                res[strlen(res)-1] = 0;
              } else { // account for the case where the to list is empty
                strcat(res, "\n");
              }

              // send the server response
              if (send(newsock[i], res, strlen(res), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
            }
            
            //===========================================
            else if (strstr(buf, "< ") != NULL) {
              // client wishes to send chat line to all recipients

              // update the time of the last command 
              time(&lastcmd[i]);

              // create the server response message
              char res[MAXBUF];
              strcpy(res, "[");
              strcat(res, usernames[i]);
              strcat(res, "] ");
              strtok(buf, " \n");
              strcat(res, strtok(NULL, "")); // concatenate chat_line
              // trim trailing space
              char *end = res + strlen(res) - 1;
              while(end > res && isspace((unsigned char)*end)) end--;
              *(end+1) = 0;
              strcat(res, "\n");
              
              // version of the response with a newline at the beginning
              char res_nl[MAXBUF];
              strcpy(res_nl, "\n");
              strcat(res_nl, res);

              // send the message to each recipient
              for (int j = 1; j < N; ++j) {
                if (recipients[i][j] == 1) {
                  if (i != j) {
                    if (send(newsock[j], res_nl, strlen(res_nl), 0) < 0) {
                      err_sys("%s: failed to send", argv[0]);
                    }
                  } else {
                    if (send(newsock[j], res, strlen(res), 0) < 0) {
                      err_sys("%s: failed to send", argv[0]);
                    }
                  }
                }
              }
            }
            
            //===========================================
            else if (strstr(buf, "close") != NULL) {
              // client wishes to close the chat session
              
              // create the server response message
              memset(buf, 0, sizeof buf);
              strcpy(buf, "[server] done\n");
              
              // send the server response
              if (send(newsock[i], buf, strlen(buf), 0) < 0) {
                err_sys("%s: failed to send", argv[0]);
              }
              
              // free client resources
              fclose(sfpin[i]);
              close(newsock[i]);
              free(usernames[i]);

              // shift container elements one to the left since we are decrementing N
              for (int j = 1; j < N; ++j) {
                if (j != i) {
                  for (int k = i; k < N-1; ++k) {
                    recipients[j][k] = recipients[j][k+1];
                  }
                }
              }
              for (int j = i; j < N-1; ++j) {
                newsock[j] = newsock[j+1];
                usernames[j] = usernames[j+1];
                keepalive[j] = keepalive[j+1];
                lastcmd[j] = lastcmd[j+1];
                pfd[j].fd = newsock[j];
                for (int k = 1; k < N-1; ++k) {
                  recipients[j][k] = recipients[j+1][k];
                }
              }
              // clear data for the last element since we have shifted elements 
              usernames[N-1] = NULL;
              keepalive[N-1] = 0;
              lastcmd[N-1] = 0;
              for (int j = 1; j < N; ++j) {
                recipients[N-1][j] = 0;
                recipients[j][N-1] = 0;
              }
              --N;
            }
          
            //===========================================
            else if (strstr(buf, "exit") != NULL) {
              // client wishes to terminate the chat program

              // free client resources
              fclose(sfpin[i]);
              close(newsock[i]);
              free(usernames[i]);

              // shift container elements one to the left since we are decrementing N
              for (int j = 1; j < N; ++j) {
                if (j != i) {
                  for (int k = i; k < N-1; ++k) {
                    recipients[j][k] = recipients[j][k+1];
                  }
                }
              }
              for (int j = i; j < N-1; ++j) {
                newsock[j] = newsock[j+1];
                usernames[j] = usernames[j+1];
                keepalive[j] = keepalive[j+1];
                lastcmd[j] = lastcmd[j+1];
                pfd[j].fd = newsock[j];
                for (int k = 1; k < N-1; ++k) {
                  recipients[j][k] = recipients[j+1][k];
                }
              }
              // clear data for the last element since we have shifted elements
              usernames[N-1] = NULL;
              keepalive[N-1] = 0;
              lastcmd[N-1] = 0;
              for (int j = 1; j < N; ++j) {
                recipients[N-1][j] = 0;
                recipients[j][N-1] = 0;
              }
              --N;
            }
            
            //===========================================
            else if (strchr(buf, KAL_char) != NULL) {
              // process periodic keepalive messages from the client
              time(&keepalive[i]);
            }
          }
        }
      }
    }
    // terminate the server
    memset(buf, 0, sizeof buf);
    // notify clients that the server is terminating
    strcpy(buf, "[server] Error: server terminating\n");
    for (i = 1; i < N; ++i) {
      if (send(newsock[i], buf, strlen(buf), 0) < 0) {
        err_sys("%s: failed to send", argv[0]);
      }
      // allow time for the client to disconnect
      sleep(1);
      // free client resources
      fclose(sfpin[i]);
      close(newsock[i]);
      free(usernames[i]);
    }
    // clean up
    close(s);
    return 0;
  }
}

// ------------------------------
// SIGALRM signal handler.  The alarm is triggered periodically to 
// display activity reports and detect terminated clients.
static void keep_alive(int signo)
{
  longjmp(return_pt, 1);
}

// ------------------------------
// Print a message and return to caller. Caller specifies "errnoflag".
// Source: [APUE 3/E]
static void err_doit(int errnoflag, int error, const char *fmt, va_list ap)
{
  char buf[MAXLINE];
  
  vsnprintf(buf, MAXLINE-1, fmt, ap);
  if (errnoflag)
    snprintf(buf+strlen(buf), MAXLINE-strlen(buf)-1, ": %s", strerror(error));
  strcat(buf, "\n");
  fflush(stdout);       // in case stdout and stderr are the same
  fputs(buf, stderr);
  fflush(NULL);         // flushes all stdio output streams
}

// ------------------------------
// Fatal error unrelated to a system call. Print a message and terminate.
// Source: [APUE 3/E]
void err_quit(const char *fmt, ...)
{
  va_list ap;
  
  va_start(ap, fmt);
  err_doit(0, 0, fmt, ap);
  va_end(ap);
  exit(1);
}

// ------------------------------
// Fatal error related to a system call. Print a message and terminate.
// Source: [APUE 3/E]
void err_sys(const char *fmt, ...)
{
  va_list ap;
  
  va_start(ap, fmt);
  err_doit(1, errno, fmt, ap);
  va_end(ap);
  exit(1);
}
