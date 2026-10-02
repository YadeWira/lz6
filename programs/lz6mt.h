/*
 * lz6mt.h - a small ordered worker pool for the lz6 command line.
 *
 * The main thread fills slots in order, workers process them in any order,
 * and the main thread collects them back in the same order: the output of a
 * multithreaded run is byte-identical to the single-threaded one.
 *
 *   mt_pool_t* p = mt_pool_create(nThreads, nSlots, workFn, workCtxArray);
 *   slot = mt_pool_acquire(p);     // blocks until the oldest slot is free
 *   ... fill slot->in / slot->inSize / slot->tag ...
 *   mt_pool_submit(p, slot);
 *   slot = mt_pool_next_done(p);   // oldest submitted slot, blocks until done
 *   ... use slot->out / slot->outSize / slot->error ...
 *   mt_pool_release(p, slot);
 *   mt_pool_destroy(p);
 *
 * POSIX: pthreads. Windows: Win32 threads, critical sections and condition
 * variables (Vista and later; the project targets Windows 7 SP1).
 */
#ifndef LZ6MT_H
#define LZ6MT_H

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef HANDLE            mt_thread_t;
typedef CRITICAL_SECTION  mt_mutex_t;
typedef CONDITION_VARIABLE mt_cond_t;
#  define MT_LOCK(m)        EnterCriticalSection(m)
#  define MT_UNLOCK(m)      LeaveCriticalSection(m)
#  define MT_WAIT(c, m)     SleepConditionVariableCS((c), (m), INFINITE)
#  define MT_SIGNAL(c)      WakeConditionVariable(c)
#  define MT_BROADCAST(c)   WakeAllConditionVariable(c)
#  define MT_MUTEX_INIT(m)  InitializeCriticalSection(m)
#  define MT_MUTEX_FREE(m)  DeleteCriticalSection(m)
#  define MT_COND_INIT(c)   InitializeConditionVariable(c)
#  define MT_COND_FREE(c)   ((void)0)
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_t         mt_thread_t;
typedef pthread_mutex_t   mt_mutex_t;
typedef pthread_cond_t    mt_cond_t;
#  define MT_LOCK(m)        pthread_mutex_lock(m)
#  define MT_UNLOCK(m)      pthread_mutex_unlock(m)
#  define MT_WAIT(c, m)     pthread_cond_wait((c), (m))
#  define MT_SIGNAL(c)      pthread_cond_signal(c)
#  define MT_BROADCAST(c)   pthread_cond_broadcast(c)
#  define MT_MUTEX_INIT(m)  pthread_mutex_init((m), NULL)
#  define MT_MUTEX_FREE(m)  pthread_mutex_destroy(m)
#  define MT_COND_INIT(c)   pthread_cond_init((c), NULL)
#  define MT_COND_FREE(c)   pthread_cond_destroy(c)
#endif

enum { MT_FREE = 0, MT_FILLED = 1, MT_BUSY = 2, MT_DONE = 3 };

typedef struct {
    int state;
    unsigned long long seq;            /* submission order */
    unsigned char* in;  size_t inSize; size_t inCap;
    unsigned char* out; size_t outSize; size_t outCap;
    unsigned tag;                      /* caller's: e.g. a block header word */
    size_t limit;                      /* caller's: how much the worker may write to out (<= outCap) */
    int error;                         /* set by the worker */
} mt_slot_t;

typedef void (*mt_work_fn)(void* workerCtx, mt_slot_t* slot);

typedef struct mt_pool_s {
    int nThreads, nSlots;
    int started;                       /* threads running; only the owner touches it */
    mt_slot_t* slots;
    mt_work_fn work;
    void** workerCtx;                  /* one per thread */
    mt_mutex_t mu;
    mt_cond_t cvWork, cvDone;
    int shutdown;
    unsigned long long nextSubmit, nextCollect;
    mt_thread_t* threads;
    struct mt_arg_s* args;
} mt_pool_t;

struct mt_arg_s { mt_pool_t* pool; int idx; };

static void mt_worker_loop(struct mt_arg_s* a)
{
    mt_pool_t* const p = a->pool;
    for (;;) {
        mt_slot_t* s = NULL;
        int i;
        MT_LOCK(&p->mu);
        for (;;) {
            unsigned long long best = ~0ULL;
            for (i = 0; i < p->nSlots; i++)
                if (p->slots[i].state == MT_FILLED && p->slots[i].seq < best) { best = p->slots[i].seq; s = &p->slots[i]; }
            if (s) break;
            if (p->shutdown) { MT_UNLOCK(&p->mu); return; }
            MT_WAIT(&p->cvWork, &p->mu);
        }
        s->state = MT_BUSY;
        MT_UNLOCK(&p->mu);

        p->work(p->workerCtx[a->idx], s);

        MT_LOCK(&p->mu);
        s->state = MT_DONE;
        MT_BROADCAST(&p->cvDone);
        MT_UNLOCK(&p->mu);
    }
}

#ifdef _WIN32
static DWORD WINAPI mt_thread_main(LPVOID arg) { mt_worker_loop((struct mt_arg_s*)arg); return 0; }
#else
static void* mt_thread_main(void* arg) { mt_worker_loop((struct mt_arg_s*)arg); return NULL; }
#endif

/* start one more worker; 0 if the system refuses */
static int mt_spawn(mt_pool_t* p)
{
    int const i = p->started;
    p->args[i].pool = p; p->args[i].idx = i;
#ifdef _WIN32
    p->threads[i] = CreateThread(NULL, 0, mt_thread_main, &p->args[i], 0, NULL);
    if (!p->threads[i]) return 0;
#else
    if (pthread_create(&p->threads[i], NULL, mt_thread_main, &p->args[i]) != 0) return 0;
#endif
    p->started++;
    return 1;
}

static void mt_join_all(mt_pool_t* p)
{
    int i;
    MT_LOCK(&p->mu); p->shutdown = 1; MT_BROADCAST(&p->cvWork); MT_UNLOCK(&p->mu);
    for (i = 0; i < p->started; i++) {
#ifdef _WIN32
        WaitForSingleObject(p->threads[i], INFINITE); CloseHandle(p->threads[i]);
#else
        pthread_join(p->threads[i], NULL);
#endif
    }
    p->started = 0;
}

/* Workers are started on demand, up to nThreads, when more slots are waiting
 * than there are workers; one is started here so that a pool that exists can
 * always make progress. Returns NULL on failure (the caller runs on one thread). */
static mt_pool_t* mt_pool_create(int nThreads, int nSlots, mt_work_fn work, void** workerCtx)
{
    mt_pool_t* p = (mt_pool_t*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->nThreads = nThreads; p->nSlots = nSlots; p->work = work; p->workerCtx = workerCtx;
    p->slots = (mt_slot_t*)calloc((size_t)nSlots, sizeof(mt_slot_t));
    p->threads = (mt_thread_t*)calloc((size_t)nThreads, sizeof(mt_thread_t));
    p->args = (struct mt_arg_s*)calloc((size_t)nThreads, sizeof(struct mt_arg_s));
    if (!p->slots || !p->threads || !p->args) { free(p->slots); free(p->threads); free(p->args); free(p); return NULL; }
    MT_MUTEX_INIT(&p->mu); MT_COND_INIT(&p->cvWork); MT_COND_INIT(&p->cvDone);
    if (!mt_spawn(p)) {
        MT_MUTEX_FREE(&p->mu); MT_COND_FREE(&p->cvWork); MT_COND_FREE(&p->cvDone);
        free(p->slots); free(p->threads); free(p->args); free(p);
        return NULL;
    }
    return p;
}

/* the next slot to fill, or NULL if all are in flight: collect one first */
static mt_slot_t* mt_pool_acquire(mt_pool_t* p)
{
    mt_slot_t* s = &p->slots[p->nextSubmit % (unsigned long long)p->nSlots];
    int isFree;
    MT_LOCK(&p->mu);
    isFree = s->state == MT_FREE;
    MT_UNLOCK(&p->mu);
    return isFree ? s : NULL;
}

static void mt_pool_submit(mt_pool_t* p, mt_slot_t* s)
{
    int i, waiting = 0;
    MT_LOCK(&p->mu);
    s->seq = p->nextSubmit++;
    s->error = 0;
    s->state = MT_FILLED;
    for (i = 0; i < p->nSlots; i++)
        if (p->slots[i].state == MT_FILLED || p->slots[i].state == MT_BUSY) waiting++;
    MT_SIGNAL(&p->cvWork);
    MT_UNLOCK(&p->mu);
    if (waiting > p->started && p->started < p->nThreads) mt_spawn(p);   /* best effort */
}

static int mt_pool_pending(const mt_pool_t* p) { return (int)(p->nextSubmit - p->nextCollect); }

/* oldest submitted slot, once its worker is done; NULL if nothing is pending */
static mt_slot_t* mt_pool_next_done(mt_pool_t* p)
{
    mt_slot_t* s;
    if (p->nextCollect == p->nextSubmit) return NULL;
    s = &p->slots[p->nextCollect % (unsigned long long)p->nSlots];
    MT_LOCK(&p->mu);
    while (s->state != MT_DONE) MT_WAIT(&p->cvDone, &p->mu);
    MT_UNLOCK(&p->mu);
    return s;
}

/* true when the oldest submitted slot is already done (never blocks) */
static int mt_pool_oldest_ready(mt_pool_t* p)
{
    int r;
    if (p->nextCollect == p->nextSubmit) return 0;
    MT_LOCK(&p->mu);
    r = p->slots[p->nextCollect % (unsigned long long)p->nSlots].state == MT_DONE;
    MT_UNLOCK(&p->mu);
    return r;
}

static void mt_pool_release(mt_pool_t* p, mt_slot_t* s)
{
    MT_LOCK(&p->mu);
    s->state = MT_FREE;
    p->nextCollect++;
    MT_UNLOCK(&p->mu);
}

static void mt_pool_destroy(mt_pool_t* p)
{
    int i;
    if (!p) return;
    mt_join_all(p);
    for (i = 0; i < p->nSlots; i++) { free(p->slots[i].in); free(p->slots[i].out); }
    MT_MUTEX_FREE(&p->mu); MT_COND_FREE(&p->cvWork); MT_COND_FREE(&p->cvDone);
    free(p->slots); free(p->threads); free(p->args); free(p);
}

static int mt_hardware_threads(void)
{
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}

#endif /* LZ6MT_H */
