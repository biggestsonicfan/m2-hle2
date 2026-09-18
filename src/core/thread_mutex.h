/*
 * thread_mutex.h — the platform threading primitives, split out of emu_thread.h.
 *
 * emu_thread.h still owns the threading MODEL (the run loop, the snapshot
 * double-buffering, the "unlock before sleeping" invariant). This header holds
 * only the types and macros it is built from, because net/netplay.h needs the
 * same mutex to guard its command queue and status snapshot, and emu_thread.h
 * calls into netplay from the run loop — so the two cannot include each other.
 *
 * Nothing new here: these are the definitions emu_thread.h carried, moved
 * verbatim so both sides can reach them.
 */
#ifndef THREAD_MUTEX_H
#define THREAD_MUTEX_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef HANDLE           emu_thread_t;
   typedef CRITICAL_SECTION emu_mutex_t;
#  define emu_mutex_init(m)     InitializeCriticalSection(m)
#  define emu_mutex_destroy(m)  DeleteCriticalSection(m)
#  define emu_mutex_lock(m)     EnterCriticalSection(m)
#  define emu_mutex_unlock(m)   LeaveCriticalSection(m)
#  define emu_sleep_ms(ms)      Sleep((DWORD)(ms))
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <time.h>
   typedef pthread_t       emu_thread_t;
   typedef pthread_mutex_t emu_mutex_t;
#  define emu_mutex_init(m)     pthread_mutex_init(m, NULL)
#  define emu_mutex_destroy(m)  pthread_mutex_destroy(m)
#  define emu_mutex_lock(m)     pthread_mutex_lock(m)
#  define emu_mutex_unlock(m)   pthread_mutex_unlock(m)
#  define emu_sleep_ms(ms)      usleep((useconds_t)((ms) * 1000))
#endif

#endif /* THREAD_MUTEX_H */
