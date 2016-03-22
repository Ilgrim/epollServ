#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>
#include <map>

#define BUFFERSIZE 512
#define MAXEVENTS 128

/* Simple single threaded server 
 * utilising epoll I/O event notification mechanism 
 *
 * compile: g++ epollServ.c -o _epoll
 * Usage: _epoll <port>
 * clients connect using telnet localhost <port>
**/

int serverSock_init (char *port)
{
  struct addrinfo hints, *res;
  int status, sfd;

  memset (&hints, 0, sizeof (struct addrinfo));
  hints.ai_family = AF_INET;  // IPV4   
  hints.ai_socktype = SOCK_STREAM; // TCP socket 
  hints.ai_flags = AI_PASSIVE;     // needed for serversocket

  status = getaddrinfo (NULL, port, &hints, &res);  // populates res addrinfo struct ready for socket call
  if (status != 0)
  {
    fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (status));
    return -1;
  }
     
  sfd = socket (res->ai_family, res->ai_socktype, res->ai_protocol); // create endpoint socketFD
  if (sfd == -1) {
    fprintf (stderr, "Socket error\n");
    close (sfd); 
    return -1;
  }

  int optval = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)); // set port reuse opt for debugging

  status = bind (sfd, res->ai_addr, res->ai_addrlen); // bind addr to sfd, addr in this case is INADDR_ANY 
  if (status == -1)
  {
    fprintf (stderr, "Could not bind\n");
    return -1;      
  }  

  freeaddrinfo (res);
  return sfd;
}

int main (int argc, char *argv[])
{
  int sfd, s, efd;
  struct epoll_event event;
  struct epoll_event *events;
  std::map<int,int> clientMap;

  if (argc != 2) {
      fprintf (stderr, "Usage: %s [port]\n", argv[0]);
      exit (EXIT_FAILURE);
  }

  sfd = serverSock_init (argv[1]); 
  if (sfd == -1)
    abort ();

  int flags = fcntl (sfd, F_GETFL, 0);  // change socket fd to be non-blocking
  flags |= O_NONBLOCK;
  fcntl (sfd, F_SETFL, flags);

  s = listen (sfd, SOMAXCONN);  // mark socket as passive socket type 
  if (s == -1)
    {
      perror ("listen");
      abort ();
    }

  efd = epoll_create1 (0);  // create epoll instance 
  if (efd == -1)
    {
      perror ("epoll_create");
      abort ();
    }

  event.data.fd = sfd;
  event.events = EPOLLIN | EPOLLET;  // just interested in read's events using edge triggered mode
  s = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event); // Add server socket FD to epoll's watched list
  if (s == -1)
    {
      perror ("epoll_ctl");
      abort ();
    }

  /* Events buffer used by epoll_wait to list triggered events */
  events = (epoll_event*) calloc (MAXEVENTS, sizeof(event));  

  /* The event loop */
  while (1)
    {
      int n, i;

      n = epoll_wait (efd, events, MAXEVENTS, -1);  // Block until some events happen, no timeout
      for (i = 0; i < n; i++)
	{
	  
           /* Error handling */
           if ((events[i].events & EPOLLERR) ||
              (events[i].events & EPOLLHUP) ||
              (!(events[i].events & EPOLLIN)))
	    {
              /* An error has occured on this fd, or the socket is not
                 ready for reading (why were we notified then?) */
	      fprintf (stderr, "epoll error\n");
	      close (events[i].data.fd);  // Closing the fd removes from the epoll monitored list
              clientMap.erase(events[i].data.fd);
	      continue;
	    }

	    /* serverSocket accepting new connections */
            else if (sfd == events[i].data.fd)
	    {
              /* We have a notification on the listening socket, which
                 means one or more incoming connections. */
              while (1)
                {
                  struct sockaddr in_addr;
                  socklen_t in_len;
                  int infd;
                  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                  in_len = sizeof in_addr;
                  infd = accept (sfd, &in_addr, &in_len); // create new socket fd from pending listening socket queue
                  if (infd == -1) // error
                    {
                      if ((errno == EAGAIN) ||
                          (errno == EWOULDBLOCK))
                        {
                          /* We have processed all incoming connections. */
                          break;
                        }
                      else
                        {
                          perror ("accept");
                          break;
                        }
                    }

                  int optval = 1;
                  setsockopt(infd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));  // set socket for port reuse
                  
                  /* get the client's IP addr and port num */
                  s = getnameinfo (&in_addr, in_len,
                                   hbuf, sizeof hbuf,
                                   sbuf, sizeof sbuf,
                                   NI_NUMERICHOST | NI_NUMERICSERV);
                  if (s == 0)
                    {
                      printf("Accepted connection on descriptor %d "
                             "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                  /* Make the incoming socket non-blocking and add it to the
                     list of fds to monitor. */        
                  int flags = fcntl (infd, F_GETFL, 0);
                  flags |= O_NONBLOCK;
                  fcntl (infd, F_SETFL, flags);

                  event.data.fd = infd;
                  event.events = EPOLLIN | EPOLLET;                  

                  s = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event); 
                  if (s == -1)
                    {
                      perror ("epoll_ctl");
                      abort ();
                    }
                clientMap[event.data.fd]=0;  // init msg counter    
                }
              continue;
            }
          else
            {
              /* We have data on the fd waiting to be read. Read and
                 display it. We must read whatever data is available
                 completely, as we are running in edge-triggered mode
                 and won't get a notification again for the same
                 data. */
              int done = 0;

              while (1)
                {
                  ssize_t count;
                  char buf[BUFFERSIZE];

                  count = read (events[i].data.fd, buf, sizeof buf);  
                  
                  if (count == -1)
                    {
                      /* If errno == EAGAIN, that means we have read all
                         data. So go back to the main loop. */
                      if (errno != EAGAIN)
                        {
                          perror ("read");
                          done = 1;
                        }
                      break;
                    }
                  else if (count == 0)
                    {
                      /* End of file. The remote has closed the
                         connection. */
                      done = 1;
                      break;
                    }
                  
                  buf[count]=0;
                  char wbuf[BUFFERSIZE];
                  int cx=snprintf(wbuf,BUFFERSIZE,"(fd:%d seq:%d) %s",events[i].data.fd, clientMap[events[i].data.fd],buf);

                  /* Write the buffer to standard output */
                  s = write (1, wbuf, cx);
                  if (s == -1)
                    {
                      perror ("write");
                      abort ();
                    }
                }
                // Increment msg counter
                int tmp = clientMap[events[i].data.fd];
                tmp++;
                clientMap[events[i].data.fd]=tmp;
                
              if (done)
                {
                  printf ("Closed connection on descriptor %d\n",
                          events[i].data.fd);

                  /* Closing the descriptor will make epoll remove it
                     from the set of descriptors which are monitored. */
                  close (events[i].data.fd);
                  clientMap.erase(events[i].data.fd);
                }
            }
        }
    }

  free (events);
  close (sfd);

  return EXIT_SUCCESS;
}
