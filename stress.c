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

struct sockaddr_in ssin;

// Initialize them
int concurrency = 1;
int number_of_threads = 1;

//volatile them, the numbers can go up to gazilion
volatile uint64_t numrequests = 0;
volatile uint64_t maxrequests = 0;
volatile uint64_t goodrequests = 0;
volatile uint64_t badrequests = 0;
volatile uint64_t inbytes = 0;
volatile uint64_t outbytes = 0;

uint64_t ticks;

// TODO: Add debug flag
int debug = 0;

struct timeval tv, tve;

static const char short_options[] = "n:c:t";

static const struct option long_options[] = {
  {"number", 1, NULL, 'n'},
  {"concurrency", 1, NULL, 'c'},
  {"threads", 0, NULL, 't'},
  {NULL, 0, NULL, 0}
};

static void sig_handler (int arg)
{
  maxrequests = numrequests;
}

static void start_time ()
{
  if (gettimeofday (&tv, NULL))
    {
      perror ("gettimeofday");
      exit (1);
    }
}

static void end_time ()
{
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

  ec->fd = socket (AF_INET, SOCK_STREAM, 0);
  ec->offs = 0;
  ec->flags = 0;

  if (ec->fd == -1)
    {
      perror ("socket() failed");
      exit (1);
    }

  fcntl (ec->fd, F_SETFL, O_NONBLOCK);

  ret = connect (ec->fd, (struct sockaddr *) &ssin, sizeof (ssin));

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

  if (efd == -1)
    {
      perror ("epoll");
      exit (1);
    }

  for (n = 0; n < concurrency; ++n)
    init_con (efd, ecs + n);

  for (;;)
    {

      nerr = epoll_wait (efd, evts, sizeof (evts) / sizeof (evts[0]), -1);

      if (nerr == -1)
	{
	  perror ("epoll_wait");
	  exit (1);
	}

      for (n = 0; n < nerr; ++n)
	{

	  ec = (struct econn *) evts[n].data.ptr;

	  if (ec == NULL)
	    {
	      fprintf (stderr, "fatal: NULL econn\n");
	      exit (1);
	    }

	  if (evts[n].events & (EPOLLHUP | EPOLLERR))
	    {
	      /* it can occur from time to time */
	      fprintf (stderr, "broken connection");
	      exit (1);
	    }

	  if (evts[n].events & EPOLLOUT)
	    {

	      ret =
		send (ec->fd, outbuf + ec->offs, outbufsize - ec->offs, 0);

	      if (ret == -1 && errno != EAGAIN)
		{
		  /* Bad but straightforward */
		  perror ("send");
		  exit (1);
		}

	      if (ret > 0)
		{

		  if (debug & DEBUG_REQUEST)
		    write (2, outbuf + ec->offs, outbufsize - ec->offs);

		  ec->offs += ret;

		  /* write done | schedule read */
		  if (ec->offs == outbufsize)
		    {

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

	      for (;;)
		{

		  ret = recv (ec->fd, inbuf, sizeof (inbuf), 0);

		  if (ret == -1 && errno != EAGAIN)
		    {
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
		    write (2, inbuf, ret);

		  ec->offs += ret;

		}

	      if (!ret)
		{

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

static void usage () {
  printf ("Usage: htstress [options] [website]\n"
	  "Options:\n"
	  "   -n, --number       total number of requests (0 for inifinite, Ctrl-C to abort)\n"
	  "   -c, --concurrency  number of concurrent connections\n"
	  "   -t, --threads      number of threads)\n");
  exit (0);
}

int main (int argc, char *argv[]) {
  char *rq, *s;
  double delta, rps;
  int nextoption;
  int n;
  pthread_t uselessthread;
  int port = 80;
  char *host = NULL;
  struct hostent *h;

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
	case '%':
	  usage ();
	case -1:
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

  host = s;

  rq = strpbrk (s, ":/");

  if (rq == NULL)
    rq = "/";

  else if (*rq == '/')
    {
      host = malloc (rq - s);
      memcpy (host, rq, rq - s);

    }
  else if (*rq == ':')
    {
      *rq++ = 0;
      port = atoi (rq);
      rq = strchr (rq, '/');
      if (rq == NULL)
	rq = "/";
    }

  h = gethostbyname (host);
  if (!h || !h->h_length)
    {
      printf ("gethostbyname failed | bad URL maybe?\n");
      return 1;
    }

  ssin.sin_addr.s_addr = *(u_int32_t *) h->h_addr;
  ssin.sin_family = PF_INET;
  ssin.sin_port = htons (port);

  /* prepare request buffer */
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
