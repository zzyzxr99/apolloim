/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * gmain.c: Main loop abstraction, timeouts, and idle functions
 * Copyright 1998 Owen Taylor
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GLib Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/. 
 */

/* 
 * MT safe
 */

#include "config.h"

/* uncomment the next line to get poll() debugging info */
/* #define G_MAIN_POLL_DEBUG */

#include "galias.h"
#include "glib.h"
#include "gthreadinit.h"
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#ifdef GLIB_HAVE_SYS_POLL_H
#  include <sys/poll.h>
#  undef events	 /* AIX 4.1.5 & 4.3.2 define this for SVR3,4 compatibility */
#  undef revents /* AIX 4.1.5 & 4.3.2 define this for SVR3,4 compatibility */

/* The poll() emulation on OS/X doesn't handle fds=NULL, nfds=0,
 * so we prefer our own poll emulation.
 */
#ifdef _POLL_EMUL_H_
#undef HAVE_POLL
#endif
   
#endif /* GLIB_HAVE_SYS_POLL_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <errno.h>

#ifdef G_OS_WIN32
#define STRICT
#include <windows.h>
#endif /* G_OS_WIN32 */

#ifdef G_OS_BEOS
#include <net/socket.h>
#endif /* G_OS_BEOS */

#ifdef G_OS_UNIX
#include <fcntl.h>
#include <sys/wait.h>
#endif
/* Types */

typedef struct _GTimeoutSource GTimeoutSource;
typedef struct _GChildWatchSource GChildWatchSource;
typedef struct _GPollRec GPollRec;
typedef struct _GSourceCallback GSourceCallback;

typedef enum
{
  G_SOURCE_READY = 1 << G_HOOK_FLAG_USER_SHIFT,
  G_SOURCE_CAN_RECURSE = 1 << (G_HOOK_FLAG_USER_SHIFT + 1)
} GSourceFlags;

#ifdef G_THREADS_ENABLED
typedef struct _GMainWaiter GMainWaiter;

struct _GMainWaiter
{
  GCond *cond;
  GMutex *mutex;
};
#endif  

struct _GMainContext
{
#ifdef G_THREADS_ENABLED
  /* The following lock is used for both the list of sources
   * and the list of poll records
   */
  GStaticMutex mutex;
  GCond *cond;
  GThread *owner;
  guint owner_count;
  GSList *waiters;
#endif  

  gint ref_count;

  GPtrArray *pending_dispatches;
  gint timeout;			/* Timeout for current iteration */

  guint next_id;
  GSource *source_list;
  gint in_check_or_prepare;

  GPollRec *poll_records;
  GPollRec *poll_free_list;
  GMemChunk *poll_chunk;
  guint n_poll_records;
  GPollFD *cached_poll_array;
  guint cached_poll_array_size;

#ifdef G_THREADS_ENABLED  
#ifndef G_OS_WIN32
/* this pipe is used to wake up the main loop when a source is added.
 */
  gint wake_up_pipe[2];
#else /* G_OS_WIN32 */
  HANDLE wake_up_semaphore;
#endif /* G_OS_WIN32 */

  GPollFD wake_up_rec;
  gboolean poll_waiting;

/* Flag indicating whether the set of fd's changed during a poll */
  gboolean poll_changed;
#endif /* G_THREADS_ENABLED */

  GPollFunc poll_func;

  GTimeVal current_time;
  gboolean time_is_current;
};

struct _GSourceCallback
{
  guint ref_count;
  GSourceFunc func;
  gpointer    data;
  GDestroyNotify notify;
};

struct _GMainLoop
{
  GMainContext *context;
  gboolean is_running;
  gint ref_count;
};

struct _GTimeoutSource
{
  GSource     source;
  GTimeVal    expiration;
  guint       interval;
};

struct _GChildWatchSource
{
  GSource     source;
  GPid        pid;
  gint        child_status;
#ifdef G_OS_WIN32
  GPollFD     poll;
#else /* G_OS_WIN32 */
  gint        count;
  gboolean    child_exited;
#endif /* G_OS_WIN32 */
};

struct _GPollRec
{
  gint priority;
  GPollFD *fd;
  GPollRec *next;
};

#ifdef G_THREADS_ENABLED
#define LOCK_CONTEXT(context) g_static_mutex_lock (&context->mutex)
#define UNLOCK_CONTEXT(context) g_static_mutex_unlock (&context->mutex)
#define G_THREAD_SELF g_thread_self ()
#else
#define LOCK_CONTEXT(context) (void)0
#define UNLOCK_CONTEXT(context) (void)0
#define G_THREAD_SELF NULL
#endif

#define SOURCE_DESTROYED(source) (((source)->flags & G_HOOK_FLAG_ACTIVE) == 0)
#define SOURCE_BLOCKED(source) (((source)->flags & G_HOOK_FLAG_IN_CALL) != 0 && \
		                ((source)->flags & G_SOURCE_CAN_RECURSE) == 0)

#define SOURCE_UNREF(source, context)                       \
   G_STMT_START {                                           \
    if ((source)->ref_count > 1)                            \
      (source)->ref_count--;                                \
    else                                                    \
      g_source_unref_internal ((source), (context), TRUE);  \
   } G_STMT_END


/* Forward declarations */

static void g_source_unref_internal             (GSource      *source,
						 GMainContext *context,
						 gboolean      have_lock);
static void g_source_destroy_internal           (GSource      *source,
						 GMainContext *context,
						 gboolean      have_lock);
static void g_main_context_poll                 (GMainContext *context,
						 gint          timeout,
						 gint          priority,
						 GPollFD      *fds,
						 gint          n_fds);
static void g_main_context_add_poll_unlocked    (GMainContext *context,
						 gint          priority,
						 GPollFD      *fd);
static void g_main_context_remove_poll_unlocked (GMainContext *context,
						 GPollFD      *fd);
static void g_main_context_wakeup_unlocked      (GMainContext *context);

static gboolean g_timeout_prepare  (GSource     *source,
				    gint        *timeout);
static gboolean g_timeout_check    (GSource     *source);
static gboolean g_timeout_dispatch (GSource     *source,
				    GSourceFunc  callback,
				    gpointer     user_data);
static gboolean g_child_watch_prepare  (GSource     *source,
				        gint        *timeout);
static gboolean g_child_watch_check    (GSource     *source);
static gboolean g_child_watch_dispatch (GSource     *source,
					GSourceFunc  callback,
					gpointer     user_data);
static gboolean g_idle_prepare     (GSource     *source,
				    gint        *timeout);
static gboolean g_idle_check       (GSource     *source);
static gboolean g_idle_dispatch    (GSource     *source,
				    GSourceFunc  callback,
				    gpointer     user_data);

G_LOCK_DEFINE_STATIC (main_loop);
static GMainContext *default_main_context;
static GSList *main_contexts_without_pipe = NULL;

#ifndef G_OS_WIN32
/* Child status monitoring code */
enum {
  CHILD_WATCH_UNINITIALIZED,
  CHILD_WATCH_INITIALIZED_SINGLE,
  CHILD_WATCH_INITIALIZED_THREADED
};
static gint child_watch_init_state = CHILD_WATCH_UNINITIALIZED;
static gint child_watch_count = 1;
static gint child_watch_wake_up_pipe[2] = {0, 0};
#endif /* !G_OS_WIN32 */
G_LOCK_DEFINE_STATIC (main_context_list);
static GSList *main_context_list = NULL;

GSourceFuncs g_timeout_funcs =
{
  g_timeout_prepare,
  g_timeout_check,
  g_timeout_dispatch,
  NULL
};

GSourceFuncs g_child_watch_funcs =
{
  g_child_watch_prepare,
  g_child_watch_check,
  g_child_watch_dispatch,
  NULL
};

GSourceFuncs g_idle_funcs =
{
  g_idle_prepare,
  g_idle_check,
  g_idle_dispatch,
  NULL
};

#ifdef HAVE_POLL
/* SunOS has poll, but doesn't provide a prototype. */
#  if defined (sun) && !defined (__SVR4)
extern gint poll (GPollFD *ufds, guint nfsd, gint timeout);
#  endif  /* !sun */
#else	/* !HAVE_POLL */

#ifdef G_OS_WIN32

static gint
g_poll (GPollFD *fds,
	guint    nfds,
	gint     timeout)
{
  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  gboolean poll_msgs = FALSE;
  GPollFD *f;
  DWORD ready;
  MSG msg;
  UINT timer;
  gint nhandles = 0;

  for (f = fds; f < &fds[nfds]; ++f)
    if (f->fd >= 0)
      {
	if (f->fd == G_WIN32_MSG_HANDLE)
	  poll_msgs = TRUE;
	else if (nhandles == MAXIMUM_WAIT_OBJECTS)
	  {
	    g_warning (G_STRLOC ": Too many handles to wait for!\n");
	    break;
	  }
	else
	  {
#ifdef G_MAIN_POLL_DEBUG
	    g_print ("g_poll: waiting for %#x\n", f->fd);
#endif
	    handles[nhandles++] = (HANDLE) f->fd;
	  }
      }

  if (timeout == -1)
    timeout = INFINITE;

  if (poll_msgs)
    {
      /* Waiting for messages, and maybe events
       * -> First PeekMessage
       */
#ifdef G_MAIN_POLL_DEBUG
      g_print ("PeekMessage\n");
#endif
      if (PeekMessage (&msg, NULL, 0, 0, PM_NOREMOVE))
	ready = WAIT_OBJECT_0 + nhandles;
      else
	{
	  if (nhandles == 0)
	    {
	      /* Waiting just for messages */
	      if (timeout == INFINITE)
		{
		  /* Infinite timeout
		   * -> WaitMessage
		   */
#ifdef G_MAIN_POLL_DEBUG
		  g_print ("WaitMessage\n");
#endif
		  if (!WaitMessage ())
		    {
		      gchar *emsg = g_win32_error_message (GetLastError ());
		      g_warning (G_STRLOC ": WaitMessage() failed: %s", emsg);
		      g_free (emsg);
		    }
		  ready = WAIT_OBJECT_0 + nhandles;
		}
	      else if (timeout == 0)
		{
		  /* Waiting just for messages, zero timeout.
		   * If we got here, there was no message
		   */
		  ready = WAIT_TIMEOUT;
		}
	      else
		{
		  /* Waiting just for messages, some timeout
		   * -> Set a timer, wait for message,
		   * kill timer, use PeekMessage
		   */
		  timer = SetTimer (NULL, 0, timeout, NULL);
		  if (timer == 0)
		    {
		      gchar *emsg = g_win32_error_message (GetLastError ());
		      g_warning (G_STRLOC ": SetTimer() failed: %s", emsg);
		      g_free (emsg);
		      ready = WAIT_TIMEOUT;
		    }
		  else
		    {
#ifdef G_MAIN_POLL_DEBUG
		      g_print ("WaitMessage\n");
#endif
		      WaitMessage ();
		      KillTimer (NULL, timer);
#ifdef G_MAIN_POLL_DEBUG
		      g_print ("PeekMessage\n");
#endif
		      if (PeekMessage (&msg, NULL, 0, 0, PM_NOREMOVE)
			  && msg.message != WM_TIMER)
			ready = WAIT_OBJECT_0;
		      else
			ready = WAIT_TIMEOUT;
		    }
		}
	    }
	  else
	    {
	      /* Wait for either message or event
	       * -> Use MsgWaitForMultipleObjects
	       */
#ifdef G_MAIN_POLL_DEBUG
	      g_print ("MsgWaitForMultipleObjects(%d, %d)\n", nhandles, timeout);
#endif
	      ready = MsgWaitForMultipleObjects (nhandles, handles, FALSE,
						 timeout, QS_ALLINPUT);

	      if (ready == WAIT_FAILED)
		{
		  gchar *emsg = g_win32_error_message (GetLastError ());
		  g_warning (G_STRLOC ": MsgWaitForMultipleObjects() failed: %s", emsg);
		  g_free (emsg);
		}
	    }
	}
    }
  else if (nhandles == 0)
    {
      /* Wait for nothing (huh?) */
      return 0;
    }
  else
    {
      /* Wait for just events
       * -> Use WaitForMultipleObjects
       */
#ifdef G_MAIN_POLL_DEBUG
      g_print ("WaitForMultipleObjects(%d, %d)\n", nhandles, timeout);
#endif
      ready = WaitForMultipleObjects (nhandles, handles, FALSE, timeout);
      if (ready == WAIT_FAILED)
	{
	  gchar *emsg = g_win32_error_message (GetLastError ());
	  g_warning (G_STRLOC ": WaitForMultipleObjects() failed: %s", emsg);
	  g_free (emsg);
	}
    }

#ifdef G_MAIN_POLL_DEBUG
  g_print ("wait returns %ld%s\n",
	   ready,
	   (ready == WAIT_FAILED ? " (WAIT_FAILED)" :
	    (ready == WAIT_TIMEOUT ? " (WAIT_TIMEOUT)" :
	     (poll_msgs && ready == WAIT_OBJECT_0 + nhandles ? " (msg)" : ""))));
#endif
  for (f = fds; f < &fds[nfds]; ++f)
    f->revents = 0;

  if (ready == WAIT_FAILED)
    return -1;
  else if (ready == WAIT_TIMEOUT)
    return 0;
  else if (poll_msgs && ready == WAIT_OBJECT_0 + nhandles)
    {
      for (f = fds; f < &fds[nfds]; ++f)
	if (f->fd >= 0)
	  {
	    if (f->events & G_IO_IN)
	      if (f->fd == G_WIN32_MSG_HANDLE)
		f->revents |= G_IO_IN;
	  }
    }
#if 1 /* TEST_WITHOUT_THIS */
  else if (ready >= WAIT_OBJECT_0 && ready < WAIT_OBJECT_0 + nhandles)
    for (f = fds; f < &fds[nfds]; ++f)
      {
	if ((f->events & (G_IO_IN | G_IO_OUT))
	    && f->fd == (gint) handles[ready - WAIT_OBJECT_0])
	  {
	    if (f->events & G_IO_IN)
	      f->revents |= G_IO_IN;
	    else
	      f->revents |= G_IO_OUT;
#ifdef G_MAIN_POLL_DEBUG
	    g_print ("g_poll: got event %#x\n", f->fd);
#endif
#if 0
	    ResetEvent ((HANDLE) f->fd);
#endif
	  }
      }
#endif
    
  return 1;
}

#else  /* !G_OS_WIN32 */

/* The following implementation of poll() comes from the GNU C Library.
 * Copyright (C) 1994, 1996, 1997 Free Software Foundation, Inc.
 */

#include <string.h> /* for bzero on BSD systems */

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif /* HAVE_SYS_SELECT_H */

#ifdef G_OS_BEOS
#undef NO_FD_SET
#endif /* G_OS_BEOS */

#ifndef NO_FD_SET
#  define SELECT_MASK fd_set
#else /* !NO_FD_SET */
#  ifndef _AIX
typedef long fd_mask;
#  endif /* _AIX */
#  ifdef _IBMR2
#    define SELECT_MASK void
#  else /* !_IBMR2 */
#    define SELECT_MASK int
#  endif /* !_IBMR2 */
#endif /* !NO_FD_SET */

static gint 
g_poll (GPollFD *fds,
	guint    nfds,
	gint     timeout)
{
  struct timeval tv;
  SELECT_MASK rset, wset, xset;
  GPollFD *f;
  int ready;
  int maxfd = 0;

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&xset);

  for (f = fds; f < &fds[nfds]; ++f)
    if (f->fd >= 0)
      {
	if (f->events & G_IO_IN)
	  FD_SET (f->fd, &rset);
	if (f->events & G_IO_OUT)
	  FD_SET (f->fd, &wset);
	if (f->events & G_IO_PRI)
	  FD_SET (f->fd, &xset);
	if (f->fd > maxfd && (f->events & (G_IO_IN|G_IO_OUT|G_IO_PRI)))
	  maxfd = f->fd;
      }

  tv.tv_sec = timeout / 1000;
  tv.tv_usec = (timeout % 1000) * 1000;

  ready = select (maxfd + 1, &rset, &wset, &xset,
		  timeout == -1 ? NULL : &tv);
  if (ready > 0)
    for (f = fds; f < &fds[nfds]; ++f)
      {
	f->revents = 0;
	if (f->fd >= 0)
	  {
	    if (FD_ISSET (f->fd, &rset))
	      f->revents |= G_IO_IN;
	    if (FD_ISSET (f->fd, &wset))
	      f->revents |= G_IO_OUT;
	    if (FD_ISSET (f->fd, &xset))
	      f->revents |= G_IO_PRI;
	  }
      }

  return ready;
}

#endif /* !G_OS_WIN32 */

#endif	/* !HAVE_POLL */

/**
 * g_main_context_ref:
 * @context: a #GMainContext
 * 
 * Increases the reference count on a #GMainContext object by one.
 *
 * Returns: the @context that was passed in (since 2.6)
 **/
GMainContext *
g_main_context_ref (GMainContext *context)
{
  g_return_val_if_fail (context != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&context->ref_count) > 0, NULL); 

  g_atomic_int_inc (&context->ref_count);

  return context;
}

/* If DISABLE_MEM_POOLS is defined, then freeing the
 * mem chunk won't free the records, so we have to
 * do it manually. The conditionalization here is
 * an optimization; g_mem_chunk_free() is a no-op
 * when DISABLE_MEM_POOLS is set.
 */
#ifdef DISABLE_MEM_POOLS
static void
poll_rec_list_free (GMainContext *context,
		    GPollRec     *list)
{
  while (list)
    {
      GPollRec *tmp_rec = list;
      list = list->next;
      g_chunk_free (tmp_rec, context->poll_chunk);
    }
}
#endif /* DISABLE_MEM_POOLS */

/**
 * g_main_context_unref:
 * @context: a #GMainContext
 * 
 * Decreases the reference count on a #GMainContext object by one. If
 * the result is zero, free the context and free all associated memory.
 **/
void
g_main_context_unref (GMainContext *context)
{
  GSource *source;
  g_return_if_fail (context != NULL);
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0); 

  if (!g_atomic_int_dec_and_test (&context->ref_count))
    return;

  G_LOCK (main_context_list);
  main_context_list = g_slist_remove (main_context_list, context);
  G_UNLOCK (main_context_list);

  source = context->source_list;
  while (source)
    {
      GSource *next = source->next;
      g_source_destroy_internal (source, context, TRUE);
      source = next;
    }

#ifdef G_THREADS_ENABLED  
  g_static_mutex_free (&context->mutex);
#endif

  g_ptr_array_free (context->pending_dispatches, TRUE);
  g_free (context->cached_poll_array);

#ifdef DISABLE_MEM_POLLS
  poll_rec_list_free (context, context->poll_records);
  poll_rec_list_free (context, context->poll_free_list);
#endif /* DISABLE_MEM_POOLS */
  
  if (context->poll_chunk) 
    g_mem_chunk_destroy (context->poll_chunk);

#ifdef G_THREADS_ENABLED
  if (g_thread_supported())
    {
#ifndef G_OS_WIN32
      close (context->wake_up_pipe[0]);
      close (context->wake_up_pipe[1]);
#else
      CloseHandle (context->wake_up_semaphore);
#endif
    } 
  else
    main_contexts_without_pipe = g_slist_remove (main_contexts_without_pipe, 
						 context);
#endif
  
  g_free (context);
}

#ifdef G_THREADS_ENABLED
static void 
g_main_context_init_pipe (GMainContext *context)
{
# ifndef G_OS_WIN32
  if (context->wake_up_pipe[0] != -1)
    return;
  if (pipe (context->wake_up_pipe) < 0)
    g_error ("Cannot create pipe main loop wake-up: %s\n",
	     g_strerror (errno));
  
  context->wake_up_rec.fd = context->wake_up_pipe[0];
  context->wake_up_rec.events = G_IO_IN;
# else
  if (context->wake_up_semaphore != NULL)
    return;
  context->wake_up_semaphore = CreateSemaphore (NULL, 0, 100, NULL);
  if (context->wake_up_semaphore == NULL)
    g_error ("Cannot create wake-up semaphore: %s",
	     g_win32_error_message (GetLastError ()));
  context->wake_up_rec.fd = (gint) context->wake_up_semaphore;
  context->wake_up_rec.events = G_IO_IN;
#  ifdef G_MAIN_POLL_DEBUG
  g_print ("wake-up semaphore: %#x\n", (guint) context->wake_up_semaphore);
#  endif
# endif
  g_main_context_add_poll_unlocked (context, 0, &context->wake_up_rec);
}

void
_g_main_thread_init (void)
{
  GSList *curr = main_contexts_without_pipe;
  while (curr)
    {
      g_main_context_init_pipe ((GMainContext *)curr->data);
      curr = curr->next;
    }
  g_slist_free (main_contexts_without_pipe);
  main_contexts_without_pipe = NULL;  
}
#endif /* G_THREADS_ENABLED */

/**
 * g_main_context_new:
 * 
 * Creates a new #GMainContext strcuture
 * 
 * Return value: the new #GMainContext
 **/
GMainContext *
g_main_context_new (void)
{
  GMainContext *context = g_new0 (GMainContext, 1);

#ifdef G_THREADS_ENABLED
  g_static_mutex_init (&context->mutex);

  context->owner = NULL;
  context->waiters = NULL;

# ifndef G_OS_WIN32
  context->wake_up_pipe[0] = -1;
  context->wake_up_pipe[1] = -1;
# else
  context->wake_up_semaphore = NULL;
# endif
#endif

  context->ref_count = 1;

  context->next_id = 1;
  
  context->source_list = NULL;
  
#if HAVE_POLL
  context->poll_func = (GPollFunc)poll;
#else
  context->poll_func = g_poll;
#endif
  
  context->cached_poll_array = NULL;
  context->cached_poll_array_size = 0;
  
  context->pending_dispatches = g_ptr_array_new ();
  
  context->time_is_current = FALSE;
  
#ifdef G_THREADS_ENABLED
  if (g_thread_supported ())
    g_main_context_init_pipe (context);
  else
    main_contexts_without_pipe = g_slist_prepend (main_contexts_without_pipe, 
						  context);
#endif

  G_LOCK (main_context_list);
  main_context_list = g_slist_append (main_context_list, context);
  G_UNLOCK (main_context_list);

  return context;
}

/**
 * g_main_context_default:
 * 
 * Returns the default main context. This is the main context used
 * for main loop functions when a main loop is not explicitly
 * specified.
 * 
 * Return value: the default main context.
 **/
GMainContext *
g_main_context_default (void)
{
  /* Slow, but safe */
  
  G_LOCK (main_loop);

  if (!default_main_context)
    default_main_context = g_main_context_new ();

  G_UNLOCK (main_loop);

  return default_main_context;
}

/* Hooks for adding to the main loop */

/**
 * g_source_new:
 * @source_funcs: structure containing functions that implement
 *                the sources behavior.
 * @struct_size: size of the #GSource structure to create.
 * 
 * Creates a new #GSource structure. The size is specified to
 * allow creating structures derived from #GSource that contain
 * additional data. The size passed in must be at least
 * <literal>sizeof (GSource)</literal>.
 * 
 * The source will not initially be associated with any #GMainContext
 * and must be added to one with g_source_attach() before it will be
 * executed.
 * 
 * Return value: the newly-created #GSource.
 **/
GSource *
g_source_new (GSourceFuncs *source_funcs,
	      guint         struct_size)
{
  GSource *source;

  g_return_val_if_fail (source_funcs != NULL, NULL);
  g_return_val_if_fail (struct_size >= sizeof (GSource), NULL);
  
  source = (GSource*) g_malloc0 (struct_size);

  source->source_funcs = source_funcs;
  source->ref_count = 1;
  
  source->priority = G_PRIORITY_DEFAULT;

  source->flags = G_HOOK_FLAG_ACTIVE;

  /* NULL/0 initialization for all other fields */
  
  return source;
}

/* Holds context's lock
 */
static void
g_source_list_add (GSource      *source,
		   GMainContext *context)
{
  GSource *tmp_source, *last_source;
  
  last_source = NULL;
  tmp_source = context->source_list;
  while (tmp_source && tmp_source->priority <= source->priority)
    {
      last_source = tmp_source;
      tmp_source = tmp_source->next;
    }

  source->next = tmp_source;
  if (tmp_source)
    tmp_source->prev = source;
  
  source->prev = last_source;
  if (last_source)
    last_source->next = source;
  else
    context->source_list = source;
}

/* Holds context's lock
 */
static void
g_source_list_remove (GSource      *source,
		      GMainContext *context)
{
  if (source->prev)
    source->prev->next = source->next;
  else
    context->source_list = source->next;

  if (source->next)
    source->next->prev = source->prev;

  source->prev = NULL;
  source->next = NULL;
}

/**
 * g_source_attach:
 * @source: a #GSource
 * @context: a #GMainContext (if %NULL, the default context will be used)
 * 
 * Adds a #GSource to a @context so that it will be executed within
 * that context.
 *
 * Return value: the ID for the source within the #GMainContext
 **/
guint
g_source_attach (GSource      *source,
		 GMainContext *context)
{
  guint result = 0;
  GSList *tmp_list;

  g_return_val_if_fail (source->context == NULL, 0);
  g_return_val_if_fail (!SOURCE_DESTROYED (source), 0);
  
  if (!context)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);

  source->context = context;
  result = source->source_id = context->next_id++;

  source->ref_count++;
  g_source_list_add (source, context);

  tmp_list = source->poll_fds;
  while (tmp_list)
    {
      g_main_context_add_poll_unlocked (context, source->priority, tmp_list->data);
      tmp_list = tmp_list->next;
    }

#ifdef G_THREADS_ENABLED
  /* Now wake up the main loop if it is waiting in the poll() */
  g_main_context_wakeup_unlocked (context);
#endif

  UNLOCK_CONTEXT (context);

  return result;
}

static void
g_source_destroy_internal (GSource      *source,
			   GMainContext *context,
			   gboolean      have_lock)
{
  if (!have_lock)
    LOCK_CONTEXT (context);
  
  if (!SOURCE_DESTROYED (source))
    {
      GSList *tmp_list;
      gpointer old_cb_data;
      GSourceCallbackFuncs *old_cb_funcs;
      
      source->flags &= ~G_HOOK_FLAG_ACTIVE;

      old_cb_data = source->callback_data;
      old_cb_funcs = source->callback_funcs;

      source->callback_data = NULL;
      source->callback_funcs = NULL;

      if (old_cb_funcs)
	{
	  UNLOCK_CONTEXT (context);
	  old_cb_funcs->unref (old_cb_data);
	  LOCK_CONTEXT (context);
	}

      if (!SOURCE_BLOCKED (source))
	{
	  tmp_list = source->poll_fds;
	  while (tmp_list)
	    {
	      g_main_context_remove_poll_unlocked (context, tmp_list->data);
	      tmp_list = tmp_list->next;
	    }
	}
	  
      g_source_unref_internal (source, context, TRUE);
    }

  if (!have_lock)
    UNLOCK_CONTEXT (context);
}

/**
 * g_source_destroy:
 * @source: a #GSource
 * 
 * Removes a source from its #GMainContext, if any, and mark it as
 * destroyed.  The source cannot be subsequently added to another
 * context.
 **/
void
g_source_destroy (GSource *source)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  
  context = source->context;
  
  if (context)
    g_source_destroy_internal (source, context, FALSE);
  else
    source->flags &= ~G_HOOK_FLAG_ACTIVE;
}

/**
 * g_source_get_id:
 * @source: a #GSource
 * 
 * Returns the numeric ID for a particular source. The ID of a source
 * is unique within a particular main loop context. The reverse
 * mapping from ID to source is done by g_main_context_find_source_by_id().
 *
 * Return value: the ID for the source
 **/
guint
g_source_get_id (GSource *source)
{
  guint result;
  
  g_return_val_if_fail (source != NULL, 0);
  g_return_val_if_fail (source->context != NULL, 0);

  LOCK_CONTEXT (source->context);
  result = source->source_id;
  UNLOCK_CONTEXT (source->context);
  
  return result;
}

/**
 * g_source_get_context:
 * @source: a #GSource
 * 
 * Gets the #GMainContext with which the source is associated.
 * Calling this function on a destroyed source is an error.
 * 
 * Return value: the #GMainContext with which the source is associated,
 *               or %NULL if the context has not yet been added
 *               to a source.
 **/
GMainContext *
g_source_get_context (GSource *source)
{
  g_return_val_if_fail (!SOURCE_DESTROYED (source), NULL);

  return source->context;
}

/**
 * g_source_add_poll:
 * @source:a #GSource 
 * @fd: a #GPollFD structure holding information about a file
 *      descriptor to watch.
 * 
 * Adds a file descriptor to the set of file descriptors polled for
 * this source. This is usually combined with g_source_new() to add an
 * event source. The event source's check function will typically test
 * the @revents field in the #GPollFD struct and return %TRUE if events need
 * to be processed.
 **/
void
g_source_add_poll (GSource *source,
		   GPollFD *fd)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (fd != NULL);
  g_return_if_fail (!SOURCE_DESTROYED (source));
  
  context = source->context;

  if (context)
    LOCK_CONTEXT (context);
  
  source->poll_fds = g_slist_prepend (source->poll_fds, fd);

  if (context)
    {
      if (!SOURCE_BLOCKED (source))
	g_main_context_add_poll_unlocked (context, source->priority, fd);
      UNLOCK_CONTEXT (context);
    }
}

/**
 * g_source_remove_poll:
 * @source:a #GSource 
 * @fd: a #GPollFD structure previously passed to g_source_add_poll().
 * 
 * Removes a file descriptor from the set of file descriptors polled for
 * this source. 
 **/
void
g_source_remove_poll (GSource *source,
		      GPollFD *fd)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (fd != NULL);
  g_return_if_fail (!SOURCE_DESTROYED (source));
  
  context = source->context;

  if (context)
    LOCK_CONTEXT (context);
  
  source->poll_fds = g_slist_remove (source->poll_fds, fd);

  if (context)
    {
      if (!SOURCE_BLOCKED (source))
	g_main_context_remove_poll_unlocked (context, fd);
      UNLOCK_CONTEXT (context);
    }
}

/**
 * g_source_set_callback_indirect:
 * @source: the source
 * @callback_data: pointer to callback data "object"
 * @callback_funcs: functions for reference counting @callback_data
 *                  and getting the callback and data
 * 
 * Sets the callback function storing the data as a refcounted callback
 * "object". This is used internally. Note that calling 
 * g_source_set_callback_indirect() assumes
 * an initial reference count on @callback_data, and thus
 * @callback_funcs->unref will eventually be called once more
 * than @callback_funcs->ref.
 **/
void
g_source_set_callback_indirect (GSource              *source,
				gpointer              callback_data,
				GSourceCallbackFuncs *callback_funcs)
{
  GMainContext *context;
  gpointer old_cb_data;
  GSourceCallbackFuncs *old_cb_funcs;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (callback_funcs != NULL || callback_data == NULL);

  context = source->context;

  if (context)
    LOCK_CONTEXT (context);

  old_cb_data = source->callback_data;
  old_cb_funcs = source->callback_funcs;

  source->callback_data = callback_data;
  source->callback_funcs = callback_funcs;
  
  if (context)
    UNLOCK_CONTEXT (context);
  
  if (old_cb_funcs)
    old_cb_funcs->unref (old_cb_data);
}

static void
g_source_callback_ref (gpointer cb_data)
{
  GSourceCallback *callback = cb_data;

  callback->ref_count++;
}


static void
g_source_callback_unref (gpointer cb_data)
{
  GSourceCallback *callback = cb_data;

  callback->ref_count--;
  if (callback->ref_count == 0)
    {
      if (callback->notify)
	callback->notify (callback->data);
      g_free (callback);
    }
}

static void
g_source_callback_get (gpointer     cb_data,
		       GSource     *source, 
		       GSourceFunc *func,
		       gpointer    *data)
{
  GSourceCallback *callback = cb_data;

  *func = callback->func;
  *data = callback->data;
}

static GSourceCallbackFuncs g_source_callback_funcs = {
  g_source_callback_ref,
  g_source_callback_unref,
  g_source_callback_get,
};

/**
 * g_source_set_callback:
 * @source: the source
 * @func: a callback function
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
 * 
 * Sets the callback function for a source. The callback for a source is
 * called from the source's dispatch function.
 *
 * The exact type of @func depends on the type of source; ie. you
 * should not count on @func being called with @data as its first
 * parameter.
 * 
 * Typically, you won't use this function. Instead use functions specific
 * to the type of source you are using.
 **/
void
g_source_set_callback (GSource        *source,
		       GSourceFunc     func,
		       gpointer        data,
		       GDestroyNotify  notify)
{
  GSourceCallback *new_callback;

  g_return_if_fail (source != NULL);

  new_callback = g_new (GSourceCallback, 1);

  new_callback->ref_count = 1;
  new_callback->func = func;
  new_callback->data = data;
  new_callback->notify = notify;

  g_source_set_callback_indirect (source, new_callback, &g_source_callback_funcs);
}

/**
 * g_source_set_priority:
 * @source: a #GSource
 * @priority: the new priority.
 * 
 * Sets the priority of a source. While the main loop is being
 * run, a source will be dispatched if it is ready to be dispatched and no sources 
 * at a higher (numerically smaller) priority are ready to be dispatched.
 **/
void
g_source_set_priority (GSource  *source,
		       gint      priority)
{
  GSList *tmp_list;
  GMainContext *context;
  
  g_return_if_fail (source != NULL);

  context = source->context;

  if (context)
    LOCK_CONTEXT (context);
  
  source->priority = priority;

  if (context)
    {
      /* Remove the source from the context's source and then
       * add it back so it is sorted in the correct plcae
       */
      g_source_list_remove (source, source->context);
      g_source_list_add (source, source->context);

      if (!SOURCE_BLOCKED (source))
	{
	  tmp_list = source->poll_fds;
	  while (tmp_list)
	    {
	      g_main_context_remove_poll_unlocked (context, tmp_list->data);
	      g_main_context_add_poll_unlocked (context, priority, tmp_list->data);
	      
	      tmp_list = tmp_list->next;
	    }
	}
      
      UNLOCK_CONTEXT (source->context);
    }
}

/**
 * g_source_get_priority:
 * @source: a #GSource
 * 
 * Gets the priority of a source.
 * 
 * Return value: the priority of the source
 **/
gint
g_source_get_priority (GSource *source)
{
  g_return_val_if_fail (source != NULL, 0);

  return source->priority;
}

/**
 * g_source_set_can_recurse:
 * @source: a #GSource
 * @can_recurse: whether recursion is allowed for this source
 * 
 * Sets whether a source can be called recursively. If @can_recurse is
 * %TRUE, then while the source is being dispatched then this source
 * will be processed normally. Otherwise, all processing of this
 * source is blocked until the dispatch function returns.
 **/
void
g_source_set_can_recurse (GSource  *source,
			  gboolean  can_recurse)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);

  context = source->context;

  if (context)
    LOCK_CONTEXT (context);
  
  if (can_recurse)
    source->flags |= G_SOURCE_CAN_RECURSE;
  else
    source->flags &= ~G_SOURCE_CAN_RECURSE;

  if (context)
    UNLOCK_CONTEXT (context);
}

/**
 * g_source_get_can_recurse:
 * @source: a #GSource
 * 
 * Checks whether a source is allowed to be called recursively.
 * see g_source_set_can_recurse().
 * 
 * Return value: whether recursion is allowed.
 **/
gboolean
g_source_get_can_recurse (GSource  *source)
{
  g_return_val_if_fail (source != NULL, FALSE);
  
  return (source->flags & G_SOURCE_CAN_RECURSE) != 0;
}

/**
 * g_source_ref:
 * @source: a #GSource
 * 
 * Increases the reference count on a source by one.
 * 
 * Return value: @source
 **/
GSource *
g_source_ref (GSource *source)
{
  GMainContext *context;
  
  g_return_val_if_fail (source != NULL, NULL);

  context = source->context;

  if (context)
    LOCK_CONTEXT (context);

  source->ref_count++;

  if (context)
    UNLOCK_CONTEXT (context);

  return source;
}

/* g_source_unref() but possible to call within context lock
 */
static void
g_source_unref_internal (GSource      *source,
			 GMainContext *context,
			 gboolean      have_lock)
{
  gpointer old_cb_data = NULL;
  GSourceCallbackFuncs *old_cb_funcs = NULL;

  g_return_if_fail (source != NULL);
  
  if (!have_lock && context)
    LOCK_CONTEXT (context);

  source->ref_count--;
  if (source->ref_count == 0)
    {
      old_cb_data = source->callback_data;
      old_cb_funcs = source->callback_funcs;

      source->callback_data = NULL;
      source->callback_funcs = NULL;

      if (context && !SOURCE_DESTROYED (source))
	{
	  g_warning (G_STRLOC ": ref_count == 0, but source is still attached to a context!");
	  source->ref_count++;
	}
      else if (context)
	g_source_list_remove (source, context);

      if (source->source_funcs->finalize)
	source->source_funcs->finalize (source);
      
      g_slist_free (source->poll_fds);
      source->poll_fds = NULL;
      g_free (source);
    }
  
  if (!have_lock && context)
    UNLOCK_CONTEXT (context);

  if (old_cb_funcs)
    {
      if (have_lock)
	UNLOCK_CONTEXT (context);
      
      old_cb_funcs->unref (old_cb_data);

      if (have_lock)
	LOCK_CONTEXT (context);
    }
}

/**
 * g_source_unref:
 * @source: a #GSource
 * 
 * Decreases the reference count of a source by one. If the
 * resulting reference count is zero the source and associated
 * memory will be destroyed. 
 **/
void
g_source_unref (GSource *source)
{
  g_return_if_fail (source != NULL);

  g_source_unref_internal (source, source->context, FALSE);
}

/**
 * g_main_context_find_source_by_id:
 * @context: a #GMainContext (if %NULL, the default context will be used)
 * @source_id: the source ID, as returned by g_source_get_id()
 * 
 * Finds a #GSource given a pair of context and ID
 * 
 * Return value: the #GSource if found, otherwise, %NULL
 **/
GSource *
g_main_context_find_source_by_id (GMainContext *context,
				  guint         source_id)
{
  GSource *source;
  
  g_return_val_if_fail (source_id > 0, NULL);

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);
  
  source = context->source_list;
  while (source)
    {
      if (!SOURCE_DESTROYED (source) &&
	  source->source_id == source_id)
	break;
      source = source->next;
    }

  UNLOCK_CONTEXT (context);

  return source;
}

/**
 * g_main_context_find_source_by_funcs_user_data:
 * @context: a #GMainContext (if %NULL, the default context will be used).
 * @funcs: the @source_funcs passed to g_source_new().
 * @user_data: the user data from the callback.
 * 
 * Finds a source with the given source functions and user data.  If
 * multiple sources exist with the same source function and user data,
 * the first one found will be returned.
 * 
 * Return value: the source, if one was found, otherwise %NULL
 **/
GSource *
g_main_context_find_source_by_funcs_user_data (GMainContext *context,
					       GSourceFuncs *funcs,
					       gpointer      user_data)
{
  GSource *source;
  
  g_return_val_if_fail (funcs != NULL, NULL);

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  source = context->source_list;
  while (source)
    {
      if (!SOURCE_DESTROYED (source) &&
	  source->source_funcs == funcs &&
	  source->callback_funcs)
	{
	  GSourceFunc callback;
	  gpointer callback_data;

	  source->callback_funcs->get (source->callback_data, source, &callback, &callback_data);
	  
	  if (callback_data == user_data)
	    break;
	}
      source = source->next;
    }

  UNLOCK_CONTEXT (context);

  return source;
}

/**
 * g_main_context_find_source_by_user_data:
 * @context: a #GMainContext
 * @user_data: the user_data for the callback.
 * 
 * Finds a source with the given user data for the callback.  If
 * multiple sources exist with the same user data, the first
 * one found will be returned.
 * 
 * Return value: the source, if one was found, otherwise %NULL
 **/
GSource *
g_main_context_find_source_by_user_data (GMainContext *context,
					 gpointer      user_data)
{
  GSource *source;
  
  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  source = context->source_list;
  while (source)
    {
      if (!SOURCE_DESTROYED (source) &&
	  source->callback_funcs)
	{
	  GSourceFunc callback;
	  gpointer callback_data = NULL;

	  source->callback_funcs->get (source->callback_data, source, &callback, &callback_data);

	  if (callback_data == user_data)
	    break;
	}
      source = source->next;
    }

  UNLOCK_CONTEXT (context);

  return source;
}

/**
 * g_source_remove:
 * @tag: the id of the source to remove.
 * 
 * Removes the source with the given id from the default main context. The id of
 * a #GSource is given by g_source_get_id(), or will be returned by the
 * functions g_source_attach(), g_idle_add(), g_idle_add_full(),
 * g_timeout_add(), g_timeout_add_full(), g_child_watch_add(),
 * g_child_watch_add_full(), g_io_add_watch(), and g_io_add_watch_full().
 *
 * See also g_source_destroy().
 *
 * Return value: %TRUE if the source was found and removed.
 **/
gboolean
g_source_remove (guint tag)
{
  GSource *source;
  
  g_return_val_if_fail (tag > 0, FALSE);

  source = g_main_context_find_source_by_id (NULL, tag);
  if (source)
    g_source_destroy (source);

  return source != NULL;
}

/**
 * g_source_remove_by_user_data:
 * @user_data: the user_data for the callback.
 * 
 * Removes a source from the default main loop context given the user
 * data for the callback. If multiple sources exist with the same user
 * data, only one will be destroyed.
 * 
 * Return value: %TRUE if a source was found and removed. 
 **/
gboolean
g_source_remove_by_user_data (gpointer user_data)
{
  GSource *source;
  
  source = g_main_context_find_source_by_user_data (NULL, user_data);
  if (source)
    {
      g_source_destroy (source);
      return TRUE;
    }
  else
    return FALSE;
}

/**
 * g_source_remove_by_funcs_user_data:
 * @funcs: The @source_funcs passed to g_source_new()
 * @user_data: the user data for the callback
 * 
 * Removes a source from the default main loop context given the
 * source functions and user data. If multiple sources exist with the
 * same source functions and user data, only one will be destroyed.
 * 
 * Return value: %TRUE if a source was found and removed. 
 **/
gboolean
g_source_remove_by_funcs_user_data (GSourceFuncs *funcs,
				    gpointer      user_data)
{
  GSource *source;

  g_return_val_if_fail (funcs != NULL, FALSE);

  source = g_main_context_find_source_by_funcs_user_data (NULL, funcs, user_data);
  if (source)
    {
      g_source_destroy (source);
      return TRUE;
    }
  else
    return FALSE;
}

/**
 * g_get_current_time:
 * @result: #GTimeVal structure in which to store current time.
 * 
 * Equivalent to the UNIX gettimeofday() function, but portable.
 **/
void
g_get_current_time (GTimeVal *result)
{
#ifndef G_OS_WIN32
  struct timeval r;

  g_return_if_fail (result != NULL);

  /*this is required on alpha, there the timeval structs are int's
    not longs and a cast only would fail horribly*/
  gettimeofday (&r, NULL);
  result->tv_sec = r.tv_sec;
  result->tv_usec = r.tv_usec;
#else
  /* Avoid calling time() except for the first time.
   * GetTickCount() should be pretty fast and low-level?
   * I could also use ftime() but it seems unnecessarily overheady.
   */
  static DWORD start_tick = 0;
  static time_t start_time;
  DWORD tick;

  g_return_if_fail (result != NULL);
 
  if (start_tick == 0)
    {
      start_tick = GetTickCount ();
      time (&start_time);
    }

  tick = GetTickCount ();

  result->tv_sec = (tick - start_tick) / 1000 + start_time;
  result->tv_usec = ((tick - start_tick) % 1000) * 1000;
#endif
}

/* Running the main loop */

static gint *
get_depth_pointer (void)
{
  static GStaticPrivate depth_private = G_STATIC_PRIVATE_INIT;
  gint *depth_pointer = g_static_private_get (&depth_private);
  if (!depth_pointer)
    {
      depth_pointer = g_new (gint, 1);
      *depth_pointer = 0;
      g_static_private_set (&depth_private, depth_pointer, g_free);
    }

  return depth_pointer;
}

/**
 * g_main_depth:
 * 
 * Return value: The main loop recursion level in the current thread
 *
 * Returns the depth of the stack of calls to
 * g_main_context_dispatch() on any #GMainContext in the current thread.
 *  That is, when called from the toplevel, it gives 0. When
 * called from within a callback from g_main_context_iteration()
 * (or g_main_loop_run(), etc.) it returns 1. When called from within 
 * a callback to a recursive call to g_main_context_iterate(),
 * it returns 2. And so forth.
 *
 * This function is useful in a situation like the following:
 * Imagine an extremely simple "garbage collected" system.
 *
 * <example>
 * static GList *free_list;
 *
 * gpointer
 * allocate_memory (gsize size)
 * { 
 *   gpointer result = g_malloc (size);
 *   free_list = g_list_prepend (free_list, result);
 *   return result;
 * }
 *
 * void
 * free_allocated_memory (void)
 * {
 *   GList *l;
 *   for (l = free_list; l; l = l->next);
 *     g_free (l->data);
 *   g_list_free (free_list);
 *   free_list = NULL;
 *  }
 *
 * [...]
 *
 * while (TRUE); 
 *  {
 *    g_main_context_iteration (NULL, TRUE);
 *    free_allocated_memory();
 *   }
 * </example>
 *
 * This works from an application, however, if you want to do the same
 * thing from a library, it gets more difficult, since you no longer
 * control the main loop. You might think you can simply use an idle
 * function to make the call to free_allocated_memory(), but that
 * doesn't work, since the idle function could be called from a
 * recursive callback. This can be fixed by using g_main_depth()
 *
 * <example>
 * gpointer
 * allocate_memory (gsize size)
 * { 
 *   FreeListBlock *block = g_new (FreeListBlock, 1);\
 *   block->mem = g_malloc (size);
 *   block->depth = g_main_depth ();   
 *   free_list = g_list_prepend (free_list, block);
 *   return block->mem;
 * }
 *
 * void
 * free_allocated_memory (void)
 * {
 *   GList *l;
 *
 *   int depth = g_main_depth ();
 *   for (l = free_list; l; );
 *     {
 *       GList *next = l->next;
 *       FreeListBlock *block = l->data;
 *       if (block->depth > depth)
 *         {
 *           g_free (block->mem);
 *           g_free (block);
 *           free_list = g_list_delete_link (free_list, l);
 *         }
 *           
 *       l = next;
 *     }
 *   }
 * </example>
 *
 * There is a temptation to use g_main_depth() to solve
 * problems with reentrancy. For instance, while waiting for data
 * to be received from the network in response to a menu item,
 * the menu item might be selected again. It might seem that
 * one could make the menu item's callback return immediately
 * and do nothing if g_main_depth() returns a value greater than 1.
 * However, this should be avoided since the user then sees selecting
 * the menu item do nothing. Furthermore, you'll find yourself adding
 * these checks all over your code, since there are doubtless many,
 * many things that the user could do. Instead, you can use the
 * following techniques:
 *
 * <orderedlist>
 *  <listitem>
 *   <para>
 *     Use gtk_widget_set_sensitive() or modal dialogs to prevent
 *     the user from interacting with elements while the main
 *     loop is recursing.
 *   </para>
 *  </listitem>
 *  <listitem>
 *   <para>
 *     Avoid main loop recursion in situations where you can't handle
 *     arbitrary  callbacks. Instead, structure your code so that you
 *     simply return to the main loop and then get called again when
 *     there is more work to do.
 *   </para>
 *  </listitem>
 * </orderedlist>
 **/
int
g_main_depth (void)
{
  gint *depth = get_depth_pointer ();
  return *depth;
}

/* Temporarily remove all this source's file descriptors from the
 * poll(), so that if data comes available for one of the file descriptors
 * we don't continually spin in the poll()
 */
/* HOLDS: source->context's lock */
static void
block_source (GSource *source)
{
  GSList *tmp_list;

  g_return_if_fail (!SOURCE_BLOCKED (source));

  tmp_list = source->poll_fds;
  while (tmp_list)
    {
      g_main_context_remove_poll_unlocked (source->context, tmp_list->data);
      tmp_list = tmp_list->next;
    }
}

/* HOLDS: source->context's lock */
static void
unblock_source (GSource *source)
{
  GSList *tmp_list;
  
  g_return_if_fail (!SOURCE_BLOCKED (source)); /* Source already unblocked */
  g_return_if_fail (!SOURCE_DESTROYED (source));
  
  tmp_list = source->poll_fds;
  while (tmp_list)
    {
      g_main_context_add_poll_unlocked (source->context, source->priority, tmp_list->data);
      tmp_list = tmp_list->next;
    }
}

/* HOLDS: context's lock */
static void
g_main_dispatch (GMainContext *context)
{
  gint *depth = get_depth_pointer ();
  guint i;

  for (i = 0; i < context->pending_dispatches->len; i++)
    {
      GSource *source = context->pending_dispatches->pdata[i];

      context->pending_dispatches->pdata[i] = NULL;
      g_assert (source);

      source->flags &= ~G_SOURCE_READY;

      if (!SOURCE_DESTROYED (source))
	{
	  gboolean was_in_call;
	  gpointer user_data = NULL;
	  GSourceFunc callback = NULL;
	  GSourceCallbackFuncs *cb_funcs;
	  gpointer cb_data;
	  gboolean need_destroy;

	  gboolean (*dispatch) (GSource *,
				GSourceFunc,
				gpointer);

	  dispatch = source->source_funcs->dispatch;
	  cb_funcs = source->callback_funcs;
	  cb_data = source->callback_data;

	  if (cb_funcs)
	    cb_funcs->ref (cb_data);
	  
	  if ((source->flags & G_SOURCE_CAN_RECURSE) == 0)
	    block_source (source);
	  
	  was_in_call = source->flags & G_HOOK_FLAG_IN_CALL;
	  source->flags |= G_HOOK_FLAG_IN_CALL;

	  if (cb_funcs)
	    cb_funcs->get (cb_data, source, &callback, &user_data);

	  UNLOCK_CONTEXT (context);

	  (*depth)++;
	  need_destroy = ! dispatch (source,
				     callback,
				     user_data);
	  (*depth)--;
	  
 	  LOCK_CONTEXT (context);

	  if (cb_funcs)
	    cb_funcs->unref (cb_data);

	 if (!was_in_call)
	    source->flags &= ~G_HOOK_FLAG_IN_CALL;

	  if ((source->flags & G_SOURCE_CAN_RECURSE) == 0 &&
	      !SOURCE_DESTROYED (source))
	    unblock_source (source);
	  
	  /* Note: this depends on the fact that we can't switch
	   * sources from one main context to another
	   */
	  if (need_destroy && !SOURCE_DESTROYED (source))
	    {
	      g_assert (source->context == context);
	      g_source_destroy_internal (source, context, TRUE);
	    }
	}
      
      SOURCE_UNREF (source, context);
    }

  g_ptr_array_set_size (context->pending_dispatches, 0);
}

/* Holds context's lock */
static inline GSource *
next_valid_source (GMainContext *context,
		   GSource      *source)
{
  GSource *new_source = source ? source->next : context->source_list;

  while (new_source)
    {
      if (!SOURCE_DESTROYED (new_source))
	{
	  new_source->ref_count++;
	  break;
	}
      
      new_source = new_source->next;
    }

  if (source)
    SOURCE_UNREF (source, context);
	  
  return new_source;
}

/**
 * g_main_context_acquire:
 * @context: a #GMainContext
 * 
 * Tries to become the owner of the specified context.
 * If some other context is the owner of the context,
 * returns %FALSE immediately. Ownership is properly
 * recursive: the owner can require ownership again
 * and will release ownership when g_main_context_release()
 * is called as many times as g_main_context_acquire().
 *
 * You must be the owner of a context before you
 * can call g_main_context_prepare(), g_main_context_query(),
 * g_main_context_check(), g_main_context_dispatch().
 * 
 * Return value: %TRUE if the operation succeeded, and
 *   this thread is now the owner of @context.
 **/
gboolean 
g_main_context_acquire (GMainContext *context)
{
#ifdef G_THREADS_ENABLED
  gboolean result = FALSE;
  GThread *self = G_THREAD_SELF;

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  if (!context->owner)
    {
      context->owner = self;
      g_assert (context->owner_count == 0);
    }

  if (context->owner == self)
    {
      context->owner_count++;
      result = TRUE;
    }

  UNLOCK_CONTEXT (context); 
  
  return result;
#else /* !G_THREADS_ENABLED */
  return TRUE;
#endif /* G_THREADS_ENABLED */
}

/**
 * g_main_context_release:
 * @context: a #GMainContext
 * 
 * Releases ownership of a context previously acquired by this thread
 * with g_main_context_acquire(). If the context was acquired multiple
 * times, the only release ownership when g_main_context_release()
 * is called as many times as it was acquired.
 **/
void
g_main_context_release (GMainContext *context)
{
#ifdef G_THREADS_ENABLED
  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  context->owner_count--;
  if (context->owner_count == 0)
    {
      context->owner = NULL;

      if (context->waiters)
	{
	  GMainWaiter *waiter = context->waiters->data;
	  gboolean loop_internal_waiter =
	    (waiter->mutex == g_static_mutex_get_mutex (&context->mutex));
	  context->waiters = g_slist_delete_link (context->waiters,
						  context->waiters);
	  if (!loop_internal_waiter)
	    g_mutex_lock (waiter->mutex);
	  
	  g_cond_signal (waiter->cond);
	  
	  if (!loop_internal_waiter)
	    g_mutex_unlock (waiter->mutex);
	}
    }

  UNLOCK_CONTEXT (context); 
#endif /* G_THREADS_ENABLED */
}

/**
 * g_main_context_wait:
 * @context: a #GMainContext
 * @cond: a condition variable
 * @mutex: a mutex, currently held
 * 
 * Tries to become the owner of the specified context,
 * as with g_main_context_acquire(). But if another thread
 * is the owner, atomically drop @mutex and wait on @cond until 
 * that owner releases ownership or until @cond is signaled, then
 * try again (once) to become the owner.
 * 
 * Return value: %TRUE if the operation succeeded, and
 *   this thread is now the owner of @context.
 **/
gboolean
g_main_context_wait (GMainContext *context,
		     GCond        *cond,
		     GMutex       *mutex)
{
#ifdef G_THREADS_ENABLED
  gboolean result = FALSE;
  GThread *self = G_THREAD_SELF;
  gboolean loop_internal_waiter;
  
  if (context == NULL)
    context = g_main_context_default ();

  loop_internal_waiter = (mutex == g_static_mutex_get_mutex (&context->mutex));
  
  if (!loop_internal_waiter)
    LOCK_CONTEXT (context);

  if (context->owner && context->owner != self)
    {
      GMainWaiter waiter;

      waiter.cond = cond;
      waiter.mutex = mutex;

      context->waiters = g_slist_append (context->waiters, &waiter);
      
      if (!loop_internal_waiter)
	UNLOCK_CONTEXT (context);
      g_cond_wait (cond, mutex);
      if (!loop_internal_waiter)      
	LOCK_CONTEXT (context);

      context->waiters = g_slist_remove (context->waiters, &waiter);
    }

  if (!context->owner)
    {
      context->owner = self;
      g_assert (context->owner_count == 0);
    }

  if (context->owner == self)
    {
      context->owner_count++;
      result = TRUE;
    }

  if (!loop_internal_waiter)
    UNLOCK_CONTEXT (context); 
  
  return result;
#else /* !G_THREADS_ENABLED */
  return TRUE;
#endif /* G_THREADS_ENABLED */
}

/**
 * g_main_context_prepare:
 * @context: a #GMainContext
 * @priority: location to store priority of highest priority
 *            source already ready.
 * 
 * Prepares to poll sources within a main loop. The resulting information
 * for polling is determined by calling g_main_context_query ().
 * 
 * Return value: %TRUE if some source is ready to be dispatched
 *               prior to polling.
 **/
gboolean
g_main_context_prepare (GMainContext *context,
			gint         *priority)
{
  gint i;
  gint n_ready = 0;
  gint current_priority = G_MAXINT;
  GSource *source;

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  context->time_is_current = FALSE;

  if (context->in_check_or_prepare)
    {
      g_warning ("g_main_context_prepare() called recursively from within a source's check() or "
		 "prepare() member.");
      UNLOCK_CONTEXT (context);
      return FALSE;
    }

#ifdef G_THREADS_ENABLED
  if (context->poll_waiting)
    {
      g_warning("g_main_context_prepare(): main loop already active in another thread");
      UNLOCK_CONTEXT (context);
      return FALSE;
    }
  
  context->poll_waiting = TRUE;
#endif /* G_THREADS_ENABLED */

#if 0
  /* If recursing, finish up current dispatch, before starting over */
  if (context->pending_dispatches)
    {
      if (dispatch)
	g_main_dispatch (context, &current_time);
      
      UNLOCK_CONTEXT (context);
      return TRUE;
    }
#endif

  /* If recursing, clear list of pending dispatches */

  for (i = 0; i < context->pending_dispatches->len; i++)
    {
      if (context->pending_dispatches->pdata[i])
	SOURCE_UNREF ((GSource *)context->pending_dispatches->pdata[i], context);
    }
  g_ptr_array_set_size (context->pending_dispatches, 0);
  
  /* Prepare all sources */

  context->timeout = -1;
  
  source = next_valid_source (context, NULL);
  while (source)
    {
      gint source_timeout = -1;

      if ((n_ready > 0) && (source->priority > current_priority))
	{
	  SOURCE_UNREF (source, context);
	  break;
	}
      if (SOURCE_BLOCKED (source))
	goto next;

      if (!(source->flags & G_SOURCE_READY))
	{
	  gboolean result;
	  gboolean (*prepare)  (GSource  *source, 
				gint     *timeout);

	  prepare = source->source_funcs->prepare;
	  context->in_check_or_prepare++;
	  UNLOCK_CONTEXT (context);

	  result = (*prepare) (source, &source_timeout);

	  LOCK_CONTEXT (context);
	  context->in_check_or_prepare--;

	  if (result)
	    source->flags |= G_SOURCE_READY;
	}

      if (source->flags & G_SOURCE_READY)
	{
	  n_ready++;
	  current_priority = source->priority;
	  context->timeout = 0;
	}
      
      if (source_timeout >= 0)
	{
	  if (context->timeout < 0)
	    context->timeout = source_timeout;
	  else
	    context->timeout = MIN (context->timeout, source_timeout);
	}

    next:
      source = next_valid_source (context, source);
    }

  UNLOCK_CONTEXT (context);
  
  if (priority)
    *priority = current_priority;
  
  return (n_ready > 0);
}

/**
 * g_main_context_query:
 * @context: a #GMainContext
 * @max_priority: maximum priority source to check
 * @timeout_: location to store timeout to be used in polling
 * @fds: location to store #GPollFD records that need to be polled.
 * @n_fds: length of @fds.
 * 
 * Determines information necessary to poll this main loop.
 * 
 * Return value: the number of records actually stored in @fds,
 *   or, if more than @n_fds records need to be stored, the number
 *   of records that need to be stored.
 **/
gint
g_main_context_query (GMainContext *context,
		      gint          max_priority,
		      gint         *timeout,
		      GPollFD      *fds,
		      gint          n_fds)
{
  gint n_poll;
  GPollRec *pollrec;
  
  LOCK_CONTEXT (context);

  pollrec = context->poll_records;
  n_poll = 0;
  while (pollrec && max_priority >= pollrec->priority)
    {
      if (pollrec->fd->events)
	{
	  if (n_poll < n_fds)
	    {
	      fds[n_poll].fd = pollrec->fd->fd;
	      /* In direct contradiction to the Unix98 spec, IRIX runs into
	       * difficulty if you pass in POLLERR, POLLHUP or POLLNVAL
	       * flags in the events field of the pollfd while it should
	       * just ignoring them. So we mask them out here.
	       */
	      fds[n_poll].events = pollrec->fd->events & ~(G_IO_ERR|G_IO_HUP|G_IO_NVAL);
	      fds[n_poll].revents = 0;
	    }
	  n_poll++;
	}
      
      pollrec = pollrec->next;
    }

#ifdef G_THREADS_ENABLED
  context->poll_changed = FALSE;
#endif
  
  if (timeout)
    {
      *timeout = context->timeout;
      if (*timeout != 0)
	context->time_is_current = FALSE;
    }
  
  UNLOCK_CONTEXT (context);

  return n_poll;
}

/**
 * g_main_context_check:
 * @context: a #GMainContext
 * @max_priority: the maximum numerical priority of sources to check
 * @fds: array of #GPollFD's that was passed to the last call to
 *       g_main_context_query()
 * @n_fds: return value of g_main_context_query()
 * 
 * Passes the results of polling back to the main loop.
 * 
 * Return value: %TRUE if some sources are ready to be dispatched.
 **/
gboolean
g_main_context_check (GMainContext *context,
		      gint          max_priority,
		      GPollFD      *fds,
		      gint          n_fds)
{
  GSource *source;
  GPollRec *pollrec;
  gint n_ready = 0;
  gint i;
  
  LOCK_CONTEXT (context);

  if (context->in_check_or_prepare)
    {
      g_warning ("g_main_context_check() called recursively from within a source's check() or "
		 "prepare() member.");
      UNLOCK_CONTEXT (context);
      return FALSE;
    }
  
#ifdef G_THREADS_ENABLED
  if (!context->poll_waiting)
    {
#ifndef G_OS_WIN32
      gchar a;
      read (context->wake_up_pipe[0], &a, 1);
#endif
    }
  else
    context->poll_waiting = FALSE;

  /* If the set of poll file descriptors changed, bail out
   * and let the main loop rerun
   */
  if (context->poll_changed)
    {
      UNLOCK_CONTEXT (context);
      return 0;
    }
#endif /* G_THREADS_ENABLED */
  
  pollrec = context->poll_records;
  i = 0;
  while (i < n_fds)
    {
      if (pollrec->fd->events)
	{
	  pollrec->fd->revents = fds[i].revents;
	  i++;
	}
      pollrec = pollrec->next;
    }

  source = next_valid_source (context, NULL);
  while (source)
    {
      if ((n_ready > 0) && (source->priority > max_priority))
	{
	  SOURCE_UNREF (source, context);
	  break;
	}
      if (SOURCE_BLOCKED (source))
	goto next;

      if (!(source->flags & G_SOURCE_READY))
	{
	  gboolean result;
	  gboolean (*check) (GSource  *source);

	  check = source->source_funcs->check;
	  
	  context->in_check_or_prepare++;
	  UNLOCK_CONTEXT (context);
	  
	  result = (*check) (source);
	  
	  LOCK_CONTEXT (context);
	  context->in_check_or_prepare--;
	  
	  if (result)
	    source->flags |= G_SOURCE_READY;
	}

      if (source->flags & G_SOURCE_READY)
	{
	  source->ref_count++;
	  g_ptr_array_add (context->pending_dispatches, source);

	  n_ready++;

          /* never dispatch sources with less priority than the first
           * one we choose to dispatch
           */
          max_priority = source->priority;
	}

    next:
      source = next_valid_source (context, source);
    }

  UNLOCK_CONTEXT (context);

  return n_ready > 0;
}

/**
 * g_main_context_dispatch:
 * @context: a #GMainContext
 * 
 * Dispatches all pending sources.
 **/
void
g_main_context_dispatch (GMainContext *context)
{
  LOCK_CONTEXT (context);

  if (context->pending_dispatches->len > 0)
    {
      g_main_dispatch (context);
    }

  UNLOCK_CONTEXT (context);
}

/* HOLDS context lock */
static gboolean
g_main_context_iterate (GMainContext *context,
			gboolean      block,
			gboolean      dispatch,
			GThread      *self)
{
  gint max_priority;
  gint timeout;
  gboolean some_ready;
  gint nfds, allocated_nfds;
  GPollFD *fds = NULL;
  
  UNLOCK_CONTEXT (context);

#ifdef G_THREADS_ENABLED
  if (!g_main_context_acquire (context))
    {
      gboolean got_ownership;
      
      g_return_val_if_fail (g_thread_supported (), FALSE);

      if (!block)
	return FALSE;

      LOCK_CONTEXT (context);
      
      if (!context->cond)
	context->cond = g_cond_new ();
          
      got_ownership = g_main_context_wait (context,
					   context->cond,
					   g_static_mutex_get_mutex (&context->mutex));

      if (!got_ownership)
	{
	  UNLOCK_CONTEXT (context);
	  return FALSE;
	}
    }
  else
    LOCK_CONTEXT (context);
#endif /* G_THREADS_ENABLED */
  
  if (!context->cached_poll_array)
    {
      context->cached_poll_array_size = context->n_poll_records;
      context->cached_poll_array = g_new (GPollFD, context->n_poll_records);
    }

  allocated_nfds = context->cached_poll_array_size;
  fds = context->cached_poll_array;
  
  UNLOCK_CONTEXT (context);

  g_main_context_prepare (context, &max_priority); 
  
  while ((nfds = g_main_context_query (context, max_priority, &timeout, fds, 
				       allocated_nfds)) > allocated_nfds)
    {
      LOCK_CONTEXT (context);
      g_free (fds);
      context->cached_poll_array_size = allocated_nfds = nfds;
      context->cached_poll_array = fds = g_new (GPollFD, nfds);
      UNLOCK_CONTEXT (context);
    }

  if (!block)
    timeout = 0;
  
  g_main_context_poll (context, timeout, max_priority, fds, nfds);
  
  some_ready = g_main_context_check (context, max_priority, fds, nfds);
  
  if (dispatch)
    g_main_context_dispatch (context);
  
#ifdef G_THREADS_ENABLED
  g_main_context_release (context);
#endif /* G_THREADS_ENABLED */    

  LOCK_CONTEXT (context);

  return some_ready;
}

/**
 * g_main_context_pending:
 * @context: a #GMainContext (if %NULL, the default context will be used)
 *
 * Checks if any sources have pending events for the given context.
 * 
 * Return value: %TRUE if events are pending.
 **/
gboolean 
g_main_context_pending (GMainContext *context)
{
  gboolean retval;

  if (!context)
    context = g_main_context_default();

  LOCK_CONTEXT (context);
  retval = g_main_context_iterate (context, FALSE, FALSE, G_THREAD_SELF);
  UNLOCK_CONTEXT (context);
  
  return retval;
}

/**
 * g_main_context_iteration:
 * @context: a #GMainContext (if %NULL, the default context will be used) 
 * @may_block: whether the call may block.
 * 
 * Runs a single iteration for the given main loop. This involves
 * checking to see if any event sources are ready to be processed,
 * then if no events sources are ready and @may_block is %TRUE, waiting
 * for a source to become ready, then dispatching the highest priority
 * events sources that are ready. Note that even when @may_block is %TRUE,
 * it is still possible for g_main_context_iteration() to return
 * %FALSE, since the the wait may be interrupted for other
 * reasons than an event source becoming ready.
 * 
 * Return value: %TRUE if events were dispatched.
 **/
gboolean
g_main_context_iteration (GMainContext *context, gboolean may_block)
{
  gboolean retval;

  if (!context)
    context = g_main_context_default();
  
  LOCK_CONTEXT (context);
  retval = g_main_context_iterate (context, may_block, TRUE, G_THREAD_SELF);
  UNLOCK_CONTEXT (context);
  
  return retval;
}

/**
 * g_main_loop_new:
 * @context: a #GMainContext  (if %NULL, the default context will be used).
 * @is_running: set to %TRUE to indicate that the loop is running. This
 * is not very important since calling g_main_loop_run() will set this to
 * %TRUE anyway.
 * 
 * Creates a new #GMainLoop structure.
 * 
 * Return value: a new #GMainLoop.
 **/
GMainLoop *
g_main_loop_new (GMainContext *context,
		 gboolean      is_running)
{
  GMainLoop *loop;
  
  if (!context)
    context = g_main_context_default();
  
  g_main_context_ref (context);

  loop = g_new0 (GMainLoop, 1);
  loop->context = context;
  loop->is_running = is_running != FALSE;
  loop->ref_count = 1;
  
  return loop;
}

/**
 * g_main_loop_ref:
 * @loop: a #GMainLoop
 * 
 * Increases the reference count on a #GMainLoop object by one.
 * 
 * Return value: @loop
 **/
GMainLoop *
g_main_loop_ref (GMainLoop *loop)
{
  g_return_val_if_fail (loop != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&loop->ref_count) > 0, NULL);

  g_atomic_int_inc (&loop->ref_count);

  return loop;
}

/**
 * g_main_loop_unref:
 * @loop: a #GMainLoop
 * 
 * Decreases the reference count on a #GMainLoop object by one. If
 * the result is zero, free the loop and free all associated memory.
 **/
void
g_main_loop_unref (GMainLoop *loop)
{
  g_return_if_fail (loop != NULL);
  g_return_if_fail (g_atomic_int_get (&loop->ref_count) > 0);

  if (!g_atomic_int_dec_and_test (&loop->ref_count))
    return;

  g_main_context_unref (loop->context);
  g_free (loop);
}

/**
 * g_main_loop_run:
 * @loop: a #GMainLoop
 * 
 * Runs a main loop until g_main_loop_quit() is called on the loop.
 * If this is called for the thread of the loop's #GMainContext,
 * it will process events from the loop, otherwise it will
 * simply wait.
 **/
void 
g_main_loop_run (GMainLoop *loop)
{
  GThread *self = G_THREAD_SELF;

  g_return_if_fail (loop != NULL);
  g_return_if_fail (g_atomic_int_get (&loop->ref_count) > 0);

#ifdef G_THREADS_ENABLED
  if (!g_main_context_acquire (loop->context))
    {
      gboolean got_ownership = FALSE;
      
      /* Another thread owns this context */
      if (!g_thread_supported ())
	{
	  g_warning ("g_main_loop_run() was called from second thread but "
		     "g_thread_init() was never called.");
	  return;
	}
      
      LOCK_CONTEXT (loop->context);

      g_atomic_int_inc (&loop->ref_count);

      if (!loop->is_running)
	loop->is_running = TRUE;

      if (!loop->context->cond)
	loop->context->cond = g_cond_new ();
          
      while (loop->is_running && !got_ownership)
	got_ownership = g_main_context_wait (loop->context,
					     loop->context->cond,
					     g_static_mutex_get_mutex (&loop->context->mutex));
      
      if (!loop->is_running)
	{
	  UNLOCK_CONTEXT (loop->context);
	  if (got_ownership)
	    g_main_context_release (loop->context);
	  g_main_loop_unref (loop);
	  return;
	}

      g_assert (got_ownership);
    }
  else
    LOCK_CONTEXT (loop->context);
#endif /* G_THREADS_ENABLED */ 

  if (loop->context->in_check_or_prepare)
    {
      g_warning ("g_main_loop_run(): called recursively from within a source's "
		 "check() or prepare() member, iteration not possible.");
      return;
    }

  g_atomic_int_inc (&loop->ref_count);
  loop->is_running = TRUE;
  while (loop->is_running)
    g_main_context_iterate (loop->context, TRUE, TRUE, self);

  UNLOCK_CONTEXT (loop->context);
  
#ifdef G_THREADS_ENABLED
  g_main_context_release (loop->context);
#endif /* G_THREADS_ENABLED */    
  
  g_main_loop_unref (loop);
}

/**
 * g_main_loop_quit:
 * @loop: a #GMainLoop
 * 
 * Stops a #GMainLoop from running. Any calls to g_main_loop_run()
 * for the loop will return.
 **/
void 
g_main_loop_quit (GMainLoop *loop)
{
  g_return_if_fail (loop != NULL);
  g_return_if_fail (g_atomic_int_get (&loop->ref_count) > 0);

  LOCK_CONTEXT (loop->context);
  loop->is_running = FALSE;
  g_main_context_wakeup_unlocked (loop->context);

#ifdef G_THREADS_ENABLED
  if (loop->context->cond)
    g_cond_broadcast (loop->context->cond);
#endif /* G_THREADS_ENABLED */

  UNLOCK_CONTEXT (loop->context);
}

/**
 * g_main_loop_is_running:
 * @loop: a #GMainLoop.
 * 
 * Checks to see if the main loop is currently being run via g_main_loop_run().
 * 
 * Return value: %TRUE if the mainloop is currently being run.
 **/
gboolean
g_main_loop_is_running (GMainLoop *loop)
{
  g_return_val_if_fail (loop != NULL, FALSE);
  g_return_val_if_fail (g_atomic_int_get (&loop->ref_count) > 0, FALSE);

  return loop->is_running;
}

/**
 * g_main_loop_get_context:
 * @loop: a #GMainLoop.
 * 
 * Returns the #GMainContext of @loop.
 * 
 * Return value: the #GMainContext of @loop
 **/
GMainContext *
g_main_loop_get_context (GMainLoop *loop)
{
  g_return_val_if_fail (loop != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&loop->ref_count) > 0, NULL);
 
  return loop->context;
}

/* HOLDS: context's lock */
static void
g_main_context_poll (GMainContext *context,
		     gint          timeout,
		     gint          priority,
		     GPollFD      *fds,
		     gint          n_fds)
{
#ifdef  G_MAIN_POLL_DEBUG
  GTimer *poll_timer;
  GPollRec *pollrec;
  gint i;
#endif

  GPollFunc poll_func;

  if (n_fds || timeout != 0)
    {
#ifdef	G_MAIN_POLL_DEBUG
      g_print ("g_main_poll(%d) timeout: %d\n", n_fds, timeout);
      poll_timer = g_timer_new ();
#endif

      LOCK_CONTEXT (context);

      poll_func = context->poll_func;
      
      UNLOCK_CONTEXT (context);
      if ((*poll_func) (fds, n_fds, timeout) < 0 && errno != EINTR)
	{
#ifndef G_OS_WIN32
	  g_warning ("poll(2) failed due to: %s.",
		     g_strerror (errno));
#else
	  /* If g_poll () returns -1, it has already called g_warning() */
#endif
	}
      
#ifdef	G_MAIN_POLL_DEBUG
      LOCK_CONTEXT (context);

      g_print ("g_main_poll(%d) timeout: %d - elapsed %12.10f seconds",
	       n_fds,
	       timeout,
	       g_timer_elapsed (poll_timer, NULL));
      g_timer_destroy (poll_timer);
      pollrec = context->poll_records;
      i = 0;
      while (i < n_fds)
	{
	  if (pollrec->fd->events)
	    {
	      if (fds[i].revents)
		{
		  g_print (" [%d:", fds[i].fd);
		  if (fds[i].revents & G_IO_IN)
		    g_print ("i");
		  if (fds[i].revents & G_IO_OUT)
		    g_print ("o");
		  if (fds[i].revents & G_IO_PRI)
		    g_print ("p");
		  if (fds[i].revents & G_IO_ERR)
		    g_print ("e");
		  if (fds[i].revents & G_IO_HUP)
		    g_print ("h");
		  if (fds[i].revents & G_IO_NVAL)
		    g_print ("n");
		  g_print ("]");
		}
	      i++;
	    }
	  pollrec = pollrec->next;
	}
      g_print ("\n");
      
      UNLOCK_CONTEXT (context);
#endif
    } /* if (n_fds || timeout != 0) */
}

/**
 * g_main_context_add_poll:
 * @context: a #GMainContext (or %NULL for the default context)
 * @fd: a #GPollFD structure holding information about a file
 *      descriptor to watch.
 * @priority: the priority for this file descriptor which should be
 *      the same as the priority used for g_source_attach() to ensure that the
 *      file descriptor is polled whenever the results may be needed.
 * 
 * Adds a file descriptor to the set of file descriptors polled for
 * this context. This will very seldomly be used directly. Instead
 * a typical event source will use g_source_add_poll() instead.
 **/
void
g_main_context_add_poll (GMainContext *context,
			 GPollFD      *fd,
			 gint          priority)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);
  g_return_if_fail (fd);

  LOCK_CONTEXT (context);
  g_main_context_add_poll_unlocked (context, priority, fd);
  UNLOCK_CONTEXT (context);
}

/* HOLDS: main_loop_lock */
static void 
g_main_context_add_poll_unlocked (GMainContext *context,
				  gint          priority,
				  GPollFD      *fd)
{
  GPollRec *lastrec, *pollrec, *newrec;

  if (!context->poll_chunk)
    context->poll_chunk = g_mem_chunk_create (GPollRec, 32, G_ALLOC_ONLY);

  if (context->poll_free_list)
    {
      newrec = context->poll_free_list;
      context->poll_free_list = newrec->next;
    }
  else
    newrec = g_chunk_new (GPollRec, context->poll_chunk);

  /* This file descriptor may be checked before we ever poll */
  fd->revents = 0;
  newrec->fd = fd;
  newrec->priority = priority;

  lastrec = NULL;
  pollrec = context->poll_records;
  while (pollrec && priority >= pollrec->priority)
    {
      lastrec = pollrec;
      pollrec = pollrec->next;
    }
  
  if (lastrec)
    lastrec->next = newrec;
  else
    context->poll_records = newrec;

  newrec->next = pollrec;

  context->n_poll_records++;

#ifdef G_THREADS_ENABLED
  context->poll_changed = TRUE;

  /* Now wake up the main loop if it is waiting in the poll() */
  g_main_context_wakeup_unlocked (context);
#endif
}

/**
 * g_main_context_remove_poll:
 * @context:a #GMainContext 
 * @fd: a #GPollFD descriptor previously added with g_main_context_add_poll()
 * 
 * Removes file descriptor from the set of file descriptors to be
 * polled for a particular context.
 **/
void
g_main_context_remove_poll (GMainContext *context,
			    GPollFD      *fd)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);
  g_return_if_fail (fd);

  LOCK_CONTEXT (context);
  g_main_context_remove_poll_unlocked (context, fd);
  UNLOCK_CONTEXT (context);
}

static void
g_main_context_remove_poll_unlocked (GMainContext *context,
				     GPollFD      *fd)
{
  GPollRec *pollrec, *lastrec;

  lastrec = NULL;
  pollrec = context->poll_records;

  while (pollrec)
    {
      if (pollrec->fd == fd)
	{
	  if (lastrec != NULL)
	    lastrec->next = pollrec->next;
	  else
	    context->poll_records = pollrec->next;

#ifdef ENABLE_GC_FRIENDLY
	  pollrec->fd = NULL;  
#endif /* ENABLE_GC_FRIENDLY */

	  pollrec->next = context->poll_free_list;
	  context->poll_free_list = pollrec;

	  context->n_poll_records--;
	  break;
	}
      lastrec = pollrec;
      pollrec = pollrec->next;
    }

#ifdef G_THREADS_ENABLED
  context->poll_changed = TRUE;
  
  /* Now wake up the main loop if it is waiting in the poll() */
  g_main_context_wakeup_unlocked (context);
#endif
}

/**
 * g_source_get_current_time:
 * @source:  a #GSource
 * @timeval: #GTimeVal structure in which to store current time.
 * 
 * Gets the "current time" to be used when checking 
 * this source. The advantage of calling this function over
 * calling g_get_current_time() directly is that when 
 * checking multiple sources, GLib can cache a single value
 * instead of having to repeatedly get the system time.
 **/
void
g_source_get_current_time (GSource  *source,
			   GTimeVal *timeval)
{
  GMainContext *context;
  
  g_return_if_fail (source->context != NULL);
 
  context = source->context;

  LOCK_CONTEXT (context);

  if (!context->time_is_current)
    {
      g_get_current_time (&context->current_time);
      context->time_is_current = TRUE;
    }
  
  *timeval = context->current_time;
  
  UNLOCK_CONTEXT (context);
}

/**
 * g_main_context_set_poll_func:
 * @context: a #GMainContext
 * @func: the function to call to poll all file descriptors
 * 
 * Sets the function to use to handle polling of file descriptors. It
 * will be used instead of the poll() system call 
 * (or GLib's replacement function, which is used where 
 * poll() isn't available).
 *
 * This function could possibly be used to integrate the GLib event
 * loop with an external event loop.
 **/
void
g_main_context_set_poll_func (GMainContext *context,
			      GPollFunc     func)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);

  LOCK_CONTEXT (context);
  
  if (func)
    context->poll_func = func;
  else
    {
#ifdef HAVE_POLL
      context->poll_func = (GPollFunc) poll;
#else
      context->poll_func = (GPollFunc) g_poll;
#endif
    }

  UNLOCK_CONTEXT (context);
}

/**
 * g_main_context_get_poll_func:
 * @context: a #GMainContext
 * 
 * Gets the poll function set by g_main_context_set_poll_func().
 * 
 * Return value: the poll function
 **/
GPollFunc
g_main_context_get_poll_func (GMainContext *context)
{
  GPollFunc result;
  
  if (!context)
    context = g_main_context_default ();
  
  g_return_val_if_fail (g_atomic_int_get (&context->ref_count) > 0, NULL);

  LOCK_CONTEXT (context);
  result = context->poll_func;
  UNLOCK_CONTEXT (context);

  return result;
}

/* HOLDS: context's lock */
/* Wake the main loop up from a poll() */
static void
g_main_context_wakeup_unlocked (GMainContext *context)
{
#ifdef G_THREADS_ENABLED
  if (g_thread_supported() && context->poll_waiting)
    {
      context->poll_waiting = FALSE;
#ifndef G_OS_WIN32
      write (context->wake_up_pipe[1], "A", 1);
#else
      ReleaseSemaphore (context->wake_up_semaphore, 1, NULL);
#endif
    }
#endif
}

/**
 * g_main_context_wakeup:
 * @context: a #GMainContext
 * 
 * If @context is currently waiting in a poll(), interrupt
 * the poll(), and continue the iteration process.
 **/
void
g_main_context_wakeup (GMainContext *context)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);

  LOCK_CONTEXT (context);
  g_main_context_wakeup_unlocked (context);
  UNLOCK_CONTEXT (context);
}

/* Timeouts */

static void
g_timeout_set_expiration (GTimeoutSource *timeout_source,
			  GTimeVal       *current_time)
{
  guint seconds = timeout_source->interval / 1000;
  guint msecs = timeout_source->interval - seconds * 1000;

  timeout_source->expiration.tv_sec = current_time->tv_sec + seconds;
  timeout_source->expiration.tv_usec = current_time->tv_usec + msecs * 1000;
  if (timeout_source->expiration.tv_usec >= 1000000)
    {
      timeout_source->expiration.tv_usec -= 1000000;
      timeout_source->expiration.tv_sec++;
    }
}

static gboolean
g_timeout_prepare  (GSource  *source,
		    gint     *timeout)
{
  glong sec;
  glong msec;
  GTimeVal current_time;
  
  GTimeoutSource *timeout_source = (GTimeoutSource *)source;

  g_source_get_current_time (source, &current_time);

  sec = timeout_source->expiration.tv_sec - current_time.tv_sec;
  msec = (timeout_source->expiration.tv_usec - current_time.tv_usec) / 1000;

  /* We do the following in a rather convoluted fashion to deal with
   * the fact that we don't have an integral type big enough to hold
   * the difference of two timevals in millseconds.
   */
  if (sec < 0 || (sec == 0 && msec < 0))
    msec = 0;
  else
    {
      glong interval_sec = timeout_source->interval / 1000;
      glong interval_msec = timeout_source->interval % 1000;

      if (msec < 0)
	{
	  msec += 1000;
	  sec -= 1;
	}
      
      if (sec > interval_sec ||
	  (sec == interval_sec && msec > interval_msec))
	{
	  /* The system time has been set backwards, so we
	   * reset the expiration time to now + timeout_source->interval;
	   * this at least avoids hanging for long periods of time.
	   */
	  g_timeout_set_expiration (timeout_source, &current_time);
	  msec = MIN (G_MAXINT, timeout_source->interval);
	}
      else
	{
	  msec = MIN (G_MAXINT, (guint)msec + 1000 * (guint)sec);
	}
    }

  *timeout = (gint)msec;
  
  return msec == 0;
}

static gboolean 
g_timeout_check (GSource  *source)
{
  GTimeVal current_time;
  GTimeoutSource *timeout_source = (GTimeoutSource *)source;

  g_source_get_current_time (source, &current_time);
  
  return ((timeout_source->expiration.tv_sec < current_time.tv_sec) ||
	  ((timeout_source->expiration.tv_sec == current_time.tv_sec) &&
	   (timeout_source->expiration.tv_usec <= current_time.tv_usec)));
}

static gboolean
g_timeout_dispatch (GSource    *source,
		    GSourceFunc callback,
		    gpointer    user_data)
{
  GTimeoutSource *timeout_source = (GTimeoutSource *)source;

  if (!callback)
    {
      g_warning ("Timeout source dispatched without callback\n"
		 "You must call g_source_set_callback().");
      return FALSE;
    }
 
  if (callback (user_data))
    {
      GTimeVal current_time;

      g_source_get_current_time (source, &current_time);
      g_timeout_set_expiration (timeout_source, &current_time);

      return TRUE;
    }
  else
    return FALSE;
}

/**
 * g_timeout_source_new:
 * @interval: the timeout interval in milliseconds.
 * 
 * Creates a new timeout source.
 *
 * The source will not initially be associated with any #GMainContext
 * and must be added to one with g_source_attach() before it will be
 * executed.
 * 
 * Return value: the newly-created timeout source
 **/
GSource *
g_timeout_source_new (guint interval)
{
  GSource *source = g_source_new (&g_timeout_funcs, sizeof (GTimeoutSource));
  GTimeoutSource *timeout_source = (GTimeoutSource *)source;
  GTimeVal current_time;

  timeout_source->interval = interval;

  g_get_current_time (&current_time);
  g_timeout_set_expiration (timeout_source, &current_time);
  
  return source;
}

/**
 * g_timeout_add_full:
 * @priority: the priority of the idle source. Typically this will be in the
 *            range between #G_PRIORITY_DEFAULT_IDLE and #G_PRIORITY_HIGH_IDLE.
 * @interval: the time between calls to the function, in milliseconds
 *             (1/1000ths of a second)
 * @function: function to call
 * @data:     data to pass to @function
 * @notify:   function to call when the idle is removed, or %NULL
 * 
 * Sets a function to be called at regular intervals, with the given
 * priority.  The function is called repeatedly until it returns
 * %FALSE, at which point the timeout is automatically destroyed and
 * the function will not be called again.  The @notify function is
 * called when the timeout is destroyed.  The first call to the
 * function will be at the end of the first @interval.
 *
 * Note that timeout functions may be delayed, due to the processing of other
 * event sources. Thus they should not be relied on for precise timing.
 * After each call to the timeout function, the time of the next
 * timeout is recalculated based on the current time and the given interval
 * (it does not try to 'catch up' time lost in delays).
 * 
 * Return value: the id of event source.
 **/
guint
g_timeout_add_full (gint           priority,
		    guint          interval,
		    GSourceFunc    function,
		    gpointer       data,
		    GDestroyNotify notify)
{
  GSource *source;
  guint id;
  
  g_return_val_if_fail (function != NULL, 0);

  source = g_timeout_source_new (interval);

  if (priority != G_PRIORITY_DEFAULT)
    g_source_set_priority (source, priority);

  g_source_set_callback (source, function, data, notify);
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}

/**
 * g_timeout_add:
 * @interval: the time between calls to the function, in milliseconds
 *             (1/1000ths of a second)
 * @function: function to call
 * @data:     data to pass to @function
 * 
 * Sets a function to be called at regular intervals, with the default
 * priority, #G_PRIORITY_DEFAULT.  The function is called repeatedly
 * until it returns %FALSE, at which point the timeout is automatically
 * destroyed and the function will not be called again.  The first call
 * to the function will be at the end of the first @interval.
 *
 * Note that timeout functions may be delayed, due to the processing of other
 * event sources. Thus they should not be relied on for precise timing.
 * After each call to the timeout function, the time of the next
 * timeout is recalculated based on the current time and the given interval
 * (it does not try to 'catch up' time lost in delays).
 * 
 * Return value: the id of event source.
 **/
guint 
g_timeout_add (guint32        interval,
	       GSourceFunc    function,
	       gpointer       data)
{
  return g_timeout_add_full (G_PRIORITY_DEFAULT, 
			     interval, function, data, NULL);
}

/* Child watch functions */

#ifdef G_OS_WIN32

static gboolean
g_child_watch_prepare (GSource *source,
		       gint    *timeout)
{
  *timeout = -1;
  return FALSE;
}


static gboolean 
g_child_watch_check (GSource  *source)
{
  GChildWatchSource *child_watch_source;
  gboolean child_exited;

  child_watch_source = (GChildWatchSource *) source;

  child_exited = child_watch_source->poll.revents & G_IO_IN;

  if (child_exited)
    {
      DWORD child_status;

      /*
       * Note: We do _not_ check for the special value of STILL_ACTIVE
       * since we know that the process has exited and doing so runs into
       * problems if the child process "happens to return STILL_ACTIVE(259)"
       * as Microsoft's Platform SDK puts it.
       */
      if (!GetExitCodeProcess (child_watch_source->pid, &child_status))
        {
	  gchar *emsg = g_win32_error_message (GetLastError ());
	  g_warning (G_STRLOC ": GetExitCodeProcess() failed: %s", emsg);
	  g_free (emsg);

	  child_watch_source->child_status = -1;
	}
      else
	child_watch_source->child_status = child_status;
    }

  return child_exited;
}

#else /* G_OS_WIN32 */

static gboolean
check_for_child_exited (GSource *source)
{
  GChildWatchSource *child_watch_source;
  gint count;

  /* protect against another SIGCHLD in the middle of this call */
  count = child_watch_count;

  child_watch_source = (GChildWatchSource *) source;

  if (child_watch_source->child_exited)
    return TRUE;

  if (child_watch_source->count < count)
    {
      gint child_status;

      if (waitpid (child_watch_source->pid, &child_status, WNOHANG) > 0)
	{
	  child_watch_source->child_status = child_status;
	  child_watch_source->child_exited = TRUE;
	}
      child_watch_source->count = count;
    }

  return child_watch_source->child_exited;
}

static gboolean
g_child_watch_prepare (GSource *source,
		       gint    *timeout)
{
  GChildWatchSource *child_watch_source;
  *timeout = -1;

  child_watch_source = (GChildWatchSource *) source;

  return check_for_child_exited (source);
}


static gboolean 
g_child_watch_check (GSource  *source)
{
  GChildWatchSource *child_watch_source;

  child_watch_source = (GChildWatchSource *) source;

  return check_for_child_exited (source);
}

#endif /* G_OS_WIN32 */

static gboolean
g_child_watch_dispatch (GSource    *source, 
			GSourceFunc callback,
			gpointer    user_data)
{
  GChildWatchSource *child_watch_source;
  GChildWatchFunc child_watch_callback = (GChildWatchFunc) callback;

  child_watch_source = (GChildWatchSource *) source;

  if (!callback)
    {
      g_warning ("Child watch source dispatched without callback\n"
		 "You must call g_source_set_callback().");
      return FALSE;
    }

  (child_watch_callback) (child_watch_source->pid, child_watch_source->child_status, user_data);

  /* We never keep a child watch source around as the child is gone */
  return FALSE;
}

#ifndef G_OS_WIN32

static void
g_child_watch_signal_handler (int signum)
{
  child_watch_count ++;

  if (child_watch_init_state == CHILD_WATCH_INITIALIZED_THREADED)
    {
      write (child_watch_wake_up_pipe[1], "B", 1);
    }
  else
    {
      /* We count on the signal interrupting the poll in the same thread.
       */
    }
}
 
static void
g_child_watch_source_init_single (void)
{
  struct sigaction action;

  g_assert (! g_thread_supported());
  g_assert (child_watch_init_state == CHILD_WATCH_UNINITIALIZED);

  child_watch_init_state = CHILD_WATCH_INITIALIZED_SINGLE;

  action.sa_handler = g_child_watch_signal_handler;
  sigemptyset (&action.sa_mask);
  action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction (SIGCHLD, &action, NULL);
}

static gpointer
child_watch_helper_thread (gpointer data)
{
  GPollFD fds;
  GPollFunc poll_func;

#ifdef HAVE_POLL
      poll_func = (GPollFunc)poll;
#else
      poll_func = g_poll;
#endif

  fds.fd = child_watch_wake_up_pipe[0];
  fds.events = G_IO_IN;

  while (1)
    {
      gchar b[20];
      GSList *list;

      read (child_watch_wake_up_pipe[0], b, 20);

      /* We were woken up.  Wake up all other contexts in all other threads */
      G_LOCK (main_context_list);
      for (list = main_context_list; list; list = list->next)
	{
	  GMainContext *context;

	  context = list->data;
	  if (g_atomic_int_get (&context->ref_count) > 0)
	    /* Due to racing conditions we can find ref_count == 0, in
	     * that case, however, the context is still not destroyed
	     * and no poll can be active, otherwise the ref_count
	     * wouldn't be 0 */
	    g_main_context_wakeup (context);
	}
      G_UNLOCK (main_context_list);
    }
  return NULL;
}

static void
g_child_watch_source_init_multi_threaded (void)
{
  GError *error = NULL;
  struct sigaction action;

  g_assert (g_thread_supported());

  if (pipe (child_watch_wake_up_pipe) < 0)
    g_error ("Cannot create wake up pipe: %s\n", g_strerror (errno));
  fcntl (child_watch_wake_up_pipe[1], F_SETFL, O_NONBLOCK | fcntl (child_watch_wake_up_pipe[1], F_GETFL));

  /* We create a helper thread that polls on the wakeup pipe indefinitely */
  /* FIXME: Think this through for races */
  if (g_thread_create (child_watch_helper_thread, NULL, FALSE, &error) == NULL)
    g_error ("Cannot create a thread to monitor child exit status: %s\n", error->message);
  child_watch_init_state = CHILD_WATCH_INITIALIZED_THREADED;
 
  action.sa_handler = g_child_watch_signal_handler;
  sigemptyset (&action.sa_mask);
  action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction (SIGCHLD, &action, NULL);
}

static void
g_child_watch_source_init_promote_single_to_threaded (void)
{
  g_child_watch_source_init_multi_threaded ();
}

static void
g_child_watch_source_init (void)
{
  if (g_thread_supported())
    {
      if (child_watch_init_state == CHILD_WATCH_UNINITIALIZED)
	g_child_watch_source_init_multi_threaded ();
      else if (child_watch_init_state == CHILD_WATCH_INITIALIZED_SINGLE)
	g_child_watch_source_init_promote_single_to_threaded ();
    }
  else
    {
      if (child_watch_init_state == CHILD_WATCH_UNINITIALIZED)
	g_child_watch_source_init_single ();
    }
}

#endif /* !G_OS_WIN32 */

/**
 * g_child_watch_source_new:
 * @pid: process id of a child process to watch. On Windows, a HANDLE
 * for the process to watch (which actually doesn't have to be a child).
 * 
 * Creates a new child_watch source.
 *
 * The source will not initially be associated with any #GMainContext
 * and must be added to one with g_source_attach() before it will be
 * executed.
 * 
 * Note that on platforms where #GPid must be explicitely closed
 * (see g_spawn_close_pid()) @pid must not be closed while the
 * source is still active. Typically, you will want to call
 * g_spawn_close_pid() in the callback function for the source.
 * 
 * Return value: the newly-created child watch source
 *
 * Since: 2.4
 **/
GSource *
g_child_watch_source_new (GPid pid)
{
  GSource *source = g_source_new (&g_child_watch_funcs, sizeof (GChildWatchSource));
  GChildWatchSource *child_watch_source = (GChildWatchSource *)source;

#ifdef G_OS_WIN32
  child_watch_source->poll.fd = (int)pid;
  child_watch_source->poll.events = G_IO_IN;

  g_source_add_poll (source, &child_watch_source->poll);
#else /* G_OS_WIN32 */
  g_child_watch_source_init ();
#endif /* G_OS_WIN32 */

  child_watch_source->pid = pid;

  return source;
}

/**
 * g_child_watch_add_full:
 * @priority: the priority of the idle source. Typically this will be in the
 *            range between #G_PRIORITY_DEFAULT_IDLE and #G_PRIORITY_HIGH_IDLE.
 * @pid:      process id of a child process to watch
 * @function: function to call
 * @data:     data to pass to @function
 * @notify:   function to call when the idle is removed, or %NULL
 * 
 * Sets a function to be called when the child indicated by @pid exits, at a
 * default priority, #G_PRIORITY_DEFAULT.
 * 
 * Note that on platforms where #GPid must be explicitely closed
 * (see g_spawn_close_pid()) @pid must not be closed while the
 * source is still active. Typically, you will want to call
 * g_spawn_close_pid() in the callback function for the source.
 * 
 * GLib supports only a single callback per process id.
 *
 * Return value: the id of event source.
 *
 * Since: 2.4
 **/
guint
g_child_watch_add_full (gint            priority,
			GPid            pid,
			GChildWatchFunc function,
			gpointer        data,
			GDestroyNotify  notify)
{
  GSource *source;
  guint id;
  
  g_return_val_if_fail (function != NULL, 0);

  source = g_child_watch_source_new (pid);

  if (priority != G_PRIORITY_DEFAULT)
    g_source_set_priority (source, priority);

  g_source_set_callback (source, (GSourceFunc) function, data, notify);
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}

/**
 * g_child_watch_add:
 * @pid:      process id of a child process to watch
 * @function: function to call
 * @data:     data to pass to @function
 * 
 * Sets a function to be called when the child indicated by @pid exits, at a
 * default priority, #G_PRIORITY_DEFAULT.
 * 
 * Note that on platforms where #GPid must be explicitely closed
 * (see g_spawn_close_pid()) @pid must not be closed while the
 * source is still active. Typically, you will want to call
 * g_spawn_close_pid() in the callback function for the source.
 *
 * GLib supports only a single callback per process id.
 *
 * Return value: the id of event source.
 *
 * Since: 2.4
 **/
guint 
g_child_watch_add (GPid            pid,
		   GChildWatchFunc function,
		   gpointer        data)
{
  return g_child_watch_add_full (G_PRIORITY_DEFAULT, pid, function, data, NULL);
}


/* Idle functions */

static gboolean 
g_idle_prepare  (GSource  *source,
		 gint     *timeout)
{
  *timeout = 0;

  return TRUE;
}

static gboolean 
g_idle_check    (GSource  *source)
{
  return TRUE;
}

static gboolean
g_idle_dispatch (GSource    *source, 
		 GSourceFunc callback,
		 gpointer    user_data)
{
  if (!callback)
    {
      g_warning ("Idle source dispatched without callback\n"
		 "You must call g_source_set_callback().");
      return FALSE;
    }
  
  return callback (user_data);
}

/**
 * g_idle_source_new:
 * 
 * Creates a new idle source.
 *
 * The source will not initially be associated with any #GMainContext
 * and must be added to one with g_source_attach() before it will be
 * executed. Note that the default priority for idle sources is
 * %G_PRIORITY_DEFAULT_IDLE, as compared to other sources which
 * have a default priority of %G_PRIORITY_DEFAULT.
 * 
 * Return value: the newly-created idle source
 **/
GSource *
g_idle_source_new (void)
{
  GSource *source;

  source = g_source_new (&g_idle_funcs, sizeof (GSource));
  g_source_set_priority (source, G_PRIORITY_DEFAULT_IDLE);

  return source;
}

/**
 * g_idle_add_full:
 * @priority: the priority of the idle source. Typically this will be in the
 *            range btweeen #G_PRIORITY_DEFAULT_IDLE and #G_PRIORITY_HIGH_IDLE.
 * @function: function to call
 * @data:     data to pass to @function
 * @notify:   function to call when the idle is removed, or %NULL
 * 
 * Adds a function to be called whenever there are no higher priority
 * events pending.  If the function returns %FALSE it is automatically
 * removed from the list of event sources and will not be called again.
 * 
 * Return value: the id of the event source.
 **/
guint 
g_idle_add_full (gint           priority,
		 GSourceFunc    function,
		 gpointer       data,
		 GDestroyNotify notify)
{
  GSource *source;
  guint id;
  
  g_return_val_if_fail (function != NULL, 0);

  source = g_idle_source_new ();

  if (priority != G_PRIORITY_DEFAULT_IDLE)
    g_source_set_priority (source, priority);

  g_source_set_callback (source, function, data, notify);
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}

/**
 * g_idle_add:
 * @function: function to call 
 * @data: data to pass to @function.
 * 
 * Adds a function to be called whenever there are no higher priority
 * events pending to the default main loop. The function is given the
 * default idle priority, #G_PRIORITY_DEFAULT_IDLE.  If the function
 * returns %FALSE it is automatically removed from the list of event
 * sources and will not be called again.
 * 
 * Return value: the id of the event source.
 **/
guint 
g_idle_add (GSourceFunc    function,
	    gpointer       data)
{
  return g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, function, data, NULL);
}

/**
 * g_idle_remove_by_data:
 * @data: the data for the idle source's callback.
 * 
 * Removes the idle function with the given data.
 * 
 * Return value: %TRUE if an idle source was found and removed.
 **/
gboolean
g_idle_remove_by_data (gpointer data)
{
  return g_source_remove_by_funcs_user_data (&g_idle_funcs, data);
}

