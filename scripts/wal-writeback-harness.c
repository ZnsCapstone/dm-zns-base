/* 실제 장치/스케줄러가 아닌 production C helper의 소유권·정책 회귀. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t u64;
typedef uint64_t sector_t;
typedef int blk_status_t;
#define REQ_FUA 1
#define REQ_PREFLUSH 2
struct list_head {
	unsigned int n;
};

struct bio {
	unsigned int bi_opf;
};

struct skiplist {
	unsigned int count;
};

struct zone_pool {
	unsigned int free;
};

struct zns_base_c {
	int lock, wal_batch_work;
	bool metadata_failed, checkpoint_failed, stopping, gc_active;
	bool wal_rotation_pending, sstable_rotation_pending, wal_batch_busy, wal_batch_has_waiters;
	unsigned int wal_ckpt_inflight, wal_pending_count, wal_pending_buffered_count, wal_buffered_count;
	u64 wal_buffered_acks, wal_buffered_commits, wal_buffered_errors;
	struct list_head pending_flush_bios, wal_pending;
	struct skiplist *memtable;
	struct zone_pool *zp;
};

struct zns_io_ctx {
	struct zns_base_c *c;
	struct bio *orig_bio;
	sector_t reserved_phys, reserved_nr, phys;
	u64 lba, diag_wal_queued_ns;
	bool is_discard;
	struct list_head wal_link;
};

static bool metrics_enabled, wal_timing_enabled;
static unsigned int wal_batch_max_records = 4, gc_low_watermark = 4, flush_threshold = 100;
static unsigned int wal_group_capacity(void)
{
	return wal_batch_max_records;
}

static int zns_wal_wq, acked, kicked, dispatches, foreground_done, freed, put_error;
static u64 mapped;
#define spin_lock_irq(x) ((void)(x))
#define spin_unlock_irq(x) ((void)(x))
#define INIT_LIST_HEAD(x) ((x)->n = 0)
static bool list_empty(struct list_head *h)
{
	return !h->n;
}

static void list_add_tail(struct list_head *x, struct list_head *h)
{
	(void)x;
	h->n++;
}

static u64 ktime_get_ns(void)
{
	return 1;
}

static unsigned int gc_count_free_zones(struct zone_pool *p)
{
	return p->free;
}

static int mapping_put(struct zns_base_c *c, u64 lba, sector_t phys)
{
	(void)c;
	(void)lba;
	if (put_error)
		return -1;
	mapped = phys;
	return 0;
}

static void mod_delayed_work(int q, int *w, int d)
{
	(void)q;
	(void)w;
	(void)d;
	kicked++;
}

static void zone_dispatch_cancel(struct zns_base_c *c, sector_t p, sector_t n)
{
	(void)c;
	(void)p;
	(void)n;
	dispatches++;
}

static void foreground_write_done(struct zns_base_c *c)
{
	(void)c;
	foreground_done++;
}

static void bio_endio(struct bio *b)
{
	assert(b);
	acked++;
}

static void kfree(void *p)
{
	assert(p);
	freed++;
	free(p);
}

/* wal_try_buffer_write */
/* wal_complete_buffered */

static struct zns_io_ctx *ctx_new(struct zns_base_c *c, struct bio *bio, u64 phys)
{
	struct zns_io_ctx *x = calloc(1, sizeof(*x));

	assert(x);
	x->c = c;
	x->orig_bio = bio;
	x->phys = phys;
	x->reserved_nr = 8;
	return x;
}

int main(void)
{
	struct skiplist mt = {0};
	struct zone_pool zp = {20};
	struct bio bio = {0};
	struct zns_base_c c = {.memtable = &mt, .zp = &zp};
	struct zns_io_ctx *x = ctx_new(&c, &bio, 10), *y;

	/* 명시적 영속화 요청은 기본 경로에서도 절대 조기 ACK하지 않는다. */
	bio.bi_opf = REQ_FUA;
	assert(!wal_try_buffer_write(x));
	bio.bi_opf = REQ_PREFLUSH;
	assert(!wal_try_buffer_write(x));
	bio.bi_opf = 0;
	c.gc_active = true;
	assert(!wal_try_buffer_write(x));
	c.gc_active = false;
	c.wal_rotation_pending = true;
	assert(!wal_try_buffer_write(x));
	c.wal_rotation_pending = false;
	c.wal_ckpt_inflight = 1;
	assert(!wal_try_buffer_write(x));
	c.wal_ckpt_inflight = 0;
	c.pending_flush_bios.n = 1;
	assert(!wal_try_buffer_write(x));
	c.pending_flush_bios.n = 0;
	zp.free = 6;
	assert(!wal_try_buffer_write(x));
	zp.free = 20;
	put_error = 1;
	assert(!wal_try_buffer_write(x));
	put_error = 0;
	assert(!acked && !c.wal_pending_count);
	assert(wal_try_buffer_write(x));
	assert(acked == 1 && dispatches == 1 && foreground_done == 1 && !kicked);
	assert(!x->orig_bio && c.wal_buffered_count == 1 && mapped == 10);

	/* WAL 완료가 더 최신 overwrite를 되돌리거나 두 번째 ACK하면 안 된다. */
	y = ctx_new(&c, &bio, 20);
	assert(wal_try_buffer_write(y));
	assert(mapped == 20 && wal_complete_buffered(x, 0) && mapped == 20 && acked == 2);
	assert(c.wal_buffered_commits == 1);

	/* 모델에서 완료한 첫 레코드의 pending 회계를 제거한다. */
	c.wal_pending_count--;
	c.wal_pending_buffered_count--;
	c.wal_pending.n--;
	x = ctx_new(&c, &bio, 30);
	assert(wal_try_buffer_write(x));
	struct zns_io_ctx *z = ctx_new(&c, &bio, 40);

	assert(wal_try_buffer_write(z));
	struct zns_io_ctx *w = ctx_new(&c, &bio, 50);

	assert(wal_try_buffer_write(w));
	assert(kicked == 1 && c.wal_pending_count == 4);
	struct zns_io_ctx *blocked = ctx_new(&c, &bio, 60);

	assert(!wal_try_buffer_write(blocked));
	assert(!wal_complete_buffered(blocked, 0));
	free(blocked);

	/* 이미 ACK한 일반 쓰기의 비동기 WAL 오류는 후속 작업을 fail-closed. */
	assert(wal_complete_buffered(y, 1));
	assert(c.metadata_failed && c.checkpoint_failed && c.wal_buffered_errors == 1);
	assert(wal_complete_buffered(x, 0));
	assert(wal_complete_buffered(z, 0));
	assert(wal_complete_buffered(w, 0));
	assert(c.wal_buffered_count == 0 && c.wal_buffered_acks == 5 && freed == 5);
	/* buffered-only 기록 중에는 다음 그룹을 모을 수 있다. durable waiter
	 * 그룹과 조기 publication을 교차시키지는 않는다. */
	struct zns_base_c c2 = {.memtable = &mt, .zp = &zp, .wal_batch_busy = true};
	x = ctx_new(&c2, &bio, 70);
	assert(wal_try_buffer_write(x));
	c2.wal_batch_has_waiters = true;
	y = ctx_new(&c2, &bio, 80);
	assert(!wal_try_buffer_write(y));
	free(y);
	assert(wal_complete_buffered(x, 0) && mapped == 70);
	puts("PASS: default/FUA/PREFLUSH, GC/rotation/checkpoint/space guards, early ACK, group full, overwrite, delayed error");
	return 0;
}
