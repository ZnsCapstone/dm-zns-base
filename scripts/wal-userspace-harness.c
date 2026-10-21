/* 실제 submit_wal_async()만 삽입한다. 커널 workqueue/jiffies/I/O는 가상 모델이다. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
typedef uint64_t u64;
struct zns_base_c {
    int lock, wal_pending, wal_batch_work;
    unsigned int wal_pending_count;
    bool wal_batch_busy;
    u64 diag_wal_ready_ns;
};
struct zns_io_ctx {
    struct zns_base_c *c;
    int wal_link;
    u64 diag_wal_queued_ns;
};
static bool metrics_enabled, wal_timing_enabled;
/* 기존 timer 테스트는 기본 durable 정책만 검증한다. writeback은 별도 harness. */
static bool wal_try_buffer_write(struct zns_io_ctx *ctx) { (void)ctx; return false; }
static unsigned int wal_batch_max_records = 127, wal_batch_delay_ms;
static int zns_wal_wq;
static u64 now_ns, due_ns = UINT64_MAX, arrival[256];
static u64 first_fire, sum_wait, max_wait, batches, records, max_batch;
#define INIT_LIST_HEAD(x) ((void)(x))
#define list_add_tail(x,y) ((void)(x), (void)(y))
#define spin_lock_irq(x) ((void)(x))
#define spin_unlock_irq(x) ((void)(x))
static u64 ktime_get_ns(void) { return now_ns; }
static u64 msecs_to_jiffies(unsigned int ms) { return ms * 1000000ULL; }
static void mod_delayed_work(int wq, int *work, u64 delay) {
    (void)wq; (void)work; due_ns = now_ns + delay;
}
static void queue_delayed_work(int wq, int *work, u64 delay) {
    (void)wq; (void)work;
    if (due_ns == UINT64_MAX) due_ns = now_ns + delay;
}

/* DRIVER_FUNCTION */

static void fire(struct zns_base_c *c) {
    unsigned int i, n = c->wal_pending_count;
    if (!n) abort();
    if (!batches) first_fire = now_ns;
    for (i = 0; i < n; i++) {
        u64 wait = now_ns - arrival[i];
        sum_wait += wait;
        if (wait > max_wait) max_wait = wait;
    }
    if (n > max_batch) max_batch = n;
    records += n; batches++;
    c->wal_pending_count = 0;
    due_ns = UINT64_MAX;
}

int main(int argc, char **argv) {
    struct zns_base_c c = {0};
    unsigned int i, n;
    u64 step;
    if (argc != 4) return 2;
    wal_batch_delay_ms = (unsigned int)strtoul(argv[1], NULL, 10);
    n = (unsigned int)strtoul(argv[2], NULL, 10);
    step = strtoull(argv[3], NULL, 10);
    if (!n || n > 255) return 2;
    for (i = 0; i < n; i++) {
        struct zns_io_ctx ctx = {.c = &c};
        u64 next = (u64)i * step;
        /* 가상 worker는 deadline에 즉시 실행; 같은 시각 도착보다 먼저 실행. */
        if (due_ns <= next) { now_ns = due_ns; fire(&c); }
        now_ns = next;
        arrival[c.wal_pending_count] = now_ns;
        submit_wal_async(&ctx);
    }
    if (c.wal_pending_count) { now_ns = due_ns; fire(&c); }
    printf("{\"records\":%llu,\"batches\":%llu,\"max_batch\":%llu,"
           "\"first_fire_ms\":%.6f,\"mean_queue_ms\":%.6f,\"max_queue_ms\":%.6f}\n",
           (unsigned long long)records, (unsigned long long)batches,
           (unsigned long long)max_batch, first_fire / 1e6,
           sum_wait / (double)records / 1e6, max_wait / 1e6);
    return 0;
}
