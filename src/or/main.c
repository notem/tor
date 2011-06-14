/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2011, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file main.c
 * \brief Toplevel module. Handles signals, multiplexes between
 * connections, implements main loop, and drives scheduled events.
 **/

#define MAIN_PRIVATE
#include "or.h"
#include "buffers.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "circuituse.h"
#include "command.h"
#include "config.h"
#include "connection.h"
#include "connection_edge.h"
#include "connection_or.h"
#include "control.h"
#include "cpuworker.h"
#include "directory.h"
#include "dirserv.h"
#include "dirvote.h"
#include "dns.h"
#include "dnsserv.h"
#include "geoip.h"
#include "hibernate.h"
#include "main.h"
#include "microdesc.h"
#include "networkstatus.h"
#include "nodelist.h"
#include "ntmain.h"
#include "onion.h"
#include "policies.h"
#include "relay.h"
#include "rendclient.h"
#include "rendcommon.h"
#include "rendservice.h"
#include "rephist.h"
#include "router.h"
#include "routerlist.h"
#include "routerparse.h"
#include "status.h"
#ifdef USE_DMALLOC
#include <dmalloc.h>
#include <openssl/crypto.h>
#endif
#include "memarea.h"

#ifdef HAVE_EVENT2_EVENT_H
#include <event2/event.h>
#else
#include <event.h>
#endif

#ifdef USE_BUFFEREVENTS
#include <event2/bufferevent.h>
#endif

void evdns_shutdown(int);

/********* PROTOTYPES **********/

static void dumpmemusage(int severity);
static void dumpstats(int severity); /* log stats */
static void conn_read_callback(evutil_socket_t fd, short event, void *_conn);
static void conn_write_callback(evutil_socket_t fd, short event, void *_conn);
static void second_elapsed_callback(periodic_timer_t *timer, void *args);
static int conn_close_if_marked(int i);
static void connection_start_reading_from_linked_conn(connection_t *conn);
static int connection_should_read_from_linked_conn(connection_t *conn);

/********* START VARIABLES **********/

#ifndef USE_BUFFEREVENTS
int global_read_bucket; /**< Max number of bytes I can read this second. */
int global_write_bucket; /**< Max number of bytes I can write this second. */

/** Max number of relayed (bandwidth class 1) bytes I can read this second. */
int global_relayed_read_bucket;
/** Max number of relayed (bandwidth class 1) bytes I can write this second. */
int global_relayed_write_bucket;
/** What was the read bucket before the last second_elapsed_callback() call?
 * (used to determine how many bytes we've read). */
static int stats_prev_global_read_bucket;
/** What was the write bucket before the last second_elapsed_callback() call?
 * (used to determine how many bytes we've written). */
static int stats_prev_global_write_bucket;
#else
static uint64_t stats_prev_n_read = 0;
static uint64_t stats_prev_n_written = 0;
#endif

/* XXX we might want to keep stats about global_relayed_*_bucket too. Or not.*/
/** How many bytes have we read since we started the process? */
static uint64_t stats_n_bytes_read = 0;
/** How many bytes have we written since we started the process? */
static uint64_t stats_n_bytes_written = 0;
/** What time did this process start up? */
time_t time_of_process_start = 0;
/** How many seconds have we been running? */
long stats_n_seconds_working = 0;
/** When do we next launch DNS wildcarding checks? */
static time_t time_to_check_for_correct_dns = 0;

/** How often will we honor SIGNEWNYM requests? */
#define MAX_SIGNEWNYM_RATE 10
/** When did we last process a SIGNEWNYM request? */
static time_t time_of_last_signewnym = 0;
/** Is there a signewnym request we're currently waiting to handle? */
static int signewnym_is_pending = 0;

/** Smartlist of all open connections. */
static smartlist_t *connection_array = NULL;
/** List of connections that have been marked for close and need to be freed
 * and removed from connection_array. */
static smartlist_t *closeable_connection_lst = NULL;
/** List of linked connections that are currently reading data into their
 * inbuf from their partner's outbuf. */
static smartlist_t *active_linked_connection_lst = NULL;
/** Flag: Set to true iff we entered the current libevent main loop via
 * <b>loop_once</b>. If so, there's no need to trigger a loopexit in order
 * to handle linked connections. */
static int called_loop_once = 0;

/** We set this to 1 when we've opened a circuit, so we can print a log
 * entry to inform the user that Tor is working.  We set it to 0 when
 * we think the fact that we once opened a circuit doesn't mean we can do so
 * any longer (a big time jump happened, when we notice our directory is
 * heinously out-of-date, etc.
 */
int can_complete_circuit=0;

/** How often do we check for router descriptors that we should download
 * when we have too little directory info? */
#define GREEDY_DESCRIPTOR_RETRY_INTERVAL (10)
/** How often do we check for router descriptors that we should download
 * when we have enough directory info? */
#define LAZY_DESCRIPTOR_RETRY_INTERVAL (60)
/** How often do we 'forgive' undownloadable router descriptors and attempt
 * to download them again? */
#define DESCRIPTOR_FAILURE_RESET_INTERVAL (60*60)
/** How long do we let a directory connection stall before expiring it? */
#define DIR_CONN_MAX_STALL (5*60)

/** How long do we let OR connections handshake before we decide that
 * they are obsolete? */
#define TLS_HANDSHAKE_TIMEOUT (60)

/********* END VARIABLES ************/

/****************************************************************************
*
* This section contains accessors and other methods on the connection_array
* variables (which are global within this file and unavailable outside it).
*
****************************************************************************/

#if 0 && defined(USE_BUFFEREVENTS)
static void
free_old_inbuf(connection_t *conn)
{
  if (! conn->inbuf)
    return;

  tor_assert(conn->outbuf);
  tor_assert(buf_datalen(conn->inbuf) == 0);
  tor_assert(buf_datalen(conn->outbuf) == 0);
  buf_free(conn->inbuf);
  buf_free(conn->outbuf);
  conn->inbuf = conn->outbuf = NULL;

  if (conn->read_event) {
    event_del(conn->read_event);
    tor_event_free(conn->read_event);
  }
  if (conn->write_event) {
    event_del(conn->read_event);
    tor_event_free(conn->write_event);
  }
  conn->read_event = conn->write_event = NULL;
}
#endif

/** Add <b>conn</b> to the array of connections that we can poll on.  The
 * connection's socket must be set; the connection starts out
 * non-reading and non-writing.
 */
int
connection_add_impl(connection_t *conn, int is_connecting)
{
  tor_assert(conn);
  tor_assert(SOCKET_OK(conn->s) ||
             conn->linked ||
             (conn->type == CONN_TYPE_AP &&
              TO_EDGE_CONN(conn)->is_dns_request));

  tor_assert(conn->conn_array_index == -1); /* can only connection_add once */
  conn->conn_array_index = smartlist_len(connection_array);
  smartlist_add(connection_array, conn);

#ifdef USE_BUFFEREVENTS
  if (connection_type_uses_bufferevent(conn)) {
    if (SOCKET_OK(conn->s) && !conn->linked) {
      conn->bufev = bufferevent_socket_new(
                         tor_libevent_get_base(),
                         conn->s,
                         BEV_OPT_DEFER_CALLBACKS);
      if (!conn->bufev) {
        log_warn(LD_BUG, "Unable to create socket bufferevent");
        smartlist_del(connection_array, conn->conn_array_index);
        conn->conn_array_index = -1;
        return -1;
      }
      if (is_connecting) {
        /* Put the bufferevent into a "connecting" state so that we'll get
         * a "connected" event callback on successful write. */
        bufferevent_socket_connect(conn->bufev, NULL, 0);
      }
      connection_configure_bufferevent_callbacks(conn);
    } else if (conn->linked && conn->linked_conn &&
               connection_type_uses_bufferevent(conn->linked_conn)) {
      tor_assert(!(SOCKET_OK(conn->s)));
      if (!conn->bufev) {
        struct bufferevent *pair[2] = { NULL, NULL };
        if (bufferevent_pair_new(tor_libevent_get_base(),
                                 BEV_OPT_DEFER_CALLBACKS,
                                 pair) < 0) {
          log_warn(LD_BUG, "Unable to create bufferevent pair");
          smartlist_del(connection_array, conn->conn_array_index);
          conn->conn_array_index = -1;
          return -1;
        }
        tor_assert(pair[0]);
        conn->bufev = pair[0];
        conn->linked_conn->bufev = pair[1];
      } /* else the other side already was added, and got a bufferevent_pair */
      connection_configure_bufferevent_callbacks(conn);
    } else {
      tor_assert(!conn->linked);
    }

    if (conn->bufev)
      tor_assert(conn->inbuf == NULL);

    if (conn->linked_conn && conn->linked_conn->bufev)
      tor_assert(conn->linked_conn->inbuf == NULL);
  }
#else
  (void) is_connecting;
#endif

  if (!HAS_BUFFEREVENT(conn) && (SOCKET_OK(conn->s) || conn->linked)) {
    conn->read_event = tor_event_new(tor_libevent_get_base(),
         conn->s, EV_READ|EV_PERSIST, conn_read_callback, conn);
    conn->write_event = tor_event_new(tor_libevent_get_base(),
         conn->s, EV_WRITE|EV_PERSIST, conn_write_callback, conn);
    /* XXXX CHECK FOR NULL RETURN! */
  }

  log_debug(LD_NET,"new conn type %s, socket %d, address %s, n_conns %d.",
            conn_type_to_string(conn->type), (int)conn->s, conn->address,
            smartlist_len(connection_array));

  return 0;
}

/** Tell libevent that we don't care about <b>conn</b> any more. */
void
connection_unregister_events(connection_t *conn)
{
  if (conn->read_event) {
    if (event_del(conn->read_event))
      log_warn(LD_BUG, "Error removing read event for %d", (int)conn->s);
    tor_free(conn->read_event);
  }
  if (conn->write_event) {
    if (event_del(conn->write_event))
      log_warn(LD_BUG, "Error removing write event for %d", (int)conn->s);
    tor_free(conn->write_event);
  }
#ifdef USE_BUFFEREVENTS
  if (conn->bufev) {
    bufferevent_free(conn->bufev);
    conn->bufev = NULL;
  }
#endif
  if (conn->dns_server_port) {
    dnsserv_close_listener(conn);
  }
}

/** Remove the connection from the global list, and remove the
 * corresponding poll entry.  Calling this function will shift the last
 * connection (if any) into the position occupied by conn.
 */
int
connection_remove(connection_t *conn)
{
  int current_index;
  connection_t *tmp;

  tor_assert(conn);

  log_debug(LD_NET,"removing socket %d (type %s), n_conns now %d",
            (int)conn->s, conn_type_to_string(conn->type),
            smartlist_len(connection_array));

  tor_assert(conn->conn_array_index >= 0);
  current_index = conn->conn_array_index;
  connection_unregister_events(conn); /* This is redundant, but cheap. */
  if (current_index == smartlist_len(connection_array)-1) { /* at the end */
    smartlist_del(connection_array, current_index);
    return 0;
  }

  /* replace this one with the one at the end */
  smartlist_del(connection_array, current_index);
  tmp = smartlist_get(connection_array, current_index);
  tmp->conn_array_index = current_index;

  return 0;
}

/** If <b>conn</b> is an edge conn, remove it from the list
 * of conn's on this circuit. If it's not on an edge,
 * flush and send destroys for all circuits on this conn.
 *
 * Remove it from connection_array (if applicable) and
 * from closeable_connection_list.
 *
 * Then free it.
 */
static void
connection_unlink(connection_t *conn)
{
  connection_about_to_close_connection(conn);
  if (conn->conn_array_index >= 0) {
    connection_remove(conn);
  }
  if (conn->linked_conn) {
    conn->linked_conn->linked_conn = NULL;
    if (! conn->linked_conn->marked_for_close &&
        conn->linked_conn->reading_from_linked_conn)
      connection_start_reading(conn->linked_conn);
    conn->linked_conn = NULL;
  }
  smartlist_remove(closeable_connection_lst, conn);
  smartlist_remove(active_linked_connection_lst, conn);
  if (conn->type == CONN_TYPE_EXIT) {
    assert_connection_edge_not_dns_pending(TO_EDGE_CONN(conn));
  }
  if (conn->type == CONN_TYPE_OR) {
    if (!tor_digest_is_zero(TO_OR_CONN(conn)->identity_digest))
      connection_or_remove_from_identity_map(TO_OR_CONN(conn));
  }
  connection_free(conn);
}

/** Schedule <b>conn</b> to be closed. **/
void
add_connection_to_closeable_list(connection_t *conn)
{
  tor_assert(!smartlist_isin(closeable_connection_lst, conn));
  tor_assert(conn->marked_for_close);
  assert_connection_ok(conn, time(NULL));
  smartlist_add(closeable_connection_lst, conn);
}

/** Return 1 if conn is on the closeable list, else return 0. */
int
connection_is_on_closeable_list(connection_t *conn)
{
  return smartlist_isin(closeable_connection_lst, conn);
}

/** Return true iff conn is in the current poll array. */
int
connection_in_array(connection_t *conn)
{
  return smartlist_isin(connection_array, conn);
}

/** Set <b>*array</b> to an array of all connections, and <b>*n</b>
 * to the length of the array. <b>*array</b> and <b>*n</b> must not
 * be modified.
 */
smartlist_t *
get_connection_array(void)
{
  if (!connection_array)
    connection_array = smartlist_create();
  return connection_array;
}

/** Provides the traffic read and written over the life of the process. */

uint64_t
get_bytes_read(void)
{
  return stats_n_bytes_read;
}

uint64_t
get_bytes_written(void)
{
  return stats_n_bytes_written;
}

/** Set the event mask on <b>conn</b> to <b>events</b>.  (The event
 * mask is a bitmask whose bits are READ_EVENT and WRITE_EVENT)
 */
void
connection_watch_events(connection_t *conn, watchable_events_t events)
{
  IF_HAS_BUFFEREVENT(conn, {
      short ev = ((short)events) & (EV_READ|EV_WRITE);
      short old_ev = bufferevent_get_enabled(conn->bufev);
      if ((ev & ~old_ev) != 0) {
        bufferevent_enable(conn->bufev, ev);
      }
      if ((old_ev & ~ev) != 0) {
        bufferevent_disable(conn->bufev, old_ev & ~ev);
      }
      return;
  });
  if (events & READ_EVENT)
    connection_start_reading(conn);
  else
    connection_stop_reading(conn);

  if (events & WRITE_EVENT)
    connection_start_writing(conn);
  else
    connection_stop_writing(conn);
}

/** Return true iff <b>conn</b> is listening for read events. */
int
connection_is_reading(connection_t *conn)
{
  tor_assert(conn);

  IF_HAS_BUFFEREVENT(conn,
    return (bufferevent_get_enabled(conn->bufev) & EV_READ) != 0;
  );
  return conn->reading_from_linked_conn ||
    (conn->read_event && event_pending(conn->read_event, EV_READ, NULL));
}

/** Tell the main loop to stop notifying <b>conn</b> of any read events. */
void
connection_stop_reading(connection_t *conn)
{
  tor_assert(conn);

  IF_HAS_BUFFEREVENT(conn, {
      bufferevent_disable(conn->bufev, EV_READ);
      return;
  });

  tor_assert(conn->read_event);

  if (conn->linked) {
    conn->reading_from_linked_conn = 0;
    connection_stop_reading_from_linked_conn(conn);
  } else {
    if (event_del(conn->read_event))
      log_warn(LD_NET, "Error from libevent setting read event state for %d "
               "to unwatched: %s",
               (int)conn->s,
               tor_socket_strerror(tor_socket_errno(conn->s)));
  }
}

/** Tell the main loop to start notifying <b>conn</b> of any read events. */
void
connection_start_reading(connection_t *conn)
{
  tor_assert(conn);

  IF_HAS_BUFFEREVENT(conn, {
      bufferevent_enable(conn->bufev, EV_READ);
      return;
  });

  tor_assert(conn->read_event);

  if (conn->linked) {
    conn->reading_from_linked_conn = 1;
    if (connection_should_read_from_linked_conn(conn))
      connection_start_reading_from_linked_conn(conn);
  } else {
    if (event_add(conn->read_event, NULL))
      log_warn(LD_NET, "Error from libevent setting read event state for %d "
               "to watched: %s",
               (int)conn->s,
               tor_socket_strerror(tor_socket_errno(conn->s)));
  }
}

/** Return true iff <b>conn</b> is listening for write events. */
int
connection_is_writing(connection_t *conn)
{
  tor_assert(conn);

  IF_HAS_BUFFEREVENT(conn,
    return (bufferevent_get_enabled(conn->bufev) & EV_WRITE) != 0;
  );

  return conn->writing_to_linked_conn ||
    (conn->write_event && event_pending(conn->write_event, EV_WRITE, NULL));
}

/** Tell the main loop to stop notifying <b>conn</b> of any write events. */
void
connection_stop_writing(connection_t *conn)
{
  tor_assert(conn);

  IF_HAS_BUFFEREVENT(conn, {
      bufferevent_disable(conn->bufev, EV_WRITE);
      return;
  });

  tor_assert(conn->write_event);

  if (conn->linked) {
    conn->writing_to_linked_conn = 0;
    if (conn->linked_conn)
      connection_stop_reading_from_linked_conn(conn->linked_conn);
  } else {
    if (event_del(conn->write_event))
      log_warn(LD_NET, "Error from libevent setting write event state for %d "
               "to unwatched: %s",
               (int)conn->s,
               tor_socket_strerror(tor_socket_errno(conn->s)));
  }
}

/** Tell the main loop to start notifying <b>conn</b> of any write events. */
void
connection_start_writing(connection_t *conn)
{
  tor_assert(conn);

  IF_HAS_BUFFEREVENT(conn, {
      bufferevent_enable(conn->bufev, EV_WRITE);
      return;
  });

  tor_assert(conn->write_event);

  if (conn->linked) {
    conn->writing_to_linked_conn = 1;
    if (conn->linked_conn &&
        connection_should_read_from_linked_conn(conn->linked_conn))
      connection_start_reading_from_linked_conn(conn->linked_conn);
  } else {
    if (event_add(conn->write_event, NULL))
      log_warn(LD_NET, "Error from libevent setting write event state for %d "
               "to watched: %s",
               (int)conn->s,
               tor_socket_strerror(tor_socket_errno(conn->s)));
  }
}

/** Return true iff <b>conn</b> is linked conn, and reading from the conn
 * linked to it would be good and feasible.  (Reading is "feasible" if the
 * other conn exists and has data in its outbuf, and is "good" if we have our
 * reading_from_linked_conn flag set and the other conn has its
 * writing_to_linked_conn flag set.)*/
static int
connection_should_read_from_linked_conn(connection_t *conn)
{
  if (conn->linked && conn->reading_from_linked_conn) {
    if (! conn->linked_conn ||
        (conn->linked_conn->writing_to_linked_conn &&
         buf_datalen(conn->linked_conn->outbuf)))
      return 1;
  }
  return 0;
}

/** Helper: Tell the main loop to begin reading bytes into <b>conn</b> from
 * its linked connection, if it is not doing so already.  Called by
 * connection_start_reading and connection_start_writing as appropriate. */
static void
connection_start_reading_from_linked_conn(connection_t *conn)
{
  tor_assert(conn);
  tor_assert(conn->linked == 1);

  if (!conn->active_on_link) {
    conn->active_on_link = 1;
    smartlist_add(active_linked_connection_lst, conn);
    if (!called_loop_once) {
      /* This is the first event on the list; we won't be in LOOP_ONCE mode,
       * so we need to make sure that the event_base_loop() actually exits at
       * the end of its run through the current connections and lets us
       * activate read events for linked connections. */
      struct timeval tv = { 0, 0 };
      tor_event_base_loopexit(tor_libevent_get_base(), &tv);
    }
  } else {
    tor_assert(smartlist_isin(active_linked_connection_lst, conn));
  }
}

/** Tell the main loop to stop reading bytes into <b>conn</b> from its linked
 * connection, if is currently doing so.  Called by connection_stop_reading,
 * connection_stop_writing, and connection_read. */
void
connection_stop_reading_from_linked_conn(connection_t *conn)
{
  tor_assert(conn);
  tor_assert(conn->linked == 1);

  if (conn->active_on_link) {
    conn->active_on_link = 0;
    /* FFFF We could keep an index here so we can smartlist_del
     * cleanly.  On the other hand, this doesn't show up on profiles,
     * so let's leave it alone for now. */
    smartlist_remove(active_linked_connection_lst, conn);
  } else {
    tor_assert(!smartlist_isin(active_linked_connection_lst, conn));
  }
}

/** Close all connections that have been scheduled to get closed. */
static void
close_closeable_connections(void)
{
  int i;
  for (i = 0; i < smartlist_len(closeable_connection_lst); ) {
    connection_t *conn = smartlist_get(closeable_connection_lst, i);
    if (conn->conn_array_index < 0) {
      connection_unlink(conn); /* blow it away right now */
    } else {
      if (!conn_close_if_marked(conn->conn_array_index))
        ++i;
    }
  }
}

/** Libevent callback: this gets invoked when (connection_t*)<b>conn</b> has
 * some data to read. */
static void
conn_read_callback(evutil_socket_t fd, short event, void *_conn)
{
  connection_t *conn = _conn;
  (void)fd;
  (void)event;

  log_debug(LD_NET,"socket %d wants to read.",(int)conn->s);

  /* assert_connection_ok(conn, time(NULL)); */

  if (connection_handle_read(conn) < 0) {
    if (!conn->marked_for_close) {
#ifndef MS_WINDOWS
      log_warn(LD_BUG,"Unhandled error on read for %s connection "
               "(fd %d); removing",
               conn_type_to_string(conn->type), (int)conn->s);
      tor_fragile_assert();
#endif
      if (CONN_IS_EDGE(conn))
        connection_edge_end_errno(TO_EDGE_CONN(conn));
      connection_mark_for_close(conn);
    }
  }
  assert_connection_ok(conn, time(NULL));

  if (smartlist_len(closeable_connection_lst))
    close_closeable_connections();
}

/** Libevent callback: this gets invoked when (connection_t*)<b>conn</b> has
 * some data to write. */
static void
conn_write_callback(evutil_socket_t fd, short events, void *_conn)
{
  connection_t *conn = _conn;
  (void)fd;
  (void)events;

  LOG_FN_CONN(conn, (LOG_DEBUG, LD_NET, "socket %d wants to write.",
                     (int)conn->s));

  /* assert_connection_ok(conn, time(NULL)); */

  if (connection_handle_write(conn, 0) < 0) {
    if (!conn->marked_for_close) {
      /* this connection is broken. remove it. */
      log_fn(LOG_WARN,LD_BUG,
             "unhandled error on write for %s connection (fd %d); removing",
             conn_type_to_string(conn->type), (int)conn->s);
      tor_fragile_assert();
      if (CONN_IS_EDGE(conn)) {
        /* otherwise we cry wolf about duplicate close */
        edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
        if (!edge_conn->end_reason)
          edge_conn->end_reason = END_STREAM_REASON_INTERNAL;
        edge_conn->edge_has_sent_end = 1;
      }
      connection_close_immediate(conn); /* So we don't try to flush. */
      connection_mark_for_close(conn);
    }
  }
  assert_connection_ok(conn, time(NULL));

  if (smartlist_len(closeable_connection_lst))
    close_closeable_connections();
}

/** If the connection at connection_array[i] is marked for close, then:
 *    - If it has data that it wants to flush, try to flush it.
 *    - If it _still_ has data to flush, and conn->hold_open_until_flushed is
 *      true, then leave the connection open and return.
 *    - Otherwise, remove the connection from connection_array and from
 *      all other lists, close it, and free it.
 * Returns 1 if the connection was closed, 0 otherwise.
 */
static int
conn_close_if_marked(int i)
{
  connection_t *conn;
  int retval;
  time_t now;

  conn = smartlist_get(connection_array, i);
  if (!conn->marked_for_close)
    return 0; /* nothing to see here, move along */
  now = time(NULL);
  assert_connection_ok(conn, now);
  /* assert_all_pending_dns_resolves_ok(); */

#ifdef USE_BUFFEREVENTS
  if (conn->bufev) {
    if (conn->hold_open_until_flushed &&
        evbuffer_get_length(bufferevent_get_output(conn->bufev))) {
      /* don't close yet. */
      return 0;
    }
    if (conn->linked_conn && ! conn->linked_conn->marked_for_close) {
      /* We need to do this explicitly so that the linked connection
       * notices that there was an EOF. */
      bufferevent_flush(conn->bufev, EV_WRITE, BEV_FINISHED);
    }
  }
#endif

  log_debug(LD_NET,"Cleaning up connection (fd %d).",conn->s);

  /* If the connection we are about to close was trying to connect to
  a proxy server and failed, the client won't be able to use that
  proxy. We should warn him about this. */
  if (conn->proxy_state == PROXY_INFANT) {
    log_warn(LD_NET,
             "The connection to a configured proxy server just failed. "
             "Make sure that the proxy server is up and running.");  
  }

  IF_HAS_BUFFEREVENT(conn, goto unlink);
  if ((SOCKET_OK(conn->s) || conn->linked_conn) &&
      connection_wants_to_flush(conn)) {
    /* s == -1 means it's an incomplete edge connection, or that the socket
     * has already been closed as unflushable. */
    ssize_t sz = connection_bucket_write_limit(conn, now);
    if (!conn->hold_open_until_flushed)
      log_info(LD_NET,
               "Conn (addr %s, fd %d, type %s, state %d) marked, but wants "
               "to flush %d bytes. (Marked at %s:%d)",
               escaped_safe_str_client(conn->address),
               (int)conn->s, conn_type_to_string(conn->type), conn->state,
               (int)conn->outbuf_flushlen,
                conn->marked_for_close_file, conn->marked_for_close);
    if (conn->linked_conn) {
      retval = move_buf_to_buf(conn->linked_conn->inbuf, conn->outbuf,
                               &conn->outbuf_flushlen);
      if (retval >= 0) {
        /* The linked conn will notice that it has data when it notices that
         * we're gone. */
        connection_start_reading_from_linked_conn(conn->linked_conn);
      }
      log_debug(LD_GENERAL, "Flushed last %d bytes from a linked conn; "
               "%d left; flushlen %d; wants-to-flush==%d", retval,
                (int)connection_get_outbuf_len(conn),
                (int)conn->outbuf_flushlen,
                connection_wants_to_flush(conn));
    } else if (connection_speaks_cells(conn)) {
      if (conn->state == OR_CONN_STATE_OPEN) {
        retval = flush_buf_tls(TO_OR_CONN(conn)->tls, conn->outbuf, sz,
                               &conn->outbuf_flushlen);
      } else
        retval = -1; /* never flush non-open broken tls connections */
    } else {
      retval = flush_buf(conn->s, conn->outbuf, sz, &conn->outbuf_flushlen);
    }
    if (retval >= 0 && /* Technically, we could survive things like
                          TLS_WANT_WRITE here. But don't bother for now. */
        conn->hold_open_until_flushed && connection_wants_to_flush(conn)) {
      if (retval > 0) {
        LOG_FN_CONN(conn, (LOG_INFO,LD_NET,
                           "Holding conn (fd %d) open for more flushing.",
                           (int)conn->s));
        conn->timestamp_lastwritten = now; /* reset so we can flush more */
      }
      return 0;
    }
    if (connection_wants_to_flush(conn)) {
      int severity;
      if (conn->type == CONN_TYPE_EXIT ||
          (conn->type == CONN_TYPE_OR && server_mode(get_options())) ||
          (conn->type == CONN_TYPE_DIR && conn->purpose == DIR_PURPOSE_SERVER))
        severity = LOG_INFO;
      else
        severity = LOG_NOTICE;
      /* XXXX Maybe allow this to happen a certain amount per hour; it usually
       * is meaningless. */
      log_fn(severity, LD_NET, "We stalled too much while trying to write %d "
             "bytes to address %s.  If this happens a lot, either "
             "something is wrong with your network connection, or "
             "something is wrong with theirs. "
             "(fd %d, type %s, state %d, marked at %s:%d).",
             (int)connection_get_outbuf_len(conn),
             escaped_safe_str_client(conn->address),
             (int)conn->s, conn_type_to_string(conn->type), conn->state,
             conn->marked_for_close_file,
             conn->marked_for_close);
    }
  }

#ifdef USE_BUFFEREVENTS
 unlink:
#endif
  connection_unlink(conn); /* unlink, remove, free */
  return 1;
}

/** We've just tried every dirserver we know about, and none of
 * them were reachable. Assume the network is down. Change state
 * so next time an application connection arrives we'll delay it
 * and try another directory fetch. Kill off all the circuit_wait
 * streams that are waiting now, since they will all timeout anyway.
 */
void
directory_all_unreachable(time_t now)
{
  connection_t *conn;
  (void)now;

  stats_n_seconds_working=0; /* reset it */

  while ((conn = connection_get_by_type_state(CONN_TYPE_AP,
                                              AP_CONN_STATE_CIRCUIT_WAIT))) {
    edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
    log_notice(LD_NET,
               "Is your network connection down? "
               "Failing connection to '%s:%d'.",
               safe_str_client(edge_conn->socks_request->address),
               edge_conn->socks_request->port);
    connection_mark_unattached_ap(edge_conn,
                                  END_STREAM_REASON_NET_UNREACHABLE);
  }
  control_event_general_status(LOG_ERR, "DIR_ALL_UNREACHABLE");
}

/** This function is called whenever we successfully pull down some new
 * network statuses or server descriptors. */
void
directory_info_has_arrived(time_t now, int from_cache)
{
  or_options_t *options = get_options();

  if (!router_have_minimum_dir_info()) {
    int quiet = directory_too_idle_to_fetch_descriptors(options, now);
    log(quiet ? LOG_INFO : LOG_NOTICE, LD_DIR,
        "I learned some more directory information, but not enough to "
        "build a circuit: %s", get_dir_info_status_string());
    update_all_descriptor_downloads(now);
    return;
  } else {
    if (directory_fetches_from_authorities(options)) {
      update_all_descriptor_downloads(now);
    }

    /* if we have enough dir info, then update our guard status with
     * whatever we just learned. */
    entry_guards_compute_status(options, now);
    /* Don't even bother trying to get extrainfo until the rest of our
     * directory info is up-to-date */
    if (options->DownloadExtraInfo)
      update_extrainfo_downloads(now);
  }

  if (server_mode(options) && !we_are_hibernating() && !from_cache &&
      (can_complete_circuit || !any_predicted_circuits(now)))
    consider_testing_reachability(1, 1);
}

/** How long do we wait before killing OR connections with no circuits?
 * In Tor versions up to 0.2.1.25 and 0.2.2.12-alpha, we waited 15 minutes
 * before cancelling these connections, which caused fast relays to accrue
 * many many idle connections. Hopefully 3 minutes is low enough that
 * it kills most idle connections, without being so low that we cause
 * clients to bounce on and off.
 */
#define IDLE_OR_CONN_TIMEOUT 180

/** Perform regular maintenance tasks for a single connection.  This
 * function gets run once per second per connection by run_scheduled_events.
 */
static void
run_connection_housekeeping(int i, time_t now)
{
  cell_t cell;
  connection_t *conn = smartlist_get(connection_array, i);
  or_options_t *options = get_options();
  or_connection_t *or_conn;
  int past_keepalive =
    now >= conn->timestamp_lastwritten + options->KeepalivePeriod;

  if (conn->outbuf && !connection_get_outbuf_len(conn) &&
      conn->type == CONN_TYPE_OR)
    TO_OR_CONN(conn)->timestamp_lastempty = now;

  if (conn->marked_for_close) {
    /* nothing to do here */
    return;
  }

  /* Expire any directory connections that haven't been active (sent
   * if a server or received if a client) for 5 min */
  if (conn->type == CONN_TYPE_DIR &&
      ((DIR_CONN_IS_SERVER(conn) &&
        conn->timestamp_lastwritten + DIR_CONN_MAX_STALL < now) ||
       (!DIR_CONN_IS_SERVER(conn) &&
        conn->timestamp_lastread + DIR_CONN_MAX_STALL < now))) {
    log_info(LD_DIR,"Expiring wedged directory conn (fd %d, purpose %d)",
             (int)conn->s, conn->purpose);
    /* This check is temporary; it's to let us know whether we should consider
     * parsing partial serverdesc responses. */
    if (conn->purpose == DIR_PURPOSE_FETCH_SERVERDESC &&
        connection_get_inbuf_len(conn) >= 1024) {
      log_info(LD_DIR,"Trying to extract information from wedged server desc "
               "download.");
      connection_dir_reached_eof(TO_DIR_CONN(conn));
    } else {
      connection_mark_for_close(conn);
    }
    return;
  }

  if (!connection_speaks_cells(conn))
    return; /* we're all done here, the rest is just for OR conns */

  /* If we haven't written to an OR connection for a while, then either nuke
     the connection or send a keepalive, depending. */

  or_conn = TO_OR_CONN(conn);
#ifdef USE_BUFFEREVENTS
  tor_assert(conn->bufev);
#else
  tor_assert(conn->outbuf);
#endif

  if (or_conn->is_bad_for_new_circs && !or_conn->n_circuits) {
    /* It's bad for new circuits, and has no unmarked circuits on it:
     * mark it now. */
    log_info(LD_OR,
             "Expiring non-used OR connection to fd %d (%s:%d) [Too old].",
             (int)conn->s, conn->address, conn->port);
    if (conn->state == OR_CONN_STATE_CONNECTING)
      connection_or_connect_failed(TO_OR_CONN(conn),
                                   END_OR_CONN_REASON_TIMEOUT,
                                   "Tor gave up on the connection");
    connection_mark_and_flush(conn);
  } else if (!connection_state_is_open(conn)) {
    if (past_keepalive) {
      /* We never managed to actually get this connection open and happy. */
      log_info(LD_OR,"Expiring non-open OR connection to fd %d (%s:%d).",
               (int)conn->s,conn->address, conn->port);
      connection_mark_for_close(conn);
    }
  } else if (we_are_hibernating() && !or_conn->n_circuits &&
             !connection_get_outbuf_len(conn)) {
    /* We're hibernating, there's no circuits, and nothing to flush.*/
    log_info(LD_OR,"Expiring non-used OR connection to fd %d (%s:%d) "
             "[Hibernating or exiting].",
             (int)conn->s,conn->address, conn->port);
    connection_mark_and_flush(conn);
  } else if (!or_conn->n_circuits &&
             now >= or_conn->timestamp_last_added_nonpadding +
                                         IDLE_OR_CONN_TIMEOUT) {
    log_info(LD_OR,"Expiring non-used OR connection to fd %d (%s:%d) "
             "[idle %d].", (int)conn->s,conn->address, conn->port,
             (int)(now - or_conn->timestamp_last_added_nonpadding));
    connection_mark_for_close(conn);
  } else if (
      now >= or_conn->timestamp_lastempty + options->KeepalivePeriod*10 &&
      now >= conn->timestamp_lastwritten + options->KeepalivePeriod*10) {
    log_fn(LOG_PROTOCOL_WARN,LD_PROTOCOL,
           "Expiring stuck OR connection to fd %d (%s:%d). (%d bytes to "
           "flush; %d seconds since last write)",
           (int)conn->s, conn->address, conn->port,
           (int)connection_get_outbuf_len(conn),
           (int)(now-conn->timestamp_lastwritten));
    connection_mark_for_close(conn);
  } else if (past_keepalive && !connection_get_outbuf_len(conn)) {
    /* send a padding cell */
    log_fn(LOG_DEBUG,LD_OR,"Sending keepalive to (%s:%d)",
           conn->address, conn->port);
    memset(&cell,0,sizeof(cell_t));
    cell.command = CELL_PADDING;
    connection_or_write_cell_to_buf(&cell, or_conn);
  }
}

/** Honor a NEWNYM request: make future requests unlinkable to past
 * requests. */
static void
signewnym_impl(time_t now)
{
  or_options_t *options = get_options();
  if (!proxy_mode(options)) {
    log_info(LD_CONTROL, "Ignoring SIGNAL NEWNYM because client functionality "
             "is disabled.");
    return;
  }

  circuit_expire_all_dirty_circs();
  addressmap_clear_transient();
  rend_client_purge_state();
  time_of_last_signewnym = now;
  signewnym_is_pending = 0;
}

/** Perform regular maintenance tasks.  This function gets run once per
 * second by second_elapsed_callback().
 */
static void
run_scheduled_events(time_t now)
{
  static time_t last_rotated_x509_certificate = 0;
  static time_t time_to_check_v3_certificate = 0;
  static time_t time_to_check_listeners = 0;
  static time_t time_to_check_descriptor = 0;
  static time_t time_to_check_ipaddress = 0;
  static time_t time_to_shrink_memory = 0;
  static time_t time_to_try_getting_descriptors = 0;
  static time_t time_to_reset_descriptor_failures = 0;
  static time_t time_to_add_entropy = 0;
  static time_t time_to_write_bridge_status_file = 0;
  static time_t time_to_downrate_stability = 0;
  static time_t time_to_save_stability = 0;
  static time_t time_to_clean_caches = 0;
  static time_t time_to_recheck_bandwidth = 0;
  static time_t time_to_check_for_expired_networkstatus = 0;
  static time_t time_to_write_stats_files = 0;
  static time_t time_to_write_bridge_stats = 0;
  static time_t time_to_check_port_forwarding = 0;
  static time_t time_to_launch_reachability_tests = 0;
  static int should_init_bridge_stats = 1;
  static time_t time_to_retry_dns_init = 0;
  static time_t time_to_next_heartbeat = 0;
  or_options_t *options = get_options();
  int is_server = server_mode(options);
  int i;
  int have_dir_info;

  /** 0. See if we've been asked to shut down and our timeout has
   * expired; or if our bandwidth limits are exhausted and we
   * should hibernate; or if it's time to wake up from hibernation.
   */
  consider_hibernation(now);

#if 0
  {
    static time_t nl_check_time = 0;
    if (nl_check_time <= now) {
      nodelist_assert_ok();
      nl_check_time = now + 30;
    }
  }
#endif

  /* 0b. If we've deferred a signewnym, make sure it gets handled
   * eventually. */
  if (signewnym_is_pending &&
      time_of_last_signewnym + MAX_SIGNEWNYM_RATE <= now) {
    log(LOG_INFO, LD_CONTROL, "Honoring delayed NEWNYM request");
    signewnym_impl(now);
  }

  /* 0c. If we've deferred log messages for the controller, handle them now */
  flush_pending_log_callbacks();

  /** 1a. Every MIN_ONION_KEY_LIFETIME seconds, rotate the onion keys,
   *  shut down and restart all cpuworkers, and update the directory if
   *  necessary.
   */
  if (is_server &&
      get_onion_key_set_at()+MIN_ONION_KEY_LIFETIME < now) {
    log_info(LD_GENERAL,"Rotating onion key.");
    rotate_onion_key();
    cpuworkers_rotate();
    if (router_rebuild_descriptor(1)<0) {
      log_info(LD_CONFIG, "Couldn't rebuild router descriptor");
    }
    if (advertised_server_mode())
      router_upload_dir_desc_to_dirservers(0);
  }

  if (time_to_try_getting_descriptors < now) {
    update_all_descriptor_downloads(now);
    update_extrainfo_downloads(now);
    if (router_have_minimum_dir_info())
      time_to_try_getting_descriptors = now + LAZY_DESCRIPTOR_RETRY_INTERVAL;
    else
      time_to_try_getting_descriptors = now + GREEDY_DESCRIPTOR_RETRY_INTERVAL;
  }

  if (time_to_reset_descriptor_failures < now) {
    router_reset_descriptor_download_failures();
    time_to_reset_descriptor_failures =
      now + DESCRIPTOR_FAILURE_RESET_INTERVAL;
  }

  if (options->UseBridges)
    fetch_bridge_descriptors(options, now);

  /** 1b. Every MAX_SSL_KEY_LIFETIME seconds, we change our TLS context. */
  if (!last_rotated_x509_certificate)
    last_rotated_x509_certificate = now;
  if (last_rotated_x509_certificate+MAX_SSL_KEY_LIFETIME < now) {
    log_info(LD_GENERAL,"Rotating tls context.");
    if (tor_tls_context_init(public_server_mode(options),
                             get_tlsclient_identity_key(),
                             is_server ? get_server_identity_key() : NULL,
                             MAX_SSL_KEY_LIFETIME) < 0) {
      log_warn(LD_BUG, "Error reinitializing TLS context");
      /* XXX is it a bug here, that we just keep going? -RD */
    }
    last_rotated_x509_certificate = now;
    /* We also make sure to rotate the TLS connections themselves if they've
     * been up for too long -- but that's done via is_bad_for_new_circs in
     * connection_run_housekeeping() above. */
  }

  if (time_to_add_entropy < now) {
    if (time_to_add_entropy) {
      /* We already seeded once, so don't die on failure. */
      crypto_seed_rng(0);
    }
/** How often do we add more entropy to OpenSSL's RNG pool? */
#define ENTROPY_INTERVAL (60*60)
    time_to_add_entropy = now + ENTROPY_INTERVAL;
  }

  /** 1c. If we have to change the accounting interval or record
   * bandwidth used in this accounting interval, do so. */
  if (accounting_is_enabled(options))
    accounting_run_housekeeping(now);

  if (time_to_launch_reachability_tests < now &&
      (authdir_mode_tests_reachability(options)) &&
       !we_are_hibernating()) {
    time_to_launch_reachability_tests = now + REACHABILITY_TEST_INTERVAL;
    /* try to determine reachability of the other Tor relays */
    dirserv_test_reachability(now);
  }

  /** 1d. Periodically, we discount older stability information so that new
   * stability info counts more, and save the stability information to disk as
   * appropriate. */
  if (time_to_downrate_stability < now)
    time_to_downrate_stability = rep_hist_downrate_old_runs(now);
  if (authdir_mode_tests_reachability(options)) {
    if (time_to_save_stability < now) {
      if (time_to_save_stability && rep_hist_record_mtbf_data(now, 1)<0) {
        log_warn(LD_GENERAL, "Couldn't store mtbf data.");
      }
#define SAVE_STABILITY_INTERVAL (30*60)
      time_to_save_stability = now + SAVE_STABILITY_INTERVAL;
    }
  }

  /* 1e. Periodically, if we're a v3 authority, we check whether our cert is
   * close to expiring and warn the admin if it is. */
  if (time_to_check_v3_certificate < now) {
    v3_authority_check_key_expiry();
#define CHECK_V3_CERTIFICATE_INTERVAL (5*60)
    time_to_check_v3_certificate = now + CHECK_V3_CERTIFICATE_INTERVAL;
  }

  /* 1f. Check whether our networkstatus has expired.
   */
  if (time_to_check_for_expired_networkstatus < now) {
    networkstatus_t *ns = networkstatus_get_latest_consensus();
    /*XXXX RD: This value needs to be the same as REASONABLY_LIVE_TIME in
     * networkstatus_get_reasonably_live_consensus(), but that value is way
     * way too high.  Arma: is the bridge issue there resolved yet? -NM */
#define NS_EXPIRY_SLOP (24*60*60)
    if (ns && ns->valid_until < now+NS_EXPIRY_SLOP &&
        router_have_minimum_dir_info()) {
      router_dir_info_changed();
    }
#define CHECK_EXPIRED_NS_INTERVAL (2*60)
    time_to_check_for_expired_networkstatus = now + CHECK_EXPIRED_NS_INTERVAL;
  }

  /* 1g. Check whether we should write statistics to disk.
   */
  if (time_to_write_stats_files < now) {
#define CHECK_WRITE_STATS_INTERVAL (60*60)
    time_t next_time_to_write_stats_files = (time_to_write_stats_files > 0 ?
           time_to_write_stats_files : now) + CHECK_WRITE_STATS_INTERVAL;
    if (options->CellStatistics) {
      time_t next_write =
          rep_hist_buffer_stats_write(time_to_write_stats_files);
      if (next_write && next_write < next_time_to_write_stats_files)
        next_time_to_write_stats_files = next_write;
    }
    if (options->DirReqStatistics) {
      time_t next_write = geoip_dirreq_stats_write(time_to_write_stats_files);
      if (next_write && next_write < next_time_to_write_stats_files)
        next_time_to_write_stats_files = next_write;
    }
    if (options->EntryStatistics) {
      time_t next_write = geoip_entry_stats_write(time_to_write_stats_files);
      if (next_write && next_write < next_time_to_write_stats_files)
        next_time_to_write_stats_files = next_write;
    }
    if (options->ExitPortStatistics) {
      time_t next_write = rep_hist_exit_stats_write(time_to_write_stats_files);
      if (next_write && next_write < next_time_to_write_stats_files)
        next_time_to_write_stats_files = next_write;
    }
    if (options->ConnDirectionStatistics) {
      time_t next_write = rep_hist_conn_stats_write(time_to_write_stats_files);
      if (next_write && next_write < next_time_to_write_stats_files)
        next_time_to_write_stats_files = next_write;
    }
    time_to_write_stats_files = next_time_to_write_stats_files;
  }

  /* 1h. Check whether we should write bridge statistics to disk.
   */
  if (should_record_bridge_info(options)) {
    if (time_to_write_bridge_stats < now) {
      if (should_init_bridge_stats) {
        /* (Re-)initialize bridge statistics. */
        geoip_bridge_stats_init(now);
        time_to_write_bridge_stats = now + WRITE_STATS_INTERVAL;
        should_init_bridge_stats = 0;
      } else {
        /* Possibly write bridge statistics to disk and ask when to write
         * them next time. */
        time_to_write_bridge_stats = geoip_bridge_stats_write(
                                           time_to_write_bridge_stats);
      }
    }
  } else if (!should_init_bridge_stats) {
    /* Bridge mode was turned off. Ensure that stats are re-initialized
     * next time bridge mode is turned on. */
    should_init_bridge_stats = 1;
  }

  /* Remove old information from rephist and the rend cache. */
  if (time_to_clean_caches < now) {
    rep_history_clean(now - options->RephistTrackTime);
    rend_cache_clean(now);
    rend_cache_clean_v2_descs_as_dir(now);
    microdesc_cache_rebuild(NULL, 0);
#define CLEAN_CACHES_INTERVAL (30*60)
    time_to_clean_caches = now + CLEAN_CACHES_INTERVAL;
  }

#define RETRY_DNS_INTERVAL (10*60)
  /* If we're a server and initializing dns failed, retry periodically. */
  if (time_to_retry_dns_init < now) {
    time_to_retry_dns_init = now + RETRY_DNS_INTERVAL;
    if (is_server && has_dns_init_failed())
      dns_init();
  }

  /** 2. Periodically, we consider force-uploading our descriptor
   * (if we've passed our internal checks). */

/** How often do we check whether part of our router info has changed in a way
 * that would require an upload? */
#define CHECK_DESCRIPTOR_INTERVAL (60)
/** How often do we (as a router) check whether our IP address has changed? */
#define CHECK_IPADDRESS_INTERVAL (15*60)

  /* 2b. Once per minute, regenerate and upload the descriptor if the old
   * one is inaccurate. */
  if (time_to_check_descriptor < now) {
    static int dirport_reachability_count = 0;
    time_to_check_descriptor = now + CHECK_DESCRIPTOR_INTERVAL;
    check_descriptor_bandwidth_changed(now);
    if (time_to_check_ipaddress < now) {
      time_to_check_ipaddress = now + CHECK_IPADDRESS_INTERVAL;
      check_descriptor_ipaddress_changed(now);
    }
/** If our router descriptor ever goes this long without being regenerated
 * because something changed, we force an immediate regenerate-and-upload. */
#define FORCE_REGENERATE_DESCRIPTOR_INTERVAL (18*60*60)
    mark_my_descriptor_dirty_if_older_than(
                                  now - FORCE_REGENERATE_DESCRIPTOR_INTERVAL);
    consider_publishable_server(0);
    /* also, check religiously for reachability, if it's within the first
     * 20 minutes of our uptime. */
    if (is_server &&
        (can_complete_circuit || !any_predicted_circuits(now)) &&
        !we_are_hibernating()) {
      if (stats_n_seconds_working < TIMEOUT_UNTIL_UNREACHABILITY_COMPLAINT) {
        consider_testing_reachability(1, dirport_reachability_count==0);
        if (++dirport_reachability_count > 5)
          dirport_reachability_count = 0;
      } else if (time_to_recheck_bandwidth < now) {
        /* If we haven't checked for 12 hours and our bandwidth estimate is
         * low, do another bandwidth test. This is especially important for
         * bridges, since they might go long periods without much use. */
        const routerinfo_t *me = router_get_my_routerinfo();
        if (time_to_recheck_bandwidth && me &&
            me->bandwidthcapacity < me->bandwidthrate &&
            me->bandwidthcapacity < 51200) {
          reset_bandwidth_test();
        }
#define BANDWIDTH_RECHECK_INTERVAL (12*60*60)
        time_to_recheck_bandwidth = now + BANDWIDTH_RECHECK_INTERVAL;
      }
    }

    /* If any networkstatus documents are no longer recent, we need to
     * update all the descriptors' running status. */
    /* purge obsolete entries */
    networkstatus_v2_list_clean(now);
    /* Remove dead routers. */
    routerlist_remove_old_routers();

    /* Also, once per minute, check whether we want to download any
     * networkstatus documents.
     */
    update_networkstatus_downloads(now);
  }

  /** 2c. Let directory voting happen. */
  if (authdir_mode_v3(options))
    dirvote_act(options, now);

  /** 3a. Every second, we examine pending circuits and prune the
   *    ones which have been pending for more than a few seconds.
   *    We do this before step 4, so it can try building more if
   *    it's not comfortable with the number of available circuits.
   */
  /* XXXX022 If our circuit build timeout is much lower than a second, maybe
   * we should do this more often? -NM
   * It can't be lower than 1.5 seconds currently; see
   * circuit_build_times_min_timeout(). -RD
   */
  circuit_expire_building();

  /** 3b. Also look at pending streams and prune the ones that 'began'
   *     a long time ago but haven't gotten a 'connected' yet.
   *     Do this before step 4, so we can put them back into pending
   *     state to be picked up by the new circuit.
   */
  connection_ap_expire_beginning();

  /** 3c. And expire connections that we've held open for too long.
   */
  connection_expire_held_open();

  /** 3d. And every 60 seconds, we relaunch listeners if any died. */
  if (!we_are_hibernating() && time_to_check_listeners < now) {
    retry_all_listeners(NULL, NULL);
    time_to_check_listeners = now+60;
  }

  /** 4. Every second, we try a new circuit if there are no valid
   *    circuits. Every NewCircuitPeriod seconds, we expire circuits
   *    that became dirty more than MaxCircuitDirtiness seconds ago,
   *    and we make a new circ if there are no clean circuits.
   */
  have_dir_info = router_have_minimum_dir_info();
  if (have_dir_info && !we_are_hibernating())
    circuit_build_needed_circs(now);

  /* every 10 seconds, but not at the same second as other such events */
  if (now % 10 == 5)
    circuit_expire_old_circuits_serverside(now);

  /** 5. We do housekeeping for each connection... */
  connection_or_set_bad_connections(NULL, 0);
  for (i=0;i<smartlist_len(connection_array);i++) {
    run_connection_housekeeping(i, now);
  }
  if (time_to_shrink_memory < now) {
    SMARTLIST_FOREACH(connection_array, connection_t *, conn, {
        if (conn->outbuf)
          buf_shrink(conn->outbuf);
        if (conn->inbuf)
          buf_shrink(conn->inbuf);
      });
    clean_cell_pool();
    buf_shrink_freelists(0);
/** How often do we check buffers and pools for empty space that can be
 * deallocated? */
#define MEM_SHRINK_INTERVAL (60)
    time_to_shrink_memory = now + MEM_SHRINK_INTERVAL;
  }

  /** 6. And remove any marked circuits... */
  circuit_close_all_marked();

  /** 7. And upload service descriptors if necessary. */
  if (can_complete_circuit && !we_are_hibernating()) {
    rend_consider_services_upload(now);
    rend_consider_descriptor_republication();
  }

  /** 8. and blow away any connections that need to die. have to do this now,
   * because if we marked a conn for close and left its socket -1, then
   * we'll pass it to poll/select and bad things will happen.
   */
  close_closeable_connections();

  /** 8b. And if anything in our state is ready to get flushed to disk, we
   * flush it. */
  or_state_save(now);

  /** 9. and if we're a server, check whether our DNS is telling stories to
   * us. */
  if (is_server && time_to_check_for_correct_dns < now) {
    if (!time_to_check_for_correct_dns) {
      time_to_check_for_correct_dns = now + 60 + crypto_rand_int(120);
    } else {
      dns_launch_correctness_checks();
      time_to_check_for_correct_dns = now + 12*3600 +
        crypto_rand_int(12*3600);
    }
  }

  /** 10b. write bridge networkstatus file to disk */
  if (options->BridgeAuthoritativeDir &&
      time_to_write_bridge_status_file < now) {
    networkstatus_dump_bridge_status_to_file(now);
#define BRIDGE_STATUSFILE_INTERVAL (30*60)
    time_to_write_bridge_status_file = now+BRIDGE_STATUSFILE_INTERVAL;
  }

  if (time_to_check_port_forwarding < now &&
      options->PortForwarding &&
      is_server) {
#define PORT_FORWARDING_CHECK_INTERVAL 5
    tor_check_port_forwarding(options->PortForwardingHelper,
                              options->DirPort,
                              options->ORPort,
                              now);
    time_to_check_port_forwarding = now+PORT_FORWARDING_CHECK_INTERVAL;
  }

  /** 11. write the heartbeat message */
  if (options->HeartbeatPeriod &&
      time_to_next_heartbeat < now) {
    log_heartbeat(now);
    time_to_next_heartbeat = now+options->HeartbeatPeriod;
  }
}

/** Timer: used to invoke second_elapsed_callback() once per second. */
static periodic_timer_t *second_timer = NULL;
/** Number of libevent errors in the last second: we die if we get too many. */
static int n_libevent_errors = 0;

/** Libevent callback: invoked once every second. */
static void
second_elapsed_callback(periodic_timer_t *timer, void *arg)
{
  /* XXXX This could be sensibly refactored into multiple callbacks, and we
   * could use Libevent's timers for this rather than checking the current
   * time against a bunch of timeouts every second. */
  static time_t current_second = 0;
  time_t now;
  size_t bytes_written;
  size_t bytes_read;
  int seconds_elapsed;
#ifdef USE_BUFFEREVENTS
  uint64_t cur_read,cur_written;
#endif
  or_options_t *options = get_options();
  (void)timer;
  (void)arg;

  n_libevent_errors = 0;

  /* log_notice(LD_GENERAL, "Tick."); */
  now = time(NULL);
  update_approx_time(now);

  /* the second has rolled over. check more stuff. */
  seconds_elapsed = current_second ? (int)(now - current_second) : 0;
#ifdef USE_BUFFEREVENTS
  connection_get_rate_limit_totals(&cur_read, &cur_written);
  bytes_written = (size_t)(cur_written - stats_prev_n_written);
  bytes_read = (size_t)(cur_read - stats_prev_n_read);
#else
  bytes_written = stats_prev_global_write_bucket - global_write_bucket;
  bytes_read = stats_prev_global_read_bucket - global_read_bucket;
#endif
  stats_n_bytes_read += bytes_read;
  stats_n_bytes_written += bytes_written;
  if (accounting_is_enabled(options) && seconds_elapsed >= 0)
    accounting_add_bytes(bytes_read, bytes_written, seconds_elapsed);
  control_event_bandwidth_used((uint32_t)bytes_read,(uint32_t)bytes_written);
  control_event_stream_bandwidth_used();

  if (seconds_elapsed > 0)
    connection_bucket_refill(seconds_elapsed, now);
#ifdef USE_BUFFEREVENTS
  stats_prev_n_written = cur_written;
  stats_prev_n_read = cur_read;
#else
  stats_prev_global_read_bucket = global_read_bucket;
  stats_prev_global_write_bucket = global_write_bucket;
#endif

  if (server_mode(options) &&
      !we_are_hibernating() &&
      seconds_elapsed > 0 &&
      can_complete_circuit &&
      stats_n_seconds_working / TIMEOUT_UNTIL_UNREACHABILITY_COMPLAINT !=
      (stats_n_seconds_working+seconds_elapsed) /
        TIMEOUT_UNTIL_UNREACHABILITY_COMPLAINT) {
    /* every 20 minutes, check and complain if necessary */
    const routerinfo_t *me = router_get_my_routerinfo();
    if (me && !check_whether_orport_reachable()) {
      log_warn(LD_CONFIG,"Your server (%s:%d) has not managed to confirm that "
               "its ORPort is reachable. Please check your firewalls, ports, "
               "address, /etc/hosts file, etc.",
               me->address, me->or_port);
      control_event_server_status(LOG_WARN,
                                  "REACHABILITY_FAILED ORADDRESS=%s:%d",
                                  me->address, me->or_port);
    }

    if (me && !check_whether_dirport_reachable()) {
      log_warn(LD_CONFIG,
               "Your server (%s:%d) has not managed to confirm that its "
               "DirPort is reachable. Please check your firewalls, ports, "
               "address, /etc/hosts file, etc.",
               me->address, me->dir_port);
      control_event_server_status(LOG_WARN,
                                  "REACHABILITY_FAILED DIRADDRESS=%s:%d",
                                  me->address, me->dir_port);
    }
  }

/** If more than this many seconds have elapsed, probably the clock
 * jumped: doesn't count. */
#define NUM_JUMPED_SECONDS_BEFORE_WARN 100
  if (seconds_elapsed < -NUM_JUMPED_SECONDS_BEFORE_WARN ||
      seconds_elapsed >= NUM_JUMPED_SECONDS_BEFORE_WARN) {
    circuit_note_clock_jumped(seconds_elapsed);
    /* XXX if the time jumps *back* many months, do our events in
     * run_scheduled_events() recover? I don't think they do. -RD */
  } else if (seconds_elapsed > 0)
    stats_n_seconds_working += seconds_elapsed;

  run_scheduled_events(now);

  current_second = now; /* remember which second it is, for next time */
}

#ifndef MS_WINDOWS
/** Called when a possibly ignorable libevent error occurs; ensures that we
 * don't get into an infinite loop by ignoring too many errors from
 * libevent. */
static int
got_libevent_error(void)
{
  if (++n_libevent_errors > 8) {
    log_err(LD_NET, "Too many libevent errors in one second; dying");
    return -1;
  }
  return 0;
}
#endif

#define UPTIME_CUTOFF_FOR_NEW_BANDWIDTH_TEST (6*60*60)

/** Called when our IP address seems to have changed. <b>at_interface</b>
 * should be true if we detected a change in our interface, and false if we
 * detected a change in our published address. */
void
ip_address_changed(int at_interface)
{
  int server = server_mode(get_options());

  if (at_interface) {
    if (! server) {
      /* Okay, change our keys. */
      init_keys();
    }
  } else {
    if (server) {
      if (stats_n_seconds_working > UPTIME_CUTOFF_FOR_NEW_BANDWIDTH_TEST)
        reset_bandwidth_test();
      stats_n_seconds_working = 0;
      router_reset_reachability();
      mark_my_descriptor_dirty("IP address changed");
    }
  }

  dns_servers_relaunch_checks();
}

/** Forget what we've learned about the correctness of our DNS servers, and
 * start learning again. */
void
dns_servers_relaunch_checks(void)
{
  if (server_mode(get_options())) {
    dns_reset_correctness_checks();
    time_to_check_for_correct_dns = 0;
  }
}

/** Called when we get a SIGHUP: reload configuration files and keys,
 * retry all connections, and so on. */
static int
do_hup(void)
{
  or_options_t *options = get_options();

#ifdef USE_DMALLOC
  dmalloc_log_stats();
  dmalloc_log_changed(0, 1, 0, 0);
#endif

  log_notice(LD_GENERAL,"Received reload signal (hup). Reloading config and "
             "resetting internal state.");
  if (accounting_is_enabled(options))
    accounting_record_bandwidth_usage(time(NULL), get_or_state());

  router_reset_warnings();
  routerlist_reset_warnings();
  /* first, reload config variables, in case they've changed */
  if (options->ReloadTorrcOnSIGHUP) {
    /* no need to provide argc/v, they've been cached in init_from_config */
    if (options_init_from_torrc(0, NULL) < 0) {
      log_err(LD_CONFIG,"Reading config failed--see warnings above. "
              "For usage, try -h.");
      return -1;
    }
    options = get_options(); /* they have changed now */
  } else {
    log_notice(LD_GENERAL, "Not reloading config file: the controller told "
               "us not to.");
  }
  if (authdir_mode_handles_descs(options, -1)) {
    /* reload the approved-routers file */
    if (dirserv_load_fingerprint_file() < 0) {
      /* warnings are logged from dirserv_load_fingerprint_file() directly */
      log_info(LD_GENERAL, "Error reloading fingerprints. "
               "Continuing with old list.");
    }
  }

  /* Rotate away from the old dirty circuits. This has to be done
   * after we've read the new options, but before we start using
   * circuits for directory fetches. */
  circuit_expire_all_dirty_circs();

  /* retry appropriate downloads */
  router_reset_status_download_failures();
  router_reset_descriptor_download_failures();
  update_networkstatus_downloads(time(NULL));

  /* We'll retry routerstatus downloads in about 10 seconds; no need to
   * force a retry there. */

  if (server_mode(options)) {
    /* Restart cpuworker and dnsworker processes, so they get up-to-date
     * configuration options. */
    cpuworkers_rotate();
    dns_reset();
  }
  return 0;
}

/** Tor main loop. */
/* static */ int
do_main_loop(void)
{
  int loop_result;
  time_t now;

  /* initialize dns resolve map, spawn workers if needed */
  if (dns_init() < 0) {
    if (get_options()->ServerDNSAllowBrokenConfig)
      log_warn(LD_GENERAL, "Couldn't set up any working nameservers. "
               "Network not up yet?  Will try again soon.");
    else {
      log_err(LD_GENERAL,"Error initializing dns subsystem; exiting.  To "
              "retry instead, set the ServerDNSAllowBrokenResolvConf option.");
    }
  }

  handle_signals(1);

  /* load the private keys, if we're supposed to have them, and set up the
   * TLS context. */
  if (! client_identity_key_is_set()) {
    if (init_keys() < 0) {
      log_err(LD_BUG,"Error initializing keys; exiting");
      return -1;
    }
  }

  /* Set up the packed_cell_t memory pool. */
  init_cell_pool();

  /* Set up our buckets */
  connection_bucket_init();
#ifndef USE_BUFFEREVENTS
  stats_prev_global_read_bucket = global_read_bucket;
  stats_prev_global_write_bucket = global_write_bucket;
#endif

  /* initialize the bootstrap status events to know we're starting up */
  control_event_bootstrap(BOOTSTRAP_STATUS_STARTING, 0);

  if (trusted_dirs_reload_certs()) {
    log_warn(LD_DIR,
             "Couldn't load all cached v3 certificates. Starting anyway.");
  }
  if (router_reload_v2_networkstatus()) {
    return -1;
  }
  if (router_reload_consensus_networkstatus()) {
    return -1;
  }
  /* load the routers file, or assign the defaults. */
  if (router_reload_router_list()) {
    return -1;
  }
  /* load the networkstatuses. (This launches a download for new routers as
   * appropriate.)
   */
  now = time(NULL);
  directory_info_has_arrived(now, 1);

  if (server_mode(get_options())) {
    /* launch cpuworkers. Need to do this *after* we've read the onion key. */
    cpu_init();
  }

  /* set up once-a-second callback. */
  if (! second_timer) {
    struct timeval one_second;
    one_second.tv_sec = 1;
    one_second.tv_usec = 0;

    second_timer = periodic_timer_new(tor_libevent_get_base(),
                                      &one_second,
                                      second_elapsed_callback,
                                      NULL);
    tor_assert(second_timer);
  }

  for (;;) {
    if (nt_service_is_stopping())
      return 0;

#ifndef MS_WINDOWS
    /* Make it easier to tell whether libevent failure is our fault or not. */
    errno = 0;
#endif
    /* All active linked conns should get their read events activated. */
    SMARTLIST_FOREACH(active_linked_connection_lst, connection_t *, conn,
                      event_active(conn->read_event, EV_READ, 1));
    called_loop_once = smartlist_len(active_linked_connection_lst) ? 1 : 0;

    update_approx_time(time(NULL));

    /* poll until we have an event, or the second ends, or until we have
     * some active linked connections to trigger events for. */
    loop_result = event_base_loop(tor_libevent_get_base(),
                                  called_loop_once ? EVLOOP_ONCE : 0);

    /* let catch() handle things like ^c, and otherwise don't worry about it */
    if (loop_result < 0) {
      int e = tor_socket_errno(-1);
      /* let the program survive things like ^z */
      if (e != EINTR && !ERRNO_IS_EINPROGRESS(e)) {
        log_err(LD_NET,"libevent call with %s failed: %s [%d]",
                tor_libevent_get_method(), tor_socket_strerror(e), e);
        return -1;
#ifndef MS_WINDOWS
      } else if (e == EINVAL) {
        log_warn(LD_NET, "EINVAL from libevent: should you upgrade libevent?");
        if (got_libevent_error())
          return -1;
#endif
      } else {
        if (ERRNO_IS_EINPROGRESS(e))
          log_warn(LD_BUG,
                   "libevent call returned EINPROGRESS? Please report.");
        log_debug(LD_NET,"libevent call interrupted.");
        /* You can't trust the results of this poll(). Go back to the
         * top of the big for loop. */
        continue;
      }
    }
  }
}

#ifndef MS_WINDOWS /* Only called when we're willing to use signals */
/** Libevent callback: invoked when we get a signal.
 */
static void
signal_callback(int fd, short events, void *arg)
{
  uintptr_t sig = (uintptr_t)arg;
  (void)fd;
  (void)events;

  process_signal(sig);
}
#endif

/** Do the work of acting on a signal received in <b>sig</b> */
void
process_signal(uintptr_t sig)
{
  switch (sig)
    {
    case SIGTERM:
      log_notice(LD_GENERAL,"Catching signal TERM, exiting cleanly.");
      tor_cleanup();
      exit(0);
      break;
    case SIGINT:
      if (!server_mode(get_options())) { /* do it now */
        log_notice(LD_GENERAL,"Interrupt: exiting cleanly.");
        tor_cleanup();
        exit(0);
      }
      hibernate_begin_shutdown();
      break;
#ifdef SIGPIPE
    case SIGPIPE:
      log_debug(LD_GENERAL,"Caught SIGPIPE. Ignoring.");
      break;
#endif
    case SIGUSR1:
      /* prefer to log it at INFO, but make sure we always see it */
      dumpstats(get_min_log_level()<LOG_INFO ? get_min_log_level() : LOG_INFO);
      control_event_signal(sig);
      break;
    case SIGUSR2:
      switch_logs_debug();
      log_debug(LD_GENERAL,"Caught USR2, going to loglevel debug. "
                "Send HUP to change back.");
      control_event_signal(sig);
      break;
    case SIGHUP:
      if (do_hup() < 0) {
        log_warn(LD_CONFIG,"Restart failed (config error?). Exiting.");
        tor_cleanup();
        exit(1);
      }
      control_event_signal(sig);
      break;
#ifdef SIGCHLD
    case SIGCHLD:
      while (waitpid(-1,NULL,WNOHANG) > 0) ; /* keep reaping until no more
                                                zombies */
      break;
#endif
    case SIGNEWNYM: {
      time_t now = time(NULL);
      if (time_of_last_signewnym + MAX_SIGNEWNYM_RATE > now) {
        signewnym_is_pending = 1;
        log(LOG_NOTICE, LD_CONTROL,
            "Rate limiting NEWNYM request: delaying by %d second(s)",
            (int)(MAX_SIGNEWNYM_RATE+time_of_last_signewnym-now));
      } else {
        signewnym_impl(now);
        control_event_signal(sig);
      }
      break;
    }
    case SIGCLEARDNSCACHE:
      addressmap_clear_transient();
      control_event_signal(sig);
      break;
  }
}

/** Returns Tor's uptime. */
long
get_uptime(void)
{
  return stats_n_seconds_working;
}

extern uint64_t rephist_total_alloc;
extern uint32_t rephist_total_num;

/**
 * Write current memory usage information to the log.
 */
static void
dumpmemusage(int severity)
{
  connection_dump_buffer_mem_stats(severity);
  log(severity, LD_GENERAL, "In rephist: "U64_FORMAT" used by %d Tors.",
      U64_PRINTF_ARG(rephist_total_alloc), rephist_total_num);
  dump_routerlist_mem_usage(severity);
  dump_cell_pool_usage(severity);
  dump_dns_mem_usage(severity);
  buf_dump_freelist_sizes(severity);
  tor_log_mallinfo(severity);
}

/** Write all statistics to the log, with log level <b>severity</b>. Called
 * in response to a SIGUSR1. */
static void
dumpstats(int severity)
{
  time_t now = time(NULL);
  time_t elapsed;
  size_t rbuf_cap, wbuf_cap, rbuf_len, wbuf_len;

  log(severity, LD_GENERAL, "Dumping stats:");

  SMARTLIST_FOREACH(connection_array, connection_t *, conn,
  {
    int i = conn_sl_idx;
    log(severity, LD_GENERAL,
        "Conn %d (socket %d) type %d (%s), state %d (%s), created %d secs ago",
        i, (int)conn->s, conn->type, conn_type_to_string(conn->type),
        conn->state, conn_state_to_string(conn->type, conn->state),
        (int)(now - conn->timestamp_created));
    if (!connection_is_listener(conn)) {
      log(severity,LD_GENERAL,
          "Conn %d is to %s:%d.", i,
          safe_str_client(conn->address),
          conn->port);
      log(severity,LD_GENERAL,
          "Conn %d: %d bytes waiting on inbuf (len %d, last read %d secs ago)",
          i,
          (int)connection_get_inbuf_len(conn),
          (int)buf_allocation(conn->inbuf),
          (int)(now - conn->timestamp_lastread));
      log(severity,LD_GENERAL,
          "Conn %d: %d bytes waiting on outbuf "
          "(len %d, last written %d secs ago)",i,
          (int)connection_get_outbuf_len(conn),
          (int)buf_allocation(conn->outbuf),
          (int)(now - conn->timestamp_lastwritten));
      if (conn->type == CONN_TYPE_OR) {
        or_connection_t *or_conn = TO_OR_CONN(conn);
        if (or_conn->tls) {
          tor_tls_get_buffer_sizes(or_conn->tls, &rbuf_cap, &rbuf_len,
                                   &wbuf_cap, &wbuf_len);
          log(severity, LD_GENERAL,
              "Conn %d: %d/%d bytes used on OpenSSL read buffer; "
              "%d/%d bytes used on write buffer.",
              i, (int)rbuf_len, (int)rbuf_cap, (int)wbuf_len, (int)wbuf_cap);
        }
      }
    }
    circuit_dump_by_conn(conn, severity); /* dump info about all the circuits
                                           * using this conn */
  });
  log(severity, LD_NET,
      "Cells processed: "U64_FORMAT" padding\n"
      "                 "U64_FORMAT" create\n"
      "                 "U64_FORMAT" created\n"
      "                 "U64_FORMAT" relay\n"
      "                        ("U64_FORMAT" relayed)\n"
      "                        ("U64_FORMAT" delivered)\n"
      "                 "U64_FORMAT" destroy",
      U64_PRINTF_ARG(stats_n_padding_cells_processed),
      U64_PRINTF_ARG(stats_n_create_cells_processed),
      U64_PRINTF_ARG(stats_n_created_cells_processed),
      U64_PRINTF_ARG(stats_n_relay_cells_processed),
      U64_PRINTF_ARG(stats_n_relay_cells_relayed),
      U64_PRINTF_ARG(stats_n_relay_cells_delivered),
      U64_PRINTF_ARG(stats_n_destroy_cells_processed));
  if (stats_n_data_cells_packaged)
    log(severity,LD_NET,"Average packaged cell fullness: %2.3f%%",
        100*(U64_TO_DBL(stats_n_data_bytes_packaged) /
             U64_TO_DBL(stats_n_data_cells_packaged*RELAY_PAYLOAD_SIZE)) );
  if (stats_n_data_cells_received)
    log(severity,LD_NET,"Average delivered cell fullness: %2.3f%%",
        100*(U64_TO_DBL(stats_n_data_bytes_received) /
             U64_TO_DBL(stats_n_data_cells_received*RELAY_PAYLOAD_SIZE)) );

  if (now - time_of_process_start >= 0)
    elapsed = now - time_of_process_start;
  else
    elapsed = 0;

  if (elapsed) {
    log(severity, LD_NET,
        "Average bandwidth: "U64_FORMAT"/%d = %d bytes/sec reading",
        U64_PRINTF_ARG(stats_n_bytes_read),
        (int)elapsed,
        (int) (stats_n_bytes_read/elapsed));
    log(severity, LD_NET,
        "Average bandwidth: "U64_FORMAT"/%d = %d bytes/sec writing",
        U64_PRINTF_ARG(stats_n_bytes_written),
        (int)elapsed,
        (int) (stats_n_bytes_written/elapsed));
  }

  log(severity, LD_NET, "--------------- Dumping memory information:");
  dumpmemusage(severity);

  rep_hist_dump_stats(now,severity);
  rend_service_dump_stats(severity);
  dump_pk_ops(severity);
  dump_distinct_digest_count(severity);
}

/** Called by exit() as we shut down the process.
 */
static void
exit_function(void)
{
  /* NOTE: If we ever daemonize, this gets called immediately.  That's
   * okay for now, because we only use this on Windows.  */
#ifdef MS_WINDOWS
  WSACleanup();
#endif
}

/** Set up the signal handlers for either parent or child. */
void
handle_signals(int is_parent)
{
#ifndef MS_WINDOWS /* do signal stuff only on Unix */
  int i;
  static const int signals[] = {
    SIGINT,  /* do a controlled slow shutdown */
    SIGTERM, /* to terminate now */
    SIGPIPE, /* otherwise SIGPIPE kills us */
    SIGUSR1, /* dump stats */
    SIGUSR2, /* go to loglevel debug */
    SIGHUP,  /* to reload config, retry conns, etc */
#ifdef SIGXFSZ
    SIGXFSZ, /* handle file-too-big resource exhaustion */
#endif
    SIGCHLD, /* handle dns/cpu workers that exit */
    -1 };
  static struct event *signal_events[16]; /* bigger than it has to be. */
  if (is_parent) {
    for (i = 0; signals[i] >= 0; ++i) {
      signal_events[i] = tor_evsignal_new(
                       tor_libevent_get_base(), signals[i], signal_callback,
                       (void*)(uintptr_t)signals[i]);
      if (event_add(signal_events[i], NULL))
        log_warn(LD_BUG, "Error from libevent when adding event for signal %d",
                 signals[i]);
    }
  } else {
    struct sigaction action;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    sigaction(SIGINT,  &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGPIPE, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGUSR2, &action, NULL);
    sigaction(SIGHUP,  &action, NULL);
#ifdef SIGXFSZ
    sigaction(SIGXFSZ, &action, NULL);
#endif
  }
#else /* MS windows */
  (void)is_parent;
#endif /* signal stuff */
}

/** Main entry point for the Tor command-line client.
 */
/* static */ int
tor_init(int argc, char *argv[])
{
  char buf[256];
  int i, quiet = 0;
  time_of_process_start = time(NULL);
  if (!connection_array)
    connection_array = smartlist_create();
  if (!closeable_connection_lst)
    closeable_connection_lst = smartlist_create();
  if (!active_linked_connection_lst)
    active_linked_connection_lst = smartlist_create();
  /* Have the log set up with our application name. */
  tor_snprintf(buf, sizeof(buf), "Tor %s", get_version());
  log_set_application_name(buf);
  /* Initialize the history structures. */
  rep_hist_init();
  /* Initialize the service cache. */
  rend_cache_init();
  addressmap_init(); /* Init the client dns cache. Do it always, since it's
                      * cheap. */

  /* We search for the "quiet" option first, since it decides whether we
   * will log anything at all to the command line. */
  for (i=1;i<argc;++i) {
    if (!strcmp(argv[i], "--hush"))
      quiet = 1;
    if (!strcmp(argv[i], "--quiet"))
      quiet = 2;
  }
 /* give it somewhere to log to initially */
  switch (quiet) {
    case 2:
      /* no initial logging */
      break;
    case 1:
      add_temp_log(LOG_WARN);
      break;
    default:
      add_temp_log(LOG_NOTICE);
  }

  log(LOG_NOTICE, LD_GENERAL, "Tor v%s. This is experimental software. "
      "Do not rely on it for strong anonymity. (Running on %s)",get_version(),
      get_uname());

  if (network_init()<0) {
    log_err(LD_BUG,"Error initializing network; exiting.");
    return -1;
  }
  atexit(exit_function);

  if (options_init_from_torrc(argc,argv) < 0) {
    log_err(LD_CONFIG,"Reading config failed--see warnings above.");
    return -1;
  }

#ifndef MS_WINDOWS
  if (geteuid()==0)
    log_warn(LD_GENERAL,"You are running Tor as root. You don't need to, "
             "and you probably shouldn't.");
#endif

  if (crypto_global_init(get_options()->HardwareAccel,
                         get_options()->AccelName,
                         get_options()->AccelDir)) {
    log_err(LD_BUG, "Unable to initialize OpenSSL. Exiting.");
    return -1;
  }

  return 0;
}

/** A lockfile structure, used to prevent two Tors from messing with the
 * data directory at once.  If this variable is non-NULL, we're holding
 * the lockfile. */
static tor_lockfile_t *lockfile = NULL;

/** Try to grab the lock file described in <b>options</b>, if we do not
 * already have it.  If <b>err_if_locked</b> is true, warn if somebody else is
 * holding the lock, and exit if we can't get it after waiting.  Otherwise,
 * return -1 if we can't get the lockfile.  Return 0 on success.
 */
int
try_locking(or_options_t *options, int err_if_locked)
{
  if (lockfile)
    return 0;
  else {
    char *fname = options_get_datadir_fname2_suffix(options, "lock",NULL,NULL);
    int already_locked = 0;
    tor_lockfile_t *lf = tor_lockfile_lock(fname, 0, &already_locked);
    tor_free(fname);
    if (!lf) {
      if (err_if_locked && already_locked) {
        int r;
        log_warn(LD_GENERAL, "It looks like another Tor process is running "
                 "with the same data directory.  Waiting 5 seconds to see "
                 "if it goes away.");
#ifndef WIN32
        sleep(5);
#else
        Sleep(5000);
#endif
        r = try_locking(options, 0);
        if (r<0) {
          log_err(LD_GENERAL, "No, it's still there.  Exiting.");
          exit(0);
        }
        return r;
      }
      return -1;
    }
    lockfile = lf;
    return 0;
  }
}

/** Return true iff we've successfully acquired the lock file. */
int
have_lockfile(void)
{
  return lockfile != NULL;
}

/** If we have successfully acquired the lock file, release it. */
void
release_lockfile(void)
{
  if (lockfile) {
    tor_lockfile_unlock(lockfile);
    lockfile = NULL;
  }
}

/** Free all memory that we might have allocated somewhere.
 * If <b>postfork</b>, we are a worker process and we want to free
 * only the parts of memory that we won't touch. If !<b>postfork</b>,
 * Tor is shutting down and we should free everything.
 *
 * Helps us find the real leaks with dmalloc and the like. Also valgrind
 * should then report 0 reachable in its leak report (in an ideal world --
 * in practice libevent, SSL, libc etc never quite free everything). */
void
tor_free_all(int postfork)
{
  if (!postfork) {
    evdns_shutdown(1);
  }
  geoip_free_all();
  dirvote_free_all();
  routerlist_free_all();
  networkstatus_free_all();
  addressmap_free_all();
  dirserv_free_all();
  rend_service_free_all();
  rend_cache_free_all();
  rend_service_authorization_free_all();
  rep_hist_free_all();
  dns_free_all();
  clear_pending_onions();
  circuit_free_all();
  entry_guards_free_all();
  connection_free_all();
  buf_shrink_freelists(1);
  memarea_clear_freelist();
  nodelist_free_all();
  microdesc_free_all();
  if (!postfork) {
    config_free_all();
    router_free_all();
    policies_free_all();
  }
  free_cell_pool();
  if (!postfork) {
    tor_tls_free_all();
  }
  /* stuff in main.c */

  smartlist_free(connection_array);
  smartlist_free(closeable_connection_lst);
  smartlist_free(active_linked_connection_lst);
  periodic_timer_free(second_timer);
  if (!postfork) {
    release_lockfile();
  }
  /* Stuff in util.c and address.c*/
  if (!postfork) {
    escaped(NULL);
    esc_router_info(NULL);
    logs_free_all(); /* free log strings. do this last so logs keep working. */
  }
}

/** Do whatever cleanup is necessary before shutting Tor down. */
void
tor_cleanup(void)
{
  or_options_t *options = get_options();
  if (options->command == CMD_RUN_TOR) {
    time_t now = time(NULL);
    /* Remove our pid file. We don't care if there was an error when we
     * unlink, nothing we could do about it anyways. */
    if (options->PidFile)
      unlink(options->PidFile);
    if (options->ControlPortWriteToFile)
      unlink(options->ControlPortWriteToFile);
    if (accounting_is_enabled(options))
      accounting_record_bandwidth_usage(now, get_or_state());
    or_state_mark_dirty(get_or_state(), 0); /* force an immediate save. */
    or_state_save(now);
    if (authdir_mode_tests_reachability(options))
      rep_hist_record_mtbf_data(now, 0);
  }
#ifdef USE_DMALLOC
  dmalloc_log_stats();
#endif
  tor_free_all(0); /* We could move tor_free_all back into the ifdef below
                      later, if it makes shutdown unacceptably slow.  But for
                      now, leave it here: it's helped us catch bugs in the
                      past. */
  crypto_global_cleanup();
#ifdef USE_DMALLOC
  dmalloc_log_unfreed();
  dmalloc_shutdown();
#endif
}

/** Read/create keys as needed, and echo our fingerprint to stdout. */
/* static */ int
do_list_fingerprint(void)
{
  char buf[FINGERPRINT_LEN+1];
  crypto_pk_env_t *k;
  const char *nickname = get_options()->Nickname;
  if (!server_mode(get_options())) {
    log_err(LD_GENERAL,
            "Clients don't have long-term identity keys. Exiting.\n");
    return -1;
  }
  tor_assert(nickname);
  if (init_keys() < 0) {
    log_err(LD_BUG,"Error initializing keys; can't display fingerprint");
    return -1;
  }
  if (!(k = get_server_identity_key())) {
    log_err(LD_GENERAL,"Error: missing identity key.");
    return -1;
  }
  if (crypto_pk_get_fingerprint(k, buf, 1)<0) {
    log_err(LD_BUG, "Error computing fingerprint");
    return -1;
  }
  printf("%s %s\n", nickname, buf);
  return 0;
}

/** Entry point for password hashing: take the desired password from
 * the command line, and print its salted hash to stdout. **/
/* static */ void
do_hash_password(void)
{

  char output[256];
  char key[S2K_SPECIFIER_LEN+DIGEST_LEN];

  crypto_rand(key, S2K_SPECIFIER_LEN-1);
  key[S2K_SPECIFIER_LEN-1] = (uint8_t)96; /* Hash 64 K of data. */
  secret_to_key(key+S2K_SPECIFIER_LEN, DIGEST_LEN,
                get_options()->command_arg, strlen(get_options()->command_arg),
                key);
  base16_encode(output, sizeof(output), key, sizeof(key));
  printf("16:%s\n",output);
}

#if defined (WINCE)
int
find_flashcard_path(PWCHAR path, size_t size)
{
  WIN32_FIND_DATA d = {0};
  HANDLE h = NULL;

  if (!path)
    return -1;

  h = FindFirstFlashCard(&d);
  if (h == INVALID_HANDLE_VALUE)
    return -1;

  if (wcslen(d.cFileName) == 0) {
    FindClose(h);
    return -1;
  }

  wcsncpy(path,d.cFileName,size);
  FindClose(h);
  return 0;
}
#endif

/** Main entry point for the Tor process.  Called from main(). */
/* This function is distinct from main() only so we can link main.c into
 * the unittest binary without conflicting with the unittests' main. */
int
tor_main(int argc, char *argv[])
{
  int result = 0;
#if defined (WINCE)
  WCHAR path [MAX_PATH] = {0};
  WCHAR fullpath [MAX_PATH] = {0};
  PWCHAR p = NULL;
  FILE* redir = NULL;
  FILE* redirdbg = NULL;

  // this is to facilitate debugging by opening
  // a file on a folder shared by the wm emulator.
  // if no flashcard (real or emulated) is present,
  // log files will be written in the root folder
  if (find_flashcard_path(path,MAX_PATH) == -1)
  {
    redir = _wfreopen( L"\\stdout.log", L"w", stdout );
    redirdbg = _wfreopen( L"\\stderr.log", L"w", stderr );
  } else {
    swprintf(fullpath,L"\\%s\\tor",path);
    CreateDirectory(fullpath,NULL);

    swprintf(fullpath,L"\\%s\\tor\\stdout.log",path);
    redir = _wfreopen( fullpath, L"w", stdout );

    swprintf(fullpath,L"\\%s\\tor\\stderr.log",path);
    redirdbg = _wfreopen( fullpath, L"w", stderr );
  }
#endif

#ifdef MS_WINDOWS
  /* Call SetProcessDEPPolicy to permanently enable DEP.
     The function will not resolve on earlier versions of Windows,
     and failure is not dangerous. */
  HMODULE hMod = GetModuleHandleA("Kernel32.dll");
  if (hMod) {
    typedef BOOL (WINAPI *PSETDEP)(DWORD);
    PSETDEP setdeppolicy = (PSETDEP)GetProcAddress(hMod,
                           "SetProcessDEPPolicy");
    if (setdeppolicy) setdeppolicy(1); /* PROCESS_DEP_ENABLE */
  }
#endif

  update_approx_time(time(NULL));
  tor_threads_init();
  init_logging();
#ifdef USE_DMALLOC
  {
    /* Instruct OpenSSL to use our internal wrappers for malloc,
       realloc and free. */
    int r = CRYPTO_set_mem_ex_functions(_tor_malloc, _tor_realloc, _tor_free);
    tor_assert(r);
  }
#endif
#ifdef NT_SERVICE
  {
     int done = 0;
     result = nt_service_parse_options(argc, argv, &done);
     if (done) return result;
  }
#endif
  if (tor_init(argc, argv)<0)
    return -1;
  switch (get_options()->command) {
  case CMD_RUN_TOR:
#ifdef NT_SERVICE
    nt_service_set_state(SERVICE_RUNNING);
#endif
    result = do_main_loop();
    break;
  case CMD_LIST_FINGERPRINT:
    result = do_list_fingerprint();
    break;
  case CMD_HASH_PASSWORD:
    do_hash_password();
    result = 0;
    break;
  case CMD_VERIFY_CONFIG:
    printf("Configuration was valid\n");
    result = 0;
    break;
  case CMD_RUN_UNITTESTS: /* only set by test.c */
  default:
    log_warn(LD_BUG,"Illegal command number %d: internal error.",
             get_options()->command);
    result = -1;
  }
  tor_cleanup();
  return result;
}

