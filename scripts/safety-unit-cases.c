int main(void)
{
	struct skiplist_node a = { .lba = 8, .phys = 101 }, b = { .lba = 16, .phys = 201 };
	struct skiplist_node head = { .forward = {&a} }, frozen_head = { .forward = {&b} };
	struct skiplist active = { &head, 1 }, frozen = { &frozen_head, 1 };
	struct zone_pool zp = { .nr_zones = 4, .zone_sectors = 1024 };
	struct zns_base_c c = { .memtable = &active, .frozen_memtable = &frozen, .zp = &zp };
	u64 phys = 0;

	assert(mapping_get(&c, 16, &phys) && phys == 201); /* frozen 매핑 조회 */
	b.lba = 8;
	assert(mapping_get(&c, 8, &phys) && phys == 101); /* active 매핑 우선 */
	a.phys = UINT64_MAX;
	assert(mapping_get(&c, 8, &phys) && phys == UINT64_MAX); /* tombstone 우선 */
	a.phys = 101;
	assert(gc_memtable_references_zone(&c, 100, 200));
	assert(!gc_memtable_references_zone(&c, 0, 101)); /* 끝 주소는 범위에서 제외 */
	a.phys = MAPPING_TOMBSTONE;
	assert(!gc_memtable_references_zone(&c, 100, 200));
	a.phys = 101;
	release_frozen_memtable(&c, &frozen, false);
	assert(c.frozen_memtable == &frozen && c.metadata_failed && c.checkpoint_failed);
	assert(destroy_count == 0); /* 실패한 flush는 공개 전 데이터를 해제하지 않음 */
	c.frozen_memtable = malloc(sizeof(frozen));
	*c.frozen_memtable = frozen;
	c.frozen_published = true;
	release_frozen_memtable(&c, c.frozen_memtable, true);
	assert(c.frozen_memtable == NULL && destroy_count == 1);
	assert(c.checkpoint_failed); /* 후속 성공으로 이전 실패 상태를 지우지 않음 */

	struct gc_live_map map = {0};

	assert(gc_live_map_update(&map, 8, 1, true) == 0);
	b.lba = 16;
	a.forward[0] = &b;
	active.count = 2;
	assert(gc_refresh_live_map(&c, &map) == 0);
	assert(gc_live_map_find(&map, 8)->phys == 101);
	assert(gc_live_map_find(&map, 16)->phys == 201); /* 새 LBA도 삽입 */
	struct skiplist_node *growth = calloc(65538, sizeof(*growth));

	assert(growth);
	for (unsigned int i = 0; i < 65537; i++)
		growth[i].forward[0] = &growth[i + 1];
	head.forward[0] = growth;
	active.count = 1; /* 개수 확인과 복사 사이에 할당 여유를 넘는 증가를 재현 */
	assert(gc_refresh_live_map(&c, &map) == -EAGAIN);
	head.forward[0] = &a;
	free(growth);
	active.count = 2;
	fail_alloc = true;
	assert(gc_refresh_live_map(&c, &map) == -ENOMEM);
	fail_alloc = false;

	/* 한 GC cycle에 foreground LBA가 늘어나는 상황처럼 해시 확장을 유도한다. */
	for (unsigned int i = 3; i < 1500; i++)
		assert(gc_live_map_update(&map, (u64)i*8, (u64)i*8 + 1, true) == 0);
	assert(gc_live_map_find(&map, 16)->phys == 201);
	free(map.entries);

	struct gc_live_entry e = { .lba = 8, .phys = 101, .used = true };
	struct gc_live_entry *entries[] = { &e };
	put_result = -ENOMEM;
	assert(gc_commit_relocation(&c, entries, 1, 501) == -ENOMEM && e.phys == 101);
	put_result = 1;
	a.phys = 301; /* foreground overwrite가 조건부 갱신보다 우선 */
	assert(gc_commit_relocation(&c, entries, 1, 501) == 0 && e.phys == 301);
	put_result = 0;
	assert(gc_commit_relocation(&c, entries, 1, 501) == 0 && e.phys == 501);

	zp.active_zone[ZONE_TAG_GC_DATA] = ZONE_NONE;
	zp.active_zone[ZONE_TAG_WAL] = ZONE_NONE;
	assert(gc_workspace_required(&c, 800) == 2);
	zp.active_zone[ZONE_TAG_GC_DATA] = 0;
	zp.wp[0] = 100;
	zp.active_zone[ZONE_TAG_WAL] = 1;
	zp.wp[1] = 1000; /* 24섹터 WAL tail에 8섹터 GC 페이지가 들어감 */
	assert(gc_workspace_required(&c, 800) == 0);
	assert(gc_workspace_required(&c, 1000) == 1);
	zp.wp[1] = 1020;
	assert(gc_workspace_required(&c, 800) == 1);
	assert(gc_workspace_required(&c, 1000) == 2);
	zp.wp[1] = 900; /* 124섹터 tail에 현재 8섹터와 다음 victim의 floor는 함께 들어가지 않음 */
	c.gc_wal_reserve = 120;
	assert(gc_wal_tail_needs_roll(&c, 8));
	assert(gc_workspace_required(&c, 800) == 1);
	zp.wp[1] = 880; /* 144섹터 tail에 현재 8섹터와 floor 120섹터가 함께 들어감 */
	assert(!gc_wal_tail_needs_roll(&c, 8));
	assert(gc_workspace_required(&c, 800) == 0);
	c.gc_wal_reserve = 0;

	/* 일반 GC는 data·WAL 순이익을 요구한다. reserve에서는 데이터 여유로 진행을 재개하되 fully-live victim은 거절한다. */
	assert(!gc_reclaim_gain_allowed(1000, 993, 3));
	assert(gc_reclaim_gain_allowed(1000, 993, 2));
	assert(!gc_reclaim_gain_allowed(1000, 1000, 2));
	assert(gc_reclaim_gain_allowed(1000, 900, 3));
	zp.zone_tag[0] = ZONE_TAG_GC_DATA;
	zp.zone_tag[1] = ZONE_TAG_WAL;
	zp.zone_tag[2] = ZONE_TAG_FREE;
	zp.zone_tag[3] = ZONE_TAG_FREE;
	zp.wp[1] = 800; /* seed 최악값 128섹터와 full-victim GC WAL을 감당하는 tail */
	assert(gc_seed_borrow_allowed(&c));
	zp.zone_tag[3] = ZONE_TAG_USER_DATA;
	assert(!gc_seed_borrow_allowed(&c)); /* 마지막 free zone은 seed에 빌려주지 않음 */
	zp.zone_tag[3] = ZONE_TAG_FREE;
	zp.wp[1] = 889; /* WAL 여유 135섹터로는 필요한 136섹터를 감당하지 못함 */
	assert(!gc_seed_borrow_allowed(&c));
	zp.wp[1] = 800;

	/* free=reserve 또는 free=1에서 WAL이 부족하고 강제 스냅샷이 기존 SSTable tail에 들어갈 때만 회전한다. */
	zp.zone_tag[0] = ZONE_TAG_SSTABLE;
	zp.zone_tag[1] = ZONE_TAG_WAL;
	zp.zone_tag[2] = ZONE_TAG_FREE;
	zp.zone_tag[3] = ZONE_TAG_FREE;
	zp.active_zone[ZONE_TAG_SSTABLE] = 0;
	zp.active_zone[ZONE_TAG_WAL] = 1;
	zp.wp[0] = 100;
	zp.wp[1] = 900;
	c.gc_wal_reserve = 120;
	c.wal_rotation_pending = false;
	c.wal_rotation_credit = false;
	assert(gc_wal_rotation_needed(&c));

	/* 100섹터는 GC floor 두 개는 감당하지만 full-victim WAL 16섹터와 seed WAL 128섹터는 감당하지 못한다. */
	c.gc_wal_reserve = 16;
	zp.wp[1] = 924;
	assert(gc_wal_rotation_needed(&c));
	c.gc_wal_reserve = 120;
	zp.wp[1] = 760;
	assert(!gc_wal_rotation_needed(&c)); /* 기존 tail이 충분하면 회전하지 않음 */
	zp.wp[1] = 900;
	zp.zone_tag[3] = ZONE_TAG_USER_DATA;
	assert(gc_wal_rotation_needed(&c)); /* 마지막 free zone으로 WAL 회전 가능 */
	zp.zone_tag[2] = ZONE_TAG_USER_DATA;
	assert(!gc_wal_rotation_needed(&c)); /* 새 WAL을 만들 free zone이 없음 */
	zp.zone_tag[2] = ZONE_TAG_FREE;
	c.wal_rotation_credit = true;
	assert(!gc_wal_rotation_needed(&c));
	c.wal_rotation_credit = false;
	c.wal_rotation_pending = true;
	assert(!gc_wal_rotation_needed(&c));
	c.wal_rotation_pending = false;
	zp.zone_tag[3] = ZONE_TAG_FREE;

	/* 이전 active SSTable zone의 모든 live table을 포함하고 예약이 drain됐을 때만 compaction이 reserve를 차용한다. */
	struct sstable_info compact_victims[4] = {
		 {
			.phys = 10 }, { .phys = 20 }, { .phys = 30 }, { .phys = 40
		}
	};
	zp.active_zone[ZONE_TAG_SSTABLE] = 0;
	zp.zone_tag[0] = ZONE_TAG_SSTABLE;
	zp.zone_tag[1] = ZONE_TAG_WAL;
	zp.zone_tag[2] = ZONE_TAG_FREE;
	zp.zone_tag[3] = ZONE_TAG_USER_DATA;
	zp.wp[0] = 500;
	zp.dispatch_wp[0] = 500;
	zp.sstable_live_count[0] = 4;
	c.wal_ckpt_inflight = 0;
	c.gc_active = false;
	assert(sstable_compaction_can_borrow_reserve(&c, compact_victims, 4, 0));
	zp.sstable_live_count[0] = 5;
	assert(!sstable_compaction_can_borrow_reserve(&c, compact_victims, 4, 0));
	zp.sstable_live_count[0] = 4;
	zp.dispatch_wp[0] = 499;
	assert(!sstable_compaction_can_borrow_reserve(&c, compact_victims, 4, 0));
	zp.dispatch_wp[0] = 500;
	c.wal_ckpt_inflight = 1;
	assert(!sstable_compaction_can_borrow_reserve(&c, compact_victims, 4, 0));
	c.wal_ckpt_inflight = 0;
	c.gc_active = true;
	assert(!sstable_compaction_can_borrow_reserve(&c, compact_victims, 4, 0));
	assert(sstable_compaction_reserve_reclaimable(&c, compact_victims, 4, 0));
	c.gc_active = false;
	sector_t fresh_phys = 0;
	int fresh_zone = -1;

	assert(zone_pool_alloc_fresh(&zp, ZONE_TAG_SSTABLE, 100,
								 &fresh_phys, &fresh_zone, true) == 0);
	assert(fresh_zone == 2 && fresh_phys == 2 * 1024 + 1);
	assert(zp.active_zone[ZONE_TAG_SSTABLE] == 2 && zp.wp[2] == 101);
	assert(zp.wp[0] == 500); /* 이전 active tail은 변경하지 않은 상태 */
	zp.zone_tag[2] = ZONE_TAG_FREE;
	zp.wp[2] = 0;
	zp.active_zone[ZONE_TAG_SSTABLE] = 0;
	struct sstable_info flush_inputs[3] = {
		 {
			.phys = 10, .record_count = 1000
		},
		 {
			.phys = 20, .record_count = 1000
		},
		 {
			.phys = 30, .record_count = 1000
		}
	};
	c.sstables = flush_inputs;
	c.nr_sstables = 3;
	c.wal_ckpt_inflight = 1;
	zp.sstable_live_count[0] = 3;
	assert(sstable_flush_can_borrow_reserve(&c, 1000, 33, 0));
	c.nr_sstables = 2;
	assert(!sstable_flush_can_borrow_reserve(&c, 1000, 33, 0));
	c.nr_sstables = 3;
	flush_inputs[2].phys = 1024 + 30;
	assert(!sstable_flush_can_borrow_reserve(&c, 1000, 33, 0));
	flush_inputs[2].phys = 30;
	c.wal_ckpt_inflight = 2;
	assert(!sstable_flush_can_borrow_reserve(&c, 1000, 33, 0));
	c.wal_ckpt_inflight = 1;
	assert(!sstable_flush_can_borrow_reserve(&c, 40000, 800, 0));
	c.sstables = NULL;
	c.nr_sstables = 0;
	c.wal_ckpt_inflight = 0;
	zp.zone_tag[2] = ZONE_TAG_USER_DATA;
	assert(!sstable_compaction_can_borrow_reserve(&c, compact_victims, 4, 0));
	zp.zone_tag[2] = ZONE_TAG_FREE;
	zp.zone_tag[3] = ZONE_TAG_FREE;

	zp.zone_tag[0] = ZONE_TAG_GC_DATA;
	zp.active_zone[ZONE_TAG_USER_DATA] = ZONE_NONE;
	sector_t shared_phys;
	unsigned int shared_zone;
	zp.active_zone[ZONE_TAG_GC_DATA] = 0;
	zp.wp[0] = 900;
	assert(zone_pool_handoff_gc_tail(&zp, 8, &shared_phys, &shared_zone) == 0);
	assert(shared_zone == 0 && shared_phys == 900 && zp.wp[0] == 908);
	assert(zp.active_zone[ZONE_TAG_GC_DATA] == ZONE_NONE);
	assert(zp.active_zone[ZONE_TAG_USER_DATA] == 0);
	assert(zone_pool_zone_is_active(&zp, 0));
	zp.active_zone[ZONE_TAG_GC_DATA] = 0;
	zp.active_zone[ZONE_TAG_USER_DATA] = ZONE_NONE;
	zp.wp[0] = 1020;
	assert(zone_pool_handoff_gc_tail(&zp, 8, &shared_phys, NULL) == -ENOSPC);
	zp.zone_tag[1] = ZONE_TAG_USER_DATA;
	zp.active_zone[ZONE_TAG_USER_DATA] = ZONE_NONE;
	int new_zone;
	/* reserve 경계에서도 active USER_DATA tail은 추가 zone 없이 사용할 수 있다.
	 * active slot을 비우면 같은 물리 tail이 남아 있어도 동일 요청이 실패해야 한다. */
	zp.wp[1] = 307;
	zp.active_zone[ZONE_TAG_USER_DATA] = 1;
	assert(zone_pool_alloc(&zp, ZONE_TAG_USER_DATA, 8, &phys, &new_zone, false) == 0);
	assert(phys == 1024 + 307 && new_zone == -1 && zp.wp[1] == 315);
	assert(gc_count_free_zones(&zp) == 2);

	zp.active_zone[ZONE_TAG_USER_DATA] = ZONE_NONE;
	assert(zone_pool_alloc(&zp, ZONE_TAG_USER_DATA, 8, &phys, &new_zone, false) == -ENOSPC);
	assert(zp.zone_tag[2] == ZONE_TAG_FREE && zp.zone_tag[3] == ZONE_TAG_FREE);
	zp.active_zone[ZONE_TAG_WAL] = ZONE_NONE;
	assert(zone_pool_alloc(&zp, ZONE_TAG_WAL, 8, &phys, &new_zone, true) == 0);
	c.gc_wal_budget = 1010;
	assert(!foreground_wal_allowed(&c, 8)); /* GC가 보호하는 WAL tail은 소비하지 않음 */
	c.gc_wal_budget = 1000;
	assert(foreground_wal_allowed(&c, 8));
	c.gc_wal_budget = 0;
	assert(foreground_wal_allowed(&c, 8));
	c.gc_wal_reserve = 1000;
	assert(!foreground_wal_allowed(&c, 25));
	assert(foreground_wal_allowed(&c, 15));
	c.gc_wal_reserve = 0;

	/* active miss에서도 오래된 SSTable보다 frozen 매핑이 우선해야 한다. */
	struct fake_device dev = {0};
	struct bio bio = {0};
	c.dev = &dev;
	c.metadata_failed = false;
	c.frozen_memtable = &frozen;
	b.lba = 16;
	b.phys = 801;
	a.forward[0] = NULL;
	struct sstable_read_ctx *r = calloc(1, sizeof(*r));
	r->c = &c;
	r->orig_bio = &bio;
	r->lba = 16;
	r->best_found = 1;
	r->best_phys = 201;
	sstable_read_finish(r);
	assert(submitted == 1 && bio.bi_iter.bi_sector == 801 && pins == 1);
	pins = 0; /* 데이터 읽기 완료를 재현 */

	/* 스냅샷 miss라도 새로 공개된 tombstone을 다시 확인한다. */
	b.phys = MAPPING_TOMBSTONE;
	r = calloc(1, sizeof(*r));
	r->c = &c;
	r->orig_bio = &bio;
	r->lba = 16;
	sstable_read_finish(r);
	assert(zeroed == 1 && completed == 1 && submitted == 1 && pins == 0);

	/* frozen map 공개·해제 이후에는 오래된 SSTable 스냅샷을 다시 만든다. */
	c.frozen_memtable = NULL;
	c.read_view_epoch = 1;
	struct sstable_info si = { .phys = 2048 };
	c.sstables = &si;
	c.nr_sstables = 1;
	r = calloc(1, sizeof(*r));
	r->c = &c;
	r->orig_bio = &bio;
	r->lba = 16;
	r->best_found = 1;
	r->best_phys = 201;
	sstable_read_finish(r);
	assert(restarted == 1 && submitted == 1 && pins == 1);
	assert(r->view_epoch == 1 && !r->best_found);
	free(r->candidates);
	free(r);
	pins = 0;

	/* 스냅샷 재할당 실패를 오래된 데이터 반환으로 숨기지 않는다. */
	r = calloc(1, sizeof(*r));
	r->c = &c;
	r->orig_bio = &bio;
	r->lba = 16;
	r->best_found = 1;
	r->best_phys = 201;
	fail_alloc = true;
	sstable_read_finish(r);
	fail_alloc = false;
	assert(bio.bi_status == BLK_STS_RESOURCE && submitted == 1 && pins == 0);

	/* 읽기 실패는 오래된 SSTable hit/zero-fill로 변환해서는 안 된다. */
	r = calloc(1, sizeof(*r));
	r->c = &c;
	r->orig_bio = &bio;
	r->lba = 16;
	r->best_found = 1;
	r->best_phys = 201;
	r->error = BLK_STS_IOERR;
	sstable_read_finish(r);
	assert(bio.bi_status == BLK_STS_IOERR && submitted == 1 && zeroed == 1 && pins == 0);
	puts("PASS: frozen visibility/failure, new LBA refresh/growth, allocation failure, CAS error, GC workspace/reserve");
	puts("PASS: async read frozen override, tombstone, publication restart and OOM");
	return 0;
}
