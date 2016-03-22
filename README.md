# Simple tcp server using epoll I/O event notifcation

Usage: start the server './_epoll port' in one terminal
open two or more terminals and connect via telnet
'telnet localhost port'.  (escape ctrl],quit)
The server terminal prints the client messages

to compile:
```bash
  g++ epollServ.c -o _epoll
```

