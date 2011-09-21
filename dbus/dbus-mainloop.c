/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* dbus-mainloop.c  Main loop utility
 *
 * Copyright (C) 2003, 2004  Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.1
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <config.h>
#include "dbus-mainloop.h"

#ifndef DOXYGEN_SHOULD_SKIP_THIS

#include <dbus/dbus-hash.h>
#include <dbus/dbus-list.h>
#include <dbus/dbus-sysdeps.h>
#include <dbus/dbus-watch.h>

#define MAINLOOP_SPEW 0

#if MAINLOOP_SPEW
#ifdef DBUS_ENABLE_VERBOSE_MODE
static const char*
watch_flags_to_string (int flags)
{
  const char *watch_type;

  if ((flags & DBUS_WATCH_READABLE) &&
      (flags & DBUS_WATCH_WRITABLE))
    watch_type = "readwrite";
  else if (flags & DBUS_WATCH_READABLE)
    watch_type = "read";
  else if (flags & DBUS_WATCH_WRITABLE)
    watch_type = "write";
  else
    watch_type = "not read or write";
  return watch_type;
}
#endif /* DBUS_ENABLE_VERBOSE_MODE */
#endif /* MAINLOOP_SPEW */

struct DBusLoop
{
  int refcount;
  /** fd => dbus_malloc'd DBusList ** of references to DBusWatch */
  DBusHashTable *watches;
  DBusList *timeouts;
  int callback_list_serial;
  int watch_count;
  int timeout_count;
  int depth; /**< number of recursive runs */
  DBusList *need_dispatch;
};

static short
watch_flags_to_poll_events (unsigned int flags)
{
  short events = 0;

  if (flags & DBUS_WATCH_READABLE)
    events |= _DBUS_POLLIN;
  if (flags & DBUS_WATCH_WRITABLE)
    events |= _DBUS_POLLOUT;

  return events;
}

static unsigned int
watch_flags_from_poll_revents (short revents)
{
  unsigned int condition = 0;

  if (revents & _DBUS_POLLIN)
    condition |= DBUS_WATCH_READABLE;
  if (revents & _DBUS_POLLOUT)
    condition |= DBUS_WATCH_WRITABLE;
  if (revents & _DBUS_POLLHUP)
    condition |= DBUS_WATCH_HANGUP;
  if (revents & _DBUS_POLLERR)
    condition |= DBUS_WATCH_ERROR;

  return condition;
}

typedef struct
{
  DBusTimeout *timeout;
  unsigned long last_tv_sec;
  unsigned long last_tv_usec;
} TimeoutCallback;

#define TIMEOUT_CALLBACK(callback) ((TimeoutCallback*)callback)

static TimeoutCallback*
timeout_callback_new (DBusTimeout         *timeout)
{
  TimeoutCallback *cb;

  cb = dbus_new (TimeoutCallback, 1);
  if (cb == NULL)
    return NULL;

  cb->timeout = timeout;
  _dbus_get_current_time (&cb->last_tv_sec,
                          &cb->last_tv_usec);
  return cb;
}

static void
timeout_callback_free (TimeoutCallback *cb)
{
  dbus_free (cb);
}

static void
free_watch_table_entry (void *data)
{
  DBusList **watches = data;
  DBusWatch *watch;

  /* DBusHashTable sometimes calls free_function(NULL) even if you never
   * have NULL as a value */
  if (watches == NULL)
    return;

  for (watch = _dbus_list_pop_first (watches);
      watch != NULL;
      watch = _dbus_list_pop_first (watches))
    {
      _dbus_watch_unref (watch);
    }

  _dbus_assert (*watches == NULL);
  dbus_free (watches);
}

DBusLoop*
_dbus_loop_new (void)
{
  DBusLoop *loop;

  loop = dbus_new0 (DBusLoop, 1);
  if (loop == NULL)
    return NULL;

  loop->watches = _dbus_hash_table_new (DBUS_HASH_INT, NULL,
                                        free_watch_table_entry);

  if (loop->watches == NULL)
    {
      dbus_free (loop);
      return NULL;
    }

  loop->refcount = 1;

  return loop;
}

DBusLoop *
_dbus_loop_ref (DBusLoop *loop)
{
  _dbus_assert (loop != NULL);
  _dbus_assert (loop->refcount > 0);

  loop->refcount += 1;

  return loop;
}

void
_dbus_loop_unref (DBusLoop *loop)
{
  _dbus_assert (loop != NULL);
  _dbus_assert (loop->refcount > 0);

  loop->refcount -= 1;
  if (loop->refcount == 0)
    {
      while (loop->need_dispatch)
        {
          DBusConnection *connection = _dbus_list_pop_first (&loop->need_dispatch);

          dbus_connection_unref (connection);
        }

      _dbus_hash_table_unref (loop->watches);
      dbus_free (loop);
    }
}

static DBusList **
ensure_watch_table_entry (DBusLoop *loop,
                          int       fd)
{
  DBusList **watches;

  watches = _dbus_hash_table_lookup_int (loop->watches, fd);

  if (watches == NULL)
    {
      watches = dbus_new0 (DBusList *, 1);

      if (watches == NULL)
        return watches;

      if (!_dbus_hash_table_insert_int (loop->watches, fd, watches))
        {
          dbus_free (watches);
          watches = NULL;
        }
    }

  return watches;
}

static void
cull_watches_for_invalid_fd (DBusLoop  *loop,
                             int        fd)
{
  DBusList *link;
  DBusList **watches;

  _dbus_warn ("invalid request, socket fd %d not open\n", fd);
  watches = _dbus_hash_table_lookup_int (loop->watches, fd);

  if (watches != NULL)
    {
      for (link = _dbus_list_get_first_link (watches);
          link != NULL;
          link = _dbus_list_get_next_link (watches, link))
        _dbus_watch_invalidate (link->data);
    }

  _dbus_hash_table_remove_int (loop->watches, fd);
}

static void
gc_watch_table_entry (DBusLoop  *loop,
                      DBusList **watches,
                      int        fd)
{
  /* If watches is already NULL we have nothing to do */
  if (watches == NULL)
    return;

  /* We can't GC hash table entries if they're non-empty lists */
  if (*watches != NULL)
    return;

  _dbus_hash_table_remove_int (loop->watches, fd);
}

dbus_bool_t
_dbus_loop_add_watch (DBusLoop  *loop,
                      DBusWatch *watch)
{
  int fd;
  DBusList **watches;

  fd = dbus_watch_get_socket (watch);
  _dbus_assert (fd != -1);

  watches = ensure_watch_table_entry (loop, fd);

  if (watches == NULL)
    return FALSE;

  if (_dbus_list_append (watches, _dbus_watch_ref (watch)))
    {
      loop->callback_list_serial += 1;
      loop->watch_count += 1;
    }
  else
    {
      _dbus_watch_unref (watch);
      gc_watch_table_entry (loop, watches, fd);

       return FALSE;
     }

  return TRUE;
}

void
_dbus_loop_remove_watch (DBusLoop         *loop,
                         DBusWatch        *watch)
{
  DBusList **watches;
  DBusList *link;
  int fd;

  /* This relies on people removing watches before they invalidate them,
   * which has been safe since fd.o #33336 was fixed. Assert about it
   * so we don't regress. */
  fd = dbus_watch_get_socket (watch);
  _dbus_assert (fd != -1);

  watches = _dbus_hash_table_lookup_int (loop->watches, fd);

  if (watches != NULL)
    {
      link = _dbus_list_get_first_link (watches);
      while (link != NULL)
        {
          DBusList *next = _dbus_list_get_next_link (watches, link);
          DBusWatch *this = link->data;

          if (this == watch)
            {
              _dbus_list_remove_link (watches, link);
              loop->callback_list_serial += 1;
              loop->watch_count -= 1;
              _dbus_watch_unref (this);

              /* if that was the last watch for that fd, drop the hash table
               * entry too */
              gc_watch_table_entry (loop, watches, fd);

              return;
            }

          link = next;
         }
     }

  _dbus_warn ("could not find watch %p to remove\n", watch);
}

dbus_bool_t
_dbus_loop_add_timeout (DBusLoop           *loop,
                        DBusTimeout        *timeout)
{
  TimeoutCallback *tcb;

  tcb = timeout_callback_new (timeout);
  if (tcb == NULL)
    return FALSE;

  if (_dbus_list_append (&loop->timeouts, tcb))
    {
      loop->callback_list_serial += 1;
      loop->timeout_count += 1;
    }
  else
    {
      timeout_callback_free (tcb);
      return FALSE;
    }
  
  return TRUE;
}

void
_dbus_loop_remove_timeout (DBusLoop           *loop,
                           DBusTimeout        *timeout)
{
  DBusList *link;
  
  link = _dbus_list_get_first_link (&loop->timeouts);
  while (link != NULL)
    {
      DBusList *next = _dbus_list_get_next_link (&loop->timeouts, link);
      TimeoutCallback *this = link->data;

      if (this->timeout == timeout)
        {
          _dbus_list_remove_link (&loop->timeouts, link);
          loop->callback_list_serial += 1;
          loop->timeout_count -= 1;
          timeout_callback_free (this);

          return;
        }
      
      link = next;
    }

  _dbus_warn ("could not find timeout %p to remove\n", timeout);
}

/* Convolutions from GLib, there really must be a better way
 * to do this.
 */
static dbus_bool_t
check_timeout (unsigned long    tv_sec,
               unsigned long    tv_usec,
               TimeoutCallback *tcb,
               int             *timeout)
{
  long sec_remaining;
  long msec_remaining;
  unsigned long expiration_tv_sec;
  unsigned long expiration_tv_usec;
  long interval_seconds;
  long interval_milliseconds;
  int interval;

  /* I'm pretty sure this function could suck (a lot) less */
  
  interval = dbus_timeout_get_interval (tcb->timeout);
  
  interval_seconds = interval / 1000L;
  interval_milliseconds = interval % 1000L;
  
  expiration_tv_sec = tcb->last_tv_sec + interval_seconds;
  expiration_tv_usec = tcb->last_tv_usec + interval_milliseconds * 1000;
  if (expiration_tv_usec >= 1000000)
    {
      expiration_tv_usec -= 1000000;
      expiration_tv_sec += 1;
    }
  
  sec_remaining = expiration_tv_sec - tv_sec;
  /* need to force this to be signed, as it is intended to sometimes
   * produce a negative result
   */
  msec_remaining = ((long) expiration_tv_usec - (long) tv_usec) / 1000L;

#if MAINLOOP_SPEW
  _dbus_verbose ("Interval is %ld seconds %ld msecs\n",
                 interval_seconds,
                 interval_milliseconds);
  _dbus_verbose ("Now is  %lu seconds %lu usecs\n",
                 tv_sec, tv_usec);
  _dbus_verbose ("Last is %lu seconds %lu usecs\n",
                 tcb->last_tv_sec, tcb->last_tv_usec);
  _dbus_verbose ("Exp is  %lu seconds %lu usecs\n",
                 expiration_tv_sec, expiration_tv_usec);
  _dbus_verbose ("Pre-correction, sec_remaining %ld msec_remaining %ld\n",
                 sec_remaining, msec_remaining);
#endif
  
  /* We do the following in a rather convoluted fashion to deal with
   * the fact that we don't have an integral type big enough to hold
   * the difference of two timevals in milliseconds.
   */
  if (sec_remaining < 0 || (sec_remaining == 0 && msec_remaining < 0))
    {
      *timeout = 0;
    }
  else
    {
      if (msec_remaining < 0)
	{
	  msec_remaining += 1000;
	  sec_remaining -= 1;
	}

      if (sec_remaining > (_DBUS_INT_MAX / 1000) ||
          msec_remaining > _DBUS_INT_MAX)
        *timeout = _DBUS_INT_MAX;
      else
        *timeout = sec_remaining * 1000 + msec_remaining;        
    }

  if (*timeout > interval)
    {
      /* This indicates that the system clock probably moved backward */
      _dbus_verbose ("System clock set backward! Resetting timeout.\n");
      
      tcb->last_tv_sec = tv_sec;
      tcb->last_tv_usec = tv_usec;

      *timeout = interval;
    }
  
#if MAINLOOP_SPEW
  _dbus_verbose ("  timeout expires in %d milliseconds\n", *timeout);
#endif
  
  return *timeout == 0;
}

dbus_bool_t
_dbus_loop_dispatch (DBusLoop *loop)
{

#if MAINLOOP_SPEW
  _dbus_verbose ("  %d connections to dispatch\n", _dbus_list_get_length (&loop->need_dispatch));
#endif
  
  if (loop->need_dispatch == NULL)
    return FALSE;
  
 next:
  while (loop->need_dispatch != NULL)
    {
      DBusConnection *connection = _dbus_list_pop_first (&loop->need_dispatch);
      
      while (TRUE)
        {
          DBusDispatchStatus status;
          
          status = dbus_connection_dispatch (connection);

          if (status == DBUS_DISPATCH_COMPLETE)
            {
              dbus_connection_unref (connection);
              goto next;
            }
          else
            {
              if (status == DBUS_DISPATCH_NEED_MEMORY)
                _dbus_wait_for_memory ();
            }
        }
    }

  return TRUE;
}

dbus_bool_t
_dbus_loop_queue_dispatch (DBusLoop       *loop,
                           DBusConnection *connection)
{
  if (_dbus_list_append (&loop->need_dispatch, connection))
    {
      dbus_connection_ref (connection);
      return TRUE;
    }
  else
    return FALSE;
}

/* Returns TRUE if we invoked any timeouts or have ready file
 * descriptors, which is just used in test code as a debug hack
 */

dbus_bool_t
_dbus_loop_iterate (DBusLoop     *loop,
                    dbus_bool_t   block)
{  
#define N_STACK_DESCRIPTORS 64
  dbus_bool_t retval;
  DBusPollFD *fds;
  DBusPollFD stack_fds[N_STACK_DESCRIPTORS];
  int n_fds;
  int i;
  DBusList *link;
  int n_ready;
  int initial_serial;
  long timeout;
  dbus_bool_t oom_watch_pending;
  int orig_depth;
  DBusHashIter hash_iter;

  retval = FALSE;      

  fds = NULL;
  n_fds = 0;
  oom_watch_pending = FALSE;
  orig_depth = loop->depth;
  
#if MAINLOOP_SPEW
  _dbus_verbose ("Iteration block=%d depth=%d timeout_count=%d watch_count=%d\n",
                 block, loop->depth, loop->timeout_count, loop->watch_count);
#endif

  if (_dbus_hash_table_get_n_entries (loop->watches) == 0 &&
      loop->timeouts == NULL)
    goto next_iteration;

  if (loop->watch_count > N_STACK_DESCRIPTORS)
    {
      fds = dbus_new0 (DBusPollFD, loop->watch_count);
      
      while (fds == NULL)
        {
          _dbus_wait_for_memory ();
          fds = dbus_new0 (DBusPollFD, loop->watch_count);
        }
    }
  else
    {      
      fds = stack_fds;
    }

  /* fill our array of fds and watches */
  n_fds = 0;
  _dbus_hash_iter_init (loop->watches, &hash_iter);

  while (_dbus_hash_iter_next (&hash_iter))
    {
      DBusList **watches;
      unsigned int flags;
      int fd;

      fd = _dbus_hash_iter_get_int_key (&hash_iter);
      watches = _dbus_hash_iter_get_value (&hash_iter);
      flags = 0;

      for (link = _dbus_list_get_first_link (watches);
          link != NULL;
          link = _dbus_list_get_next_link (watches, link))
        {
          DBusWatch *watch = link->data;

          if (_dbus_watch_get_oom_last_time (watch))
            {
              /* we skip this one this time, but reenable it next time,
               * and have a timeout on this iteration
               */
              _dbus_watch_set_oom_last_time (watch, FALSE);
              oom_watch_pending = TRUE;

              retval = TRUE; /* return TRUE here to keep the loop going,
                              * since we don't know the watch is inactive
                              */

#if MAINLOOP_SPEW
              _dbus_verbose ("  skipping watch on fd %d as it was out of memory last time\n",
                             fd);
#endif
            }
          else if (dbus_watch_get_enabled (watch))
            {
              flags |= dbus_watch_get_flags (watch);
            }
        }

      if (flags != 0)
        {
          fds[n_fds].fd = fd;
          fds[n_fds].revents = 0;
          fds[n_fds].events = watch_flags_to_poll_events (flags);

#if MAINLOOP_SPEW
          _dbus_verbose ("  polling watch on fd %d  %s\n",
                         loop->fds[loop->n_fds].fd, watch_flags_to_string (flags));
#endif

          n_fds += 1;
        }
      else
        {
#if MAINLOOP_SPEW
          _dbus_verbose ("  skipping disabled watch on fd %d  %s\n",
                         fd,
                         watch_flags_to_string (dbus_watch_get_flags (watch)));
#endif
        }
    }

  timeout = -1;
  if (loop->timeout_count > 0)
    {
      unsigned long tv_sec;
      unsigned long tv_usec;
      
      _dbus_get_current_time (&tv_sec, &tv_usec);

      link = _dbus_list_get_first_link (&loop->timeouts);
      while (link != NULL)
        {
          DBusList *next = _dbus_list_get_next_link (&loop->timeouts, link);
          TimeoutCallback *tcb = link->data;

          if (dbus_timeout_get_enabled (tcb->timeout))
            {
              int msecs_remaining;

              check_timeout (tv_sec, tv_usec, tcb, &msecs_remaining);

              if (timeout < 0)
                timeout = msecs_remaining;
              else
                timeout = MIN (msecs_remaining, timeout);

#if MAINLOOP_SPEW
              _dbus_verbose ("  timeout added, %d remaining, aggregate timeout %ld\n",
                             msecs_remaining, timeout);
#endif
              
              _dbus_assert (timeout >= 0);
                  
              if (timeout == 0)
                break; /* it's not going to get shorter... */
            }
#if MAINLOOP_SPEW
          else
            {
              _dbus_verbose ("  skipping disabled timeout\n");
            }
#endif
          
          link = next;
        }
    }

  /* Never block if we have stuff to dispatch */
  if (!block || loop->need_dispatch != NULL)
    {
      timeout = 0;
#if MAINLOOP_SPEW
      _dbus_verbose ("  timeout is 0 as we aren't blocking\n");
#endif
    }

  /* if a watch is OOM, don't wait longer than the OOM
   * wait to re-enable it
   */
  if (oom_watch_pending)
    timeout = MIN (timeout, _dbus_get_oom_wait ());

#if MAINLOOP_SPEW
  _dbus_verbose ("  polling on %d descriptors timeout %ld\n", n_fds, timeout);
#endif
  
  n_ready = _dbus_poll (fds, n_fds, timeout);

  initial_serial = loop->callback_list_serial;

  if (loop->timeout_count > 0)
    {
      unsigned long tv_sec;
      unsigned long tv_usec;

      _dbus_get_current_time (&tv_sec, &tv_usec);

      /* It'd be nice to avoid this O(n) thingy here */
      link = _dbus_list_get_first_link (&loop->timeouts);
      while (link != NULL)
        {
          DBusList *next = _dbus_list_get_next_link (&loop->timeouts, link);
          TimeoutCallback *tcb = link->data;

          if (initial_serial != loop->callback_list_serial)
            goto next_iteration;

          if (loop->depth != orig_depth)
            goto next_iteration;

          if (dbus_timeout_get_enabled (tcb->timeout))
            {
              int msecs_remaining;
              
              if (check_timeout (tv_sec, tv_usec,
                                 tcb, &msecs_remaining))
                {
                  /* Save last callback time and fire this timeout */
                  tcb->last_tv_sec = tv_sec;
                  tcb->last_tv_usec = tv_usec;

#if MAINLOOP_SPEW
                  _dbus_verbose ("  invoking timeout\n");
#endif

                  /* can theoretically return FALSE on OOM, but we just
                   * let it fire again later - in practice that's what
                   * every wrapper callback in dbus-daemon used to do */
                  dbus_timeout_handle (tcb->timeout);

                  retval = TRUE;
                }
              else
                {
#if MAINLOOP_SPEW
                  _dbus_verbose ("  timeout has not expired\n");
#endif
                }
            }
#if MAINLOOP_SPEW
          else
            {
              _dbus_verbose ("  skipping invocation of disabled timeout\n");
            }
#endif

          link = next;
        }
    }
      
  if (n_ready > 0)
    {
      for (i = 0; i < n_fds; i++)
        {
          DBusList **watches;
          DBusList *next;
          unsigned int condition;

          /* FIXME I think this "restart if we change the watches"
           * approach could result in starving watches
           * toward the end of the list.
           */
          if (initial_serial != loop->callback_list_serial)
            goto next_iteration;

          if (loop->depth != orig_depth)
            goto next_iteration;

          if (fds[i].revents == 0)
            continue;

          if (_DBUS_UNLIKELY (fds[i].revents & _DBUS_POLLNVAL))
            {
              cull_watches_for_invalid_fd (loop, fds[i].fd);
              goto next_iteration;
            }

          condition = watch_flags_from_poll_revents (fds[i].revents);

          /* condition may still be 0 if we got some
           * weird POLLFOO thing like POLLWRBAND
           */
          if (condition == 0)
            continue;

          watches = _dbus_hash_table_lookup_int (loop->watches, fds[i].fd);

          if (watches == NULL)
            continue;

          for (link = _dbus_list_get_first_link (watches);
              link != NULL;
              link = next)
            {
              DBusWatch *watch = link->data;

              next = _dbus_list_get_next_link (watches, link);

              if (dbus_watch_get_enabled (watch))
                {
                  dbus_bool_t oom;

                  oom = !dbus_watch_handle (watch, condition);

                  if (oom)
                    {
                      _dbus_watch_set_oom_last_time (watch, TRUE);
                    }

#if MAINLOOP_SPEW
                  _dbus_verbose ("  Invoked watch, oom = %d\n", oom);
#endif
                  retval = TRUE;

                  /* We re-check this every time, in case the callback
                   * added/removed watches, which might make our position in
                   * the linked list invalid. See the FIXME above. */
                  if (initial_serial != loop->callback_list_serial)
                    goto next_iteration;

                  if (loop->depth != orig_depth)
                    goto next_iteration;
                }
            }
        }
    }
      
 next_iteration:
#if MAINLOOP_SPEW
  _dbus_verbose ("  moving to next iteration\n");
#endif
  
  if (fds && fds != stack_fds)
    dbus_free (fds);

  if (_dbus_loop_dispatch (loop))
    retval = TRUE;
  
#if MAINLOOP_SPEW
  _dbus_verbose ("Returning %d\n", retval);
#endif
  
  return retval;
}

void
_dbus_loop_run (DBusLoop *loop)
{
  int our_exit_depth;

  _dbus_assert (loop->depth >= 0);
  
  _dbus_loop_ref (loop);
  
  our_exit_depth = loop->depth;
  loop->depth += 1;

  _dbus_verbose ("Running main loop, depth %d -> %d\n",
                 loop->depth - 1, loop->depth);
  
  while (loop->depth != our_exit_depth)
    _dbus_loop_iterate (loop, TRUE);

  _dbus_loop_unref (loop);
}

void
_dbus_loop_quit (DBusLoop *loop)
{
  _dbus_assert (loop->depth > 0);  
  
  loop->depth -= 1;

  _dbus_verbose ("Quit main loop, depth %d -> %d\n",
                 loop->depth + 1, loop->depth);
}

int
_dbus_get_oom_wait (void)
{
#ifdef DBUS_BUILD_TESTS
  /* make tests go fast */
  return 0;
#else
  return 500;
#endif
}

void
_dbus_wait_for_memory (void)
{
  _dbus_verbose ("Waiting for more memory\n");
  _dbus_sleep_milliseconds (_dbus_get_oom_wait ());
}

#endif /* !DOXYGEN_SHOULD_SKIP_THIS */
