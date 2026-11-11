#include "threadpool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    JobFn fn;
    void *arg;
} Job;

struct ThreadPool {
    pthread_mutex_t mu;
    pthread_cond_t not_empty; /* signalled when a job is queued or shutdown starts */
    Job *queue;               /* circular buffer */
    int capacity, head, count;
    int active;
    bool shutdown;
    pthread_t *threads;
    int nthreads;
};

static void *worker(void *arg) {
    ThreadPool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->mu);
        /* The loop re-checks the condition because pthread_cond_wait can wake
           spuriously and another worker may have taken the job first. */
        while (tp->count == 0 && !tp->shutdown) pthread_cond_wait(&tp->not_empty, &tp->mu);
        if (tp->count == 0) { /* shutdown and nothing left to do */
            pthread_mutex_unlock(&tp->mu);
            return NULL;
        }
        Job job = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->capacity;
        tp->count--;
        tp->active++;
        pthread_mutex_unlock(&tp->mu);

        job.fn(job.arg); /* run outside the lock so workers execute in parallel */

        pthread_mutex_lock(&tp->mu);
        tp->active--;
        pthread_mutex_unlock(&tp->mu);
    }
}

ThreadPool *tp_create(int nthreads, int queue_capacity) {
    if (nthreads < 1 || queue_capacity < 1) return NULL;
    ThreadPool *tp = calloc(1, sizeof *tp);
    if (!tp) return NULL;
    tp->queue = calloc((size_t)queue_capacity, sizeof(Job));
    tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!tp->queue || !tp->threads) {
        free(tp->queue);
        free(tp->threads);
        free(tp);
        return NULL;
    }
    tp->capacity = queue_capacity;
    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0) {
            tp_destroy(tp); /* joins the threads that did start */
            return NULL;
        }
        tp->nthreads++;
    }
    return tp;
}

int tp_submit(ThreadPool *tp, JobFn fn, void *arg) {
    int rc = -1;
    pthread_mutex_lock(&tp->mu);
    if (!tp->shutdown && tp->count < tp->capacity) {
        int tail = (tp->head + tp->count) % tp->capacity;
        tp->queue[tail] = (Job){fn, arg};
        tp->count++;
        pthread_cond_signal(&tp->not_empty);
        rc = 0;
    }
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

void tp_destroy(ThreadPool *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    tp->shutdown = true;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    for (int i = 0; i < tp->nthreads; i++) pthread_join(tp->threads[i], NULL);
    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
    free(tp->queue);
    free(tp->threads);
    free(tp);
}

int tp_queued(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mu);
    int n = tp->count;
    pthread_mutex_unlock(&tp->mu);
    return n;
}

int tp_active(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mu);
    int n = tp->active;
    pthread_mutex_unlock(&tp->mu);
    return n;
}
