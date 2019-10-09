/*  Stress | a HTTP/HTTPS benchmark Tool
    Copyright (C) 2017 Alexander Tsanis

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
Build:
gcc stress.c -O3 -g -pthread -o stress

Usage:
./stress

example of use: ./stress -n 2000 -c 100 -t 8 localhost:8080
> localhost:8080 can be your local server, website, anything.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
typedef void (*sighandler_t) (int);

// It can be set to HTTPS protocol
#define HTTP_PREFIX "http://"

// It can be set to any available HTTP version {1.0,1.1,2.0}
#define HTTP_REQUEST "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n"

#define DEBUG_REQUEST  0x01
#define DEBUG_RESPONSE 0x02

#define INBUFSIZE 1024

#define BADREQUEST 0x1

#define MAXEVENTS 256

struct econn {
  int fd;
  size_t offs;
  int flags;
};

char *outbuf;
size_t outbufsize;

struct sockaddr_storage sss;
socklen_t sssln = 0;

// Initialize them
int concurrency = 1;
int number_of_threads = 1;

char *udaddr = "";

//volatile them, the numbers can go up to gazilion
volatile uint64_t numrequests = 0;
volatile uint64_t maxrequests = 0;
volatile uint64_t goodrequests = 0;
volatile uint64_t badrequests = 0;
volatile uint64_t inbytes = 0;
volatile uint64_t outbytes = 0;

uint64_t ticks = 0;

// TODO: Add debug flag
int debug = 0;

// For signal handler
int exit_i = 0;

struct timeval tv, tve;

static const char short_options[] = "n:c:t:u:h";

static const struct option long_options[] = {
  {"number", 1, NULL, 'n'},
  {"concurrency", 1, NULL, 'c'},
  {"threads", 0, NULL, 't'},
  {"udaddr", 1, NULL, 'u'},
  {"host", 1, NULL, 'h'},
  {NULL, 0, NULL, 0}
};

static void sig_handler (int arg) {
  maxrequests = numrequests;
}

static void start_time () {
  if (gettimeofday (&tv, NULL))
    {
      perror ("gettimeofday");
      exit (1);
    }
}

static void end_time () {
  if (gettimeofday (&tve, NULL))
    {
      perror ("gettimeofday");
      exit (1);
    }
}

static void init_con (int efd, struct econn *ec)
{

  struct epoll_event evt;
  int ret;

  ec->fd = socket (sss.ss_family, SOCK_STREAM, 0);
  ec->offs = 0;
  ec->flags = 0;

  if (ec->fd == -1)
    {
      perror ("socket() failed");
      exit (1);
    }

  fcntl (ec->fd, F_SETFL, O_NONBLOCK);

  do
    {
      ret = connect (ec->fd, (struct sockaddr *) &sss, sssln);
    }
  while (ret && errno == EAGAIN);

  if (ret && errno != EINPROGRESS)
    {
      perror ("connect() failed");
      exit (1);
    }

  evt.events = EPOLLOUT;
  evt.data.ptr = ec;

  if (epoll_ctl (efd, EPOLL_CTL_ADD, ec->fd, &evt))
    {
      perror ("epoll_ctl");
      exit (1);
    }
}

static void *worker (void *arg) {
  int efd, fd, ret, nerr, n, m;
  struct epoll_event evts[MAXEVENTS];
  char inbuf[INBUFSIZE], c;
  struct econn ecs[concurrency], *ec;

  efd = epoll_create (concurrency);

  if (efd == -1) {
      perror ("epoll");
      exit (1);
    }

  for (n = 0; n < concurrency; ++n)
    init_con (efd, ecs + n);

  for (;;) {

      do {
	  nerr = epoll_wait (efd, evts, sizeof (evts) / sizeof (evts[0]), -1);
	}
      while (!exit_i && nerr < 0 && errno == EINTR);

      if (exit_i != 0) {
	  exit (0);
	}

      if (nerr == -1) {
	  perror ("epoll_wait");
	  exit (1);
	}

      for (n = 0; n < nerr; ++n) {

	  ec = (struct econn *) evts[n].data.ptr;

	  if (ec == NULL) {
	      fprintf (stderr, "fatal: NULL econn\n");
	      exit (1);
	    }

	  if (evts[n].events & EPOLLERR) {
	      /* it can occur from time to time */
	      int error = 0;
	      socklen_t errlen = sizeof (error);
	      if (getsockopt
		  (fd, SOL_SOCKET, SO_ERROR, (void *) &error, &errlen) == 0)
		{
		  fprintf (stderr, "error = %s\n", strerror (error));
		}
	      exit (1);
	    }

	  if (evts[n].events & EPOLLHUP) {
	      /* happens on HTTP 1.0 */
	      fprintf (stderr, "EPOLLHUP\n");
	      exit (1);
	    }

	  if (evts[n].events & EPOLLOUT) {

	      ret =
		send (ec->fd, outbuf + ec->offs, outbufsize - ec->offs, 0);

	      if (ret == -1 && errno != EAGAIN) {
		  /* Bad but straightforward */
		  perror ("send");
		  exit (1);
		}

	      if (ret > 0) {

		  if (debug & DEBUG_REQUEST)
		    write (2, outbuf + ec->offs, outbufsize - ec->offs);

		  ec->offs += ret;

		  /* write done | schedule read */
		  if (ec->offs == outbufsize) {

		      evts[n].events = EPOLLIN;
		      evts[n].data.ptr = ec;

		      ec->offs = 0;

		      if (epoll_ctl (efd, EPOLL_CTL_MOD, ec->fd, evts + n))
			{
			  perror ("epoll_ctl");
			  exit (1);
			}
		    }
		}

	    }
	  else if (evts[n].events & EPOLLIN)
	    {

	      for (;;) {

		  ret = recv (ec->fd, inbuf, sizeof (inbuf), 0);

		  if (ret == -1 && errno != EAGAIN) {
		      perror ("recv");
		      exit (1);
		    }

		  if (ret <= 0)
		    break;

		  if (ec->offs <= 9 && ec->offs + ret > 10)
		    {

		      c = inbuf[9 - ec->offs];

		      if (c == '4' || c == '5')
			ec->flags |= BADREQUEST;
		    }

		  if (debug & DEBUG_RESPONSE)
		    {
		      int res = write (2, inbuf, ret);
		      if (res < 0)
			perror ("Unable to write to stderr");
		    }

		  ec->offs += ret;

		}

	      if (!ret) {

		  close (ec->fd);

		  m = __sync_fetch_and_add (&numrequests, 1);

		  if (maxrequests && m + 1 > maxrequests)
		    __sync_fetch_and_sub (&numrequests, 1);

		  else if (ec->flags & BADREQUEST)
		    __sync_fetch_and_add (&badrequests, 1);

		  else
		    __sync_fetch_and_add (&goodrequests, 1);

		  if (maxrequests && m + 1 >= maxrequests)
		    {
		      end_time ();
		      return NULL;
		    }

		  if (ticks && m % ticks == 0)
		    printf ("%d requests\n", m);

		  init_con (efd, ec);
		}
	    }
	}
    }
}

void signal_exit (int signal) {
  exit_i++;
}

static void usage () {
  printf ("Usage: htstress [options] [website]\n"
	  "Options:\n"
	  "   -n, --number       total number of requests (0 for inifinite, Ctrl-C to abort)\n"
	  "   -c, --concurrency  number of concurrent connections\n"
	  "   -t, --threads      number of threads)\n"
	  "   -u, --udaddr       path to unix domain socket\n"
	  "   -h, --host         host to use for http request\n");
  exit (0);
}

int main (int argc, char *argv[])
{
  char *rq, *s;
  double delta, rps;
  int nextoption;
  int n;
  pthread_t uselessthread;
  char *host = NULL;
  char *node = NULL;
  char *port = "http";
  struct hostent *h;
  struct sockaddr_in *ssin = (struct sockaddr_in *) &sss;
  struct sockaddr_in6 *ssin6 = (struct sockaddr_in6 *) &sss;
  struct sockaddr_un *ssun = (struct sockaddr_un *) &sss;
  struct addrinfo *result, *rp;
  struct addrinfo hints;
  int j, testfd;

  sighandler_t ret;
  ret = signal (SIGINT, signal_exit);

  if (ret == SIG_ERR)
    {
      perror ("signal(SIGINT, handler)");
      exit (0);
    }

  ret = signal (SIGTERM, signal_exit);

  if (ret == SIG_ERR)
    {
      perror ("signal(SIGTERM, handler)");
      exit (0);
    }

  memset (&hints, 0, sizeof (struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;

  memset (&sss, 0, sizeof (struct sockaddr_storage));

  if (argc == 1)
    usage ();

  do
    {
      nextoption =
	getopt_long (argc, argv, short_options, long_options, NULL);

      switch (nextoption)
	{
	case 'n':
	  maxrequests = strtoull (optarg, 0, 10);
	  break;
	case 'c':
	  concurrency = atoi (optarg);
	  break;
	case 't':
	  number_of_threads = atoi (optarg);
	  break;
	case '4':
	  hints.ai_family = PF_INET;
	  break;
	case '6':
	  hints.ai_family = PF_INET6;
	  break;
	case '%':
	  usage ();
	case -1:
	  break;
	case 'u':
	  udaddr = optarg;
	  break;
	case 'h':
	  host = optarg;
	  break;
	default:
	  printf ("Unexpected argument: '%c'\n", nextoption);
	  return 1;
	}
    }
  while (nextoption != -1);

  if (optind >= argc)
    {
      printf ("Missing URL\n");
      return 1;
    }

  /* parse URL */
  s = argv[optind];

  if (!strncmp (s, HTTP_PREFIX, sizeof (HTTP_PREFIX) - 1))
    s += (sizeof (HTTP_PREFIX) - 1);

  node = s;

  rq = strpbrk (s, ":/");

  if (rq == NULL)
    rq = "/";

  else if (*rq == '/')
    {
      node = strndup (s, rq - s);
      if (node == NULL)
	{
	  perror ("node = strndup(s, rq - s)");
	  exit (EXIT_FAILURE);
	}

    }
  else if (*rq == ':')
    {
      *rq++ = 0;
      port = rq;
      rq = strchr (rq, '/');
      if (*rq == '/')
	{
	  port = strndup (port, rq - port);
	  if (port == NULL)
	    {
	      perror ("port = strndup(rq, rq - port)");
	      exit (EXIT_FAILURE);
	    }
	}
      else
	rq = "/";
    }

  if (strnlen (udaddr, sizeof (ssun->sun_path) - 1) == 0)
    {
      j = getaddrinfo (node, port, &hints, &result);
      if (j != 0)
	{
	  fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (j));
	  exit (EXIT_FAILURE);
	}

      for (rp = result; rp != NULL; rp = rp->ai_next)
	{
	  testfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
	  if (testfd == -1)
	    continue;

	  if (connect (testfd, rp->ai_addr, rp->ai_addrlen) == 0)
	    {
	      close (testfd);
	      break;
	    }

	  close (testfd);
	}

      if (rp == NULL)
	{			/* No address succeeded */
	  fprintf (stderr, "getaddrinfo failed\n");
	  exit (EXIT_FAILURE);
	}

      if (rp->ai_addr->sa_family == PF_INET)
	{
	  *ssin = *(struct sockaddr_in *) rp->ai_addr;
	}
      else if (rp->ai_addr->sa_family == PF_INET6)
	{
	  *ssin6 = *(struct sockaddr_in6 *) rp->ai_addr;
	}
      else
	{
	  fprintf (stderr, "invalid family %d from getaddrinfo\n",
		   rp->ai_addr->sa_family);
	  exit (EXIT_FAILURE);
	}

      sssln = rp->ai_addrlen;

      freeaddrinfo (result);
    }
  else
    {
      ssun->sun_family = PF_UNIX;
      ssun->sun_path;
      strncpy (ssun->sun_path, udaddr, sizeof (ssun->sun_path) - 1);
      sssln = sizeof (struct sockaddr_un);
    }

  /* prepare request buffer */
  if (host == NULL)
    host = node;
  outbuf = malloc (strlen (rq) + sizeof (HTTP_REQUEST) + strlen (host));
  outbufsize = sprintf (outbuf, HTTP_REQUEST, rq, host);

  ticks = maxrequests / 10;

  signal (SIGINT, &sig_handler);

  if (!maxrequests)
    {
      ticks = 1000;
      printf ("[Press Ctrl-C to finish]\n");
    }

  start_time ();

  /* run test */
  for (n = 0; n < number_of_threads - 1; ++n)
    pthread_create (&uselessthread, 0, &worker, 0);

  worker (0);

  /* output result */
  delta =
    tve.tv_sec - tv.tv_sec + ((double) (tve.tv_usec - tv.tv_usec)) / 1e6;

  printf ("\n"
	  "requests:      %" PRIu64 "\n"
	  "good requests: %" PRIu64 " [%d%%]\n"
	  "bad requests:  %" PRIu64 " [%d%%]\n"
	  "seconds:       %.3f\n"
	  "requests/sec:  %.3f\n"
	  "\n",
	  numrequests,
	  goodrequests,
	  (int) (numrequests ? goodrequests * 100 / numrequests : 0),
	  badrequests,
	  (int) (numrequests ? badrequests * 100 / numrequests : 0), delta,
	  delta > 0 ? maxrequests / delta : 0);

  return 0;
}

