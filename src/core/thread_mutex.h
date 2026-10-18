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
   /* Give another ready thread the CPU without sleeping. Sleep(1) is ~15.6 ms
    * in a process with neither a window nor an audio device, which is far too
    * expensive to spend on letting the UI take a mutex. */
#  define emu_yield()           ((void)SwitchToThread())
   /* A condition variable on an emu_mutex_t, for the sound board's thread. */
   typedef CONDITION_VARIABLE emu_cond_t;
#  define emu_cond_init(c)      InitializeConditionVariable(c)
#  define emu_cond_wait(c, m)   ((void)SleepConditionVariableCS((c), (m), INFINITE))
#  define emu_cond_signal(c)    WakeConditionVariable(c)
#  define emu_cond_broadcast(c) WakeAllConditionVariable(c)
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <time.h>
#  include <sched.h>
   typedef pthread_t       emu_thread_t;
   typedef pthread_mutex_t emu_mutex_t;
#  define emu_mutex_init(m)     pthread_mutex_init(m, NULL)
#  define emu_mutex_destroy(m)  pthread_mutex_destroy(m)
#  define emu_mutex_lock(m)     pthread_mutex_lock(m)
#  define emu_mutex_unlock(m)   pthread_mutex_unlock(m)
#  define emu_sleep_ms(ms)      usleep((useconds_t)((ms) * 1000))
#  ifdef _arch_dreamcast
#    include <kos/thread.h>
#    define emu_yield()         thd_pass()        /* KallistiOS: newlib hides sched_yield */
#  else
#    define emu_yield()         ((void)sched_yield())
#  endif
   typedef pthread_cond_t  emu_cond_t;
#  define emu_cond_init(c)      pthread_cond_init(c, NULL)
#  define emu_cond_wait(c, m)   pthread_cond_wait(c, m)
#  define emu_cond_signal(c)    pthread_cond_signal(c)
#  define emu_cond_broadcast(c) pthread_cond_broadcast(c)
#endif

#endif /* THREAD_MUTEX_H */
