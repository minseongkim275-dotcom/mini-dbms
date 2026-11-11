#ifndef MINIDB_THREADPOOL_H
#define MINIDB_THREADPOOL_H

/* Fixed-size pool of worker threads fed by a bounded FIFO queue. */

typedef struct ThreadPool ThreadPool;
typedef void (*JobFn)(void *arg);

/* Returns NULL if the arguments are invalid or threads cannot be created. */
ThreadPool *tp_create(int nthreads, int queue_capacity);

/* Queues a job without blocking. Returns 0, or -1 if the queue is full or the
   pool is shutting down (the caller keeps ownership of `arg`). */
int tp_submit(ThreadPool *tp, JobFn fn, void *arg);

/* Stops accepting jobs, lets the workers finish everything already queued,
   then joins them and frees the pool. */
void tp_destroy(ThreadPool *tp);

int tp_queued(ThreadPool *tp); /* jobs waiting in the queue */
int tp_active(ThreadPool *tp); /* jobs currently running */

#endif
