/*
 *  tvheadend, TCP common functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "tvheadend.h"
#include "tcp.h"
#include "tvhpoll.h"
#include "notify.h"
#include "access.h"

int tcp_preferred_address_family = AF_INET;
int tcp_server_running;
th_pipe_t tcp_server_pipe;

/**
 *
 */
int
tcp_connect(const char *hostname, int port, const char *bindaddr,
            char *errbuf, size_t errbufsize, int timeout)
{
  int fd, r, res, err;
  struct addrinfo *ai;
  char portstr[6];
  socklen_t errlen = sizeof(err);

  snprintf(portstr, 6, "%u", port);
  res = getaddrinfo(hostname, portstr, NULL, &ai);
  
  if (res != 0) {
    snprintf(errbuf, errbufsize, "%s", gai_strerror(res));
    return -1;
  }

  fd = tvh_socket(ai->ai_family, SOCK_STREAM, 0);
  if(fd == -1) {
    snprintf(errbuf, errbufsize, "Unable to create socket: %s",
	     strerror(errno));
    freeaddrinfo(ai);
    return -1;
  }

  /**
   * Switch to nonblocking
   */
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

  if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
    if (bindaddr && bindaddr[0] != '\0') {
      struct sockaddr_storage ip;
      memset(&ip, 0, sizeof(ip));
      ip.ss_family = ai->ai_family;
      if (inet_pton(AF_INET, bindaddr, IP_IN_ADDR(ip)) <= 0 ||
          bind(fd, (struct sockaddr *)&ip, IP_IN_ADDRLEN(ip)) < 0) {
        snprintf(errbuf, errbufsize, "Cannot bind to IPv%s addr '%s'", bindaddr,
                                     ai->ai_family == AF_INET6 ? "6" : "4");
        freeaddrinfo(ai);
        return -1;
      }
    }
  } else {
    snprintf(errbuf, errbufsize, "Invalid protocol family");
    freeaddrinfo(ai);
    return -1;
  }

  r = connect(fd, ai->ai_addr, ai->ai_addrlen);
  freeaddrinfo(ai);

  if(r == -1) {
    /* timeout < 0 - do not wait at all */
    if(errno == EINPROGRESS && timeout < 0) {
      err = 0;
    } else if(errno == EINPROGRESS) {
      tvhpoll_event_t ev;
      tvhpoll_t *efd;

      efd = tvhpoll_create(1);
      memset(&ev, 0, sizeof(ev));
      ev.events   = TVHPOLL_OUT;
      ev.fd       = fd;
      ev.data.ptr = &fd;
      tvhpoll_add(efd, &ev, 1);

      /* minimal timeout is one second */
      if (timeout < 1)
        timeout = 0;

      while (1) {
        if (!tvheadend_running) {
          errbuf[0] = '\0';
          tvhpoll_destroy(efd);
          close(fd);
          return -1;
        }

        r = tvhpoll_wait(efd, &ev, 1, timeout * 1000);
        if (r > 0)
          break;
        
        if (r == 0) { /* Timeout */
          snprintf(errbuf, errbufsize, "Connection attempt timed out");
          tvhpoll_destroy(efd);
          close(fd);
          return -1;
        }
      
        if (!ERRNO_AGAIN(errno)) {
          snprintf(errbuf, errbufsize, "poll() error: %s", strerror(errno));
          tvhpoll_destroy(efd);
          close(fd);
          return -1;
        }
      }
      
      tvhpoll_destroy(efd);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen);
    } else {
      err = errno;
    }
  } else {
    err = 0;
  }

  if(err != 0) {
    snprintf(errbuf, errbufsize, "%s", strerror(err));
    close(fd);
    return -1;
  }
  
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);


  /* Set the keep-alive active */
  err = 1;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&err, errlen);

  return fd;
}


/**
 *
 */
int
tcp_write_queue(int fd, htsbuf_queue_t *q)
{
  htsbuf_data_t *hd;
  int l, r = 0;
  void *p;

  while((hd = TAILQ_FIRST(&q->hq_q)) != NULL) {
    if (!r) {
      l = hd->hd_data_len - hd->hd_data_off;
      p = hd->hd_data + hd->hd_data_off;
      r = tvh_write(fd, p, l);
    }
    htsbuf_data_free(q, hd);
  }
  q->hq_size = 0;
  return r;
}


/**
 *
 */
static int
tcp_fill_htsbuf_from_fd(int fd, htsbuf_queue_t *hq)
{
  htsbuf_data_t *hd = TAILQ_LAST(&hq->hq_q, htsbuf_data_queue);
  int c;

  if(hd != NULL) {
    /* Fill out any previous buffer */
    c = hd->hd_data_size - hd->hd_data_len;

    if(c > 0) {

      c = read(fd, hd->hd_data + hd->hd_data_len, c);
      if(c < 1)
	return -1;

      hd->hd_data_len += c;
      hq->hq_size += c;
      return 0;
    }
  }
  
  hd = malloc(sizeof(htsbuf_data_t));
  
  hd->hd_data_size = 1000;
  hd->hd_data = malloc(hd->hd_data_size);

  c = read(fd, hd->hd_data, hd->hd_data_size);
  if(c < 1) {
    free(hd->hd_data);
    free(hd);
    return -1;
  }
  hd->hd_data_len = c;
  hd->hd_data_off = 0;
  TAILQ_INSERT_TAIL(&hq->hq_q, hd, hd_link);
  hq->hq_size += c;
  return 0;
}


/**
 *
 */
char *
tcp_read_line(int fd, htsbuf_queue_t *spill)
{
  int len;
  char *buf;

  do {
    len = htsbuf_find(spill, 0xa);

    if(len == -1) {
      if(tcp_fill_htsbuf_from_fd(fd, spill) < 0)
        return NULL;
    }
  } while (len == -1);

  buf = malloc(len+1);
  
  htsbuf_read(spill, buf, len);
  buf[len] = 0;
  while(len > 0 && buf[len - 1] < 32)
    buf[--len] = 0;
  htsbuf_drop(spill, 1); /* Drop the \n */
  return buf;
}



/**
 *
 */
int
tcp_read_data(int fd, char *buf, const size_t bufsize, htsbuf_queue_t *spill)
{
  int x, tot = htsbuf_read(spill, buf, bufsize);

  if(tot == bufsize)
    return 0;

  x = recv(fd, buf + tot, bufsize - tot, MSG_WAITALL);
  if(x != bufsize - tot)
    return -1;

  return 0;
}

/**
 *
 */
int
tcp_read(int fd, void *buf, size_t len)
{
  int x = recv(fd, buf, len, MSG_WAITALL);

  if(x == -1)
    return errno;
  if(x != len)
    return ECONNRESET;
  return 0;

}

/**
 *
 */
int
tcp_read_timeout(int fd, void *buf, size_t len, int timeout)
{
  int x, tot = 0;
  struct pollfd fds;

  assert(timeout > 0);

  fds.fd = fd;
  fds.events = POLLIN;
  fds.revents = 0;

  while(tot != len) {

    x = poll(&fds, 1, timeout);
    if(x == 0)
      return ETIMEDOUT;
    if(x == -1) {
      if (!tvheadend_running)
        return ECONNRESET;
      if (ERRNO_AGAIN(errno))
        continue;
      return errno;
    }

    x = recv(fd, buf + tot, len - tot, MSG_DONTWAIT);
    if(x == -1) {
      if(ERRNO_AGAIN(errno))
        continue;
      return errno;
    }

    if(x == 0)
      return ECONNRESET;

    tot += x;
  }

  return 0;

}

/**
 *
 */
char *
tcp_get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen)
{
  if(sa == NULL || s == NULL)
    return NULL;

  switch(sa->sa_family)
  {
    case AF_INET:
      inet_ntop(AF_INET, &(((struct sockaddr_in*)sa)->sin_addr), s, maxlen);
      break;
    case AF_INET6:
      inet_ntop(AF_INET6, &(((struct sockaddr_in6*)sa)->sin6_addr), s, maxlen);
      break;
    default:
      strncpy(s, "Unknown AF", maxlen);
      return NULL;
  }

  return s;
}

/**
 *
 */
static tvhpoll_t *tcp_server_poll;
static uint32_t tcp_server_launch_id;

typedef struct tcp_server {
  int serverfd;
  tcp_server_ops_t ops;
  void *opaque;
} tcp_server_t;

typedef struct tcp_server_launch {
  pthread_t tid;
  uint32_t id;
  int fd;
  tcp_server_ops_t ops;
  void *opaque;
  char *representative;
  void (*status) (void *opaque, htsmsg_t *m);
  struct sockaddr_storage peer;
  struct sockaddr_storage self;
  time_t started;
  LIST_ENTRY(tcp_server_launch) link;
  LIST_ENTRY(tcp_server_launch) alink;
  LIST_ENTRY(tcp_server_launch) jlink;
} tcp_server_launch_t;

static LIST_HEAD(, tcp_server_launch) tcp_server_launches = { 0 };
static LIST_HEAD(, tcp_server_launch) tcp_server_active = { 0 };
static LIST_HEAD(, tcp_server_launch) tcp_server_join = { 0 };

/**
 *
 */
void *
tcp_connection_launch
  (int fd, void (*status) (void *opaque, htsmsg_t *m), access_t *aa)
{
  tcp_server_launch_t *tsl, *res;
  uint32_t used = 0;
  time_t started = dispatch_clock;

  lock_assert(&global_lock);

  assert(status);

  if (aa == NULL)
    return NULL;

try_again:
  res = NULL;
  LIST_FOREACH(tsl, &tcp_server_active, alink) {
    if (tsl->fd == fd) {
      res = tsl;
      if (!aa->aa_conn_limit)
        break;
      continue;
    }
    if (!strcmp(aa->aa_representative ?: "", tsl->representative ?: ""))
      used++;
  }
  if (res == NULL)
    return NULL;

  if (aa->aa_conn_limit && used >= aa->aa_conn_limit) {
    if (started + 3 < dispatch_clock) {
      tvherror("tcp", "multiple connections are not allowed for user '%s' from '%s' (limit %u)",
               aa->aa_username ?: "", aa->aa_representative ?: "", aa->aa_conn_limit);
      return NULL;
    }
    pthread_mutex_unlock(&global_lock);
    usleep(250000);
    pthread_mutex_lock(&global_lock);
    if (tvheadend_running)
      goto try_again;
    return NULL;
  }

  res->representative = aa->aa_representative ? strdup(aa->aa_representative) : NULL;
  res->status = status;
  LIST_INSERT_HEAD(&tcp_server_launches, res, link);
  notify_reload("connections");
  return res;
}

/**
 *
 */
void
tcp_connection_land(void *tcp_id)
{
  tcp_server_launch_t *tsl = tcp_id;

  lock_assert(&global_lock);

  if (tsl == NULL)
    return;

  LIST_REMOVE(tsl, link);
  notify_reload("connections");

  free(tsl->representative);
  tsl->representative = NULL;
}

/**
 *
 */
void
tcp_connection_cancel(uint32_t id)
{
  tcp_server_launch_t *tsl;

  lock_assert(&global_lock);

  LIST_FOREACH(tsl, &tcp_server_active, alink)
    if (tsl->id == id) {
      if (tsl->ops.cancel)
        tsl->ops.cancel(tsl->opaque);
      break;
    }
}

/*
 *
 */
static void *
tcp_server_start(void *aux)
{
  tcp_server_launch_t *tsl = aux;
  struct timeval to;
  int val;
  char c = 'J';

  val = 1;
  setsockopt(tsl->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
  
#ifdef TCP_KEEPIDLE
  val = 30;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
#endif

#ifdef TCP_KEEPINVL
  val = 15;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
#endif

#ifdef TCP_KEEPCNT
  val = 5;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
#endif

  val = 1;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

  to.tv_sec  = 30;
  to.tv_usec =  0;
  setsockopt(tsl->fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

  /* Start */
  time(&tsl->started);
  pthread_mutex_lock(&global_lock);
  tsl->id = ++tcp_server_launch_id;
  if (!tsl->id) tsl->id = ++tcp_server_launch_id;
  tsl->ops.start(tsl->fd, &tsl->opaque, &tsl->peer, &tsl->self);

  /* Stop */
  if (tsl->ops.stop) tsl->ops.stop(tsl->opaque);
  LIST_REMOVE(tsl, alink);
  LIST_INSERT_HEAD(&tcp_server_join, tsl, jlink);
  pthread_mutex_unlock(&global_lock);
  tvh_write(tcp_server_pipe.wr, &c, 1);
  return NULL;
}


/**
 *
 */
static void *
tcp_server_loop(void *aux)
{
  int r;
  tvhpoll_event_t ev;
  tcp_server_t *ts;
  tcp_server_launch_t *tsl;
  socklen_t slen;
  char c;

  while(tcp_server_running) {
    r = tvhpoll_wait(tcp_server_poll, &ev, 1, -1);
    if(r == -1) {
      perror("tcp_server: tvhpoll_wait");
      continue;
    }

    if (r == 0) continue;

    if (ev.data.ptr == &tcp_server_pipe) {
      r = read(tcp_server_pipe.rd, &c, 1);
      if (r > 0) {
next:
        pthread_mutex_lock(&global_lock);
        while ((tsl = LIST_FIRST(&tcp_server_join)) != NULL) {
          LIST_REMOVE(tsl, jlink);
          pthread_mutex_unlock(&global_lock);
          pthread_join(tsl->tid, NULL);
          free(tsl);
          goto next;
        }
        pthread_mutex_unlock(&global_lock);
      }
      continue;
    }

    ts = ev.data.ptr;

    if(ev.events & TVHPOLL_HUP) {
      close(ts->serverfd);
      free(ts);
      continue;
    } 

    if(ev.events & TVHPOLL_IN) {
      tsl = malloc(sizeof(tcp_server_launch_t));
      tsl->ops            = ts->ops;
      tsl->opaque         = ts->opaque;
      tsl->status         = NULL;
      tsl->representative = NULL;
      slen = sizeof(struct sockaddr_storage);

      tsl->fd = accept(ts->serverfd, 
                       (struct sockaddr *)&tsl->peer, &slen);
      if(tsl->fd == -1) {
     	perror("accept");
     	free(tsl);
     	sleep(1);
     	continue;
      }

      slen = sizeof(struct sockaddr_storage);
      if(getsockname(tsl->fd, (struct sockaddr *)&tsl->self, &slen)) {
        close(tsl->fd);
        free(tsl);
        continue;
      }

      pthread_mutex_lock(&global_lock);
      LIST_INSERT_HEAD(&tcp_server_active, tsl, alink);
      pthread_mutex_unlock(&global_lock);
      tvhthread_create(&tsl->tid, NULL, tcp_server_start, tsl);
    }
  }
  tvhtrace("tcp", "server thread finished");
  return NULL;
}

/**
 *
 */
void *
tcp_server_create
  (const char *bindaddr, int port, tcp_server_ops_t *ops, void *opaque)
{
  int fd, x;
  tcp_server_t *ts;
  struct addrinfo hints, *res, *ressave, *use = NULL;
  char port_buf[6];
  int one = 1;
  int zero = 0;

  snprintf(port_buf, 6, "%d", port);

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  if (bindaddr != NULL)
      hints.ai_flags |= AI_NUMERICHOST;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  x = getaddrinfo(bindaddr, port_buf, &hints, &res);

  if(x != 0) {
    tvhlog(LOG_ERR, "tcp", "getaddrinfo: %s: %s", bindaddr != NULL ? bindaddr : "*",
      x == EAI_SYSTEM ? strerror(errno) : gai_strerror(x));
    return NULL;
  }

  ressave = res;
  while(res) {
    if(res->ai_family == tcp_preferred_address_family) {
      use = res;
      break;
    } else if(use == NULL) {
      use = res;
    }
    res = res->ai_next;
  }

  fd = tvh_socket(use->ai_family, use->ai_socktype, use->ai_protocol);
  if(fd == -1) {
    freeaddrinfo(ressave);
    return NULL;
  }

  if(use->ai_family == AF_INET6)
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(int));

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
  
  x = bind(fd, use->ai_addr, use->ai_addrlen);
  freeaddrinfo(ressave);

  if(x != 0)
  {
    tvhlog(LOG_ERR, "tcp", "bind: %s: %s", bindaddr != NULL ? bindaddr : "*", strerror(errno));
    close(fd);
    return NULL;
  }

  listen(fd, 1);

  ts = malloc(sizeof(tcp_server_t));
  ts->serverfd = fd;
  ts->ops    = *ops;
  ts->opaque = opaque;
  return ts;
}


/**
 *
 */

void tcp_server_register(void *server)
{
  tcp_server_t *ts = server;
  tvhpoll_event_t ev;

  if (ts == NULL)
    return;

  memset(&ev, 0, sizeof(ev));

  ev.fd       = ts->serverfd;
  ev.events   = TVHPOLL_IN;
  ev.data.ptr = ts;
  tvhpoll_add(tcp_server_poll, &ev, 1);
}

/**
 *
 */
void
tcp_server_delete(void *server)
{
  tcp_server_t *ts = server;
  tvhpoll_event_t ev;

  if (server == NULL)
    return;

  memset(&ev, 0, sizeof(ev));
  ev.fd       = ts->serverfd;
  ev.events   = TVHPOLL_IN;
  ev.data.ptr = ts;
  tvhpoll_rem(tcp_server_poll, &ev, 1);  
  free(ts);
}

/*
 * Connections status
 */
htsmsg_t *
tcp_server_connections ( void )
{
  tcp_server_launch_t *tsl;
  lock_assert(&global_lock);
  htsmsg_t *l, *e, *m;
  char buf[1024];
  int c = 0;
  
  /* Build list */
  l = htsmsg_create_list();
  LIST_FOREACH(tsl, &tcp_server_launches, link) {
    if (!tsl->status) continue;
    c++;
    e = htsmsg_create_map();
    tcp_get_ip_str((struct sockaddr*)&tsl->peer, buf, sizeof(buf));
    htsmsg_add_u32(e, "id", tsl->id);
    htsmsg_add_str(e, "peer", buf);
    htsmsg_add_s64(e, "started", tsl->started);
    tsl->status(tsl->opaque, e);
    htsmsg_add_msg(l, NULL, e);
  }

  /* Output */
  m = htsmsg_create_map();
  htsmsg_add_msg(m, "entries", l);
  htsmsg_add_u32(m, "totalCount", c);
  return m;
}

/**
 *
 */
pthread_t tcp_server_tid;

void
tcp_server_preinit(int opt_ipv6)
{
  if(opt_ipv6)
    tcp_preferred_address_family = AF_INET6;
}

void
tcp_server_init(void)
{
  tvhpoll_event_t ev;
  tvh_pipe(O_NONBLOCK, &tcp_server_pipe);
  tcp_server_poll = tvhpoll_create(10);

  memset(&ev, 0, sizeof(ev));
  ev.fd       = tcp_server_pipe.rd;
  ev.events   = TVHPOLL_IN;
  ev.data.ptr = &tcp_server_pipe;
  tvhpoll_add(tcp_server_poll, &ev, 1);

  tcp_server_running = 1;
  tvhthread_create(&tcp_server_tid, NULL, tcp_server_loop, NULL);
}

void
tcp_server_done(void)
{
  tcp_server_launch_t *tsl;  
  char c = 'E';

  tcp_server_running = 0;
  tvh_write(tcp_server_pipe.wr, &c, 1);

  pthread_mutex_lock(&global_lock);
  LIST_FOREACH(tsl, &tcp_server_active, alink) {
    if (tsl->ops.cancel)
      tsl->ops.cancel(tsl->opaque);
    if (tsl->fd >= 0)
      close(tsl->fd);
    tsl->fd = -1;
    pthread_kill(tsl->tid, SIGTERM);
  }
  pthread_mutex_unlock(&global_lock);

  pthread_join(tcp_server_tid, NULL);
  tvh_pipe_close(&tcp_server_pipe);
  tvhpoll_destroy(tcp_server_poll);
  
  while (LIST_FIRST(&tcp_server_active) != NULL)
    usleep(20000);
  pthread_mutex_lock(&global_lock);
  while ((tsl = LIST_FIRST(&tcp_server_join)) != NULL) {
    LIST_REMOVE(tsl, jlink);
    pthread_mutex_unlock(&global_lock);
    pthread_join(tsl->tid, NULL);
    free(tsl);
    pthread_mutex_lock(&global_lock);
  }
  pthread_mutex_unlock(&global_lock);
}
