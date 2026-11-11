#include "test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "threadpool.h"

static atomic_int counter;

static void add_one(void *arg) {
    (void)arg;
    atomic_fetch_add(&counter, 1);
}

static void test_invalid_arguments(void) {
    CHECK(tp_create(0, 10) == NULL);
    CHECK(tp_create(4, 0) == NULL);
    tp_destroy(NULL);
}

/* Every submitted job runs exactly once, and destroy waits for the queue to drain. */
static void test_runs_all_jobs(void) {
    atomic_store(&counter, 0);
    ThreadPool *tp = tp_create(4, 10000);
    CHECK(tp != NULL);
    for (int i = 0; i < 10000; i++) CHECK(tp_submit(tp, add_one, NULL) == 0);
    tp_destroy(tp);
    CHECK(atomic_load(&counter) == 10000);
}

/* A gate the test controls, so jobs can be held inside the workers. */
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool gate_open;
static atomic_int started, running, max_running;

static void wait_at_gate(void *arg) {
    (void)arg;
    int now = atomic_fetch_add(&running, 1) + 1;
    int seen = atomic_load(&max_running);
    while (now > seen && !atomic_compare_exchange_weak(&max_running, &seen, now)) {}
    atomic_fetch_add(&started, 1);

    pthread_mutex_lock(&gate_mu);
    while (!gate_open) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    atomic_fetch_sub(&running, 1);
}

static void open_gate(void) {
    pthread_mutex_lock(&gate_mu);
    gate_open = true;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

static void wait_until_started(int n) {
    for (int i = 0; i < 5000 && atomic_load(&started) < n; i++) usleep(1000);
}

/* With every worker busy, the queue fills up and further submits are refused
   instead of blocking. */
static void test_full_queue_rejects(void) {
    gate_open = false;
    atomic_store(&started, 0);
    atomic_store(&running, 0);
    atomic_store(&max_running, 0);

    ThreadPool *tp = tp_create(2, 3);
    CHECK(tp_submit(tp, wait_at_gate, NULL) == 0);
    CHECK(tp_submit(tp, wait_at_gate, NULL) == 0);
    wait_until_started(2); /* both workers are now parked at the gate */
    CHECK(tp_active(tp) == 2 && tp_queued(tp) == 0);

    for (int i = 0; i < 3; i++) CHECK(tp_submit(tp, wait_at_gate, NULL) == 0); /* fills the queue */
    CHECK(tp_queued(tp) == 3);
    CHECK(tp_submit(tp, wait_at_gate, NULL) == -1);
    CHECK(tp_submit(tp, wait_at_gate, NULL) == -1);

    open_gate();
    tp_destroy(tp);
    CHECK(atomic_load(&started) == 5);     /* the queued jobs still ran; the rejected ones did not */
    CHECK(atomic_load(&max_running) == 2); /* never more jobs at once than threads */
}

/* Jobs really do run at the same time on different threads. */
static void test_jobs_run_in_parallel(void) {
    gate_open = false;
    atomic_store(&started, 0);
    atomic_store(&running, 0);
    atomic_store(&max_running, 0);

    ThreadPool *tp = tp_create(4, 16);
    for (int i = 0; i < 4; i++) tp_submit(tp, wait_at_gate, NULL);
    wait_until_started(4);
    CHECK(atomic_load(&running) == 4); /* all four are inside a job simultaneously */
    open_gate();
    tp_destroy(tp);
    CHECK(atomic_load(&max_running) == 4);
}

int main(void) {
    RUN(test_invalid_arguments);
    RUN(test_runs_all_jobs);
    RUN(test_full_queue_rejects);
    RUN(test_jobs_run_in_parallel);
    TEST_REPORT("test_threadpool");
}
