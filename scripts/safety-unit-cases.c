int main(void) {
    struct skiplist_node a = { .lba=8, .phys=101 }, b = { .lba=16, .phys=201 };
    struct skiplist_node head = { .forward={&a} }, frozen_head = { .forward={&b} };
    struct skiplist active = { &head, 1 }, frozen = { &frozen_head, 1 };
    struct zone_pool zp = { .nr_zones=4, .zone_sectors=1024 };
    struct zns_base_c c = { .memtable=&active, .frozen_memtable=&frozen, .zp=&zp };
    u64 phys = 0;
    assert(mapping_get(&c, 16, &phys) && phys == 201); /* frozen read */
    b.lba = 8;
    assert(mapping_get(&c, 8, &phys) && phys == 101); /* active wins */
    a.phys = UINT64_MAX;
    assert(mapping_get(&c, 8, &phys) && phys == UINT64_MAX); /* tombstone wins */
    a.phys = 101;
    assert(gc_memtable_references_zone(&c, 100, 200));
    assert(!gc_memtable_references_zone(&c, 0, 101)); /* end is exclusive */
    a.phys = MAPPING_TOMBSTONE;
    assert(!gc_memtable_references_zone(&c, 100, 200));
    a.phys = 101;
    release_frozen_memtable(&c, &frozen, false);
    assert(c.frozen_memtable == &frozen && c.metadata_failed && c.checkpoint_failed);
    assert(destroy_count == 0); /* failed flush never frees unpublished data */
    c.frozen_memtable = malloc(sizeof(frozen));
    *c.frozen_memtable = frozen;
    c.frozen_published = true;
    release_frozen_memtable(&c, c.frozen_memtable, true);
    assert(c.frozen_memtable == NULL && destroy_count == 1);
    assert(c.checkpoint_failed); /* later success cannot erase earlier failure */

    struct gc_live_map map = {0};
    assert(gc_live_map_update(&map, 8, 1, true) == 0);
    b.lba = 16;
    a.forward[0] = &b;
    active.count = 2;
    assert(gc_refresh_live_map(&c, &map) == 0);
    assert(gc_live_map_find(&map, 8)->phys == 101);
    assert(gc_live_map_find(&map, 16)->phys == 201); /* new LBA inserted */
    struct skiplist_node *growth = calloc(65538, sizeof(*growth));
    assert(growth);
    for (unsigned int i=0; i<65537; i++) growth[i].forward[0] = &growth[i+1];
    head.forward[0] = growth;
    active.count = 1; /* simulate count/copy growth beyond allocation slack */
    assert(gc_refresh_live_map(&c, &map) == -EAGAIN);
    head.forward[0] = &a;
    free(growth);
    active.count = 2;
    fail_alloc = true;
    assert(gc_refresh_live_map(&c, &map) == -ENOMEM);
    fail_alloc = false;
    /* Force hash growth, like many new foreground LBAs in one GC cycle. */
    for (unsigned int i=3; i<1500; i++)
        assert(gc_live_map_update(&map, (u64)i*8, (u64)i*8+1, true) == 0);
    assert(gc_live_map_find(&map, 16)->phys == 201);
    free(map.entries);

    struct gc_live_entry e = { .lba=8, .phys=101, .used=true };
    struct gc_live_entry *entries[] = { &e };
    put_result = -ENOMEM;
    assert(gc_commit_relocation(&c, entries, 1, 501) == -ENOMEM && e.phys == 101);
    put_result = 1;
    a.phys = 301; /* foreground overwrite wins CAS */
    assert(gc_commit_relocation(&c, entries, 1, 501) == 0 && e.phys == 301);
    put_result = 0;
    assert(gc_commit_relocation(&c, entries, 1, 501) == 0 && e.phys == 501);

    zp.active_zone[ZONE_TAG_GC_DATA] = ZONE_NONE;
    zp.active_zone[ZONE_TAG_WAL] = ZONE_NONE;
    assert(gc_workspace_required(&c, 800) == 2);
    zp.active_zone[ZONE_TAG_GC_DATA] = 0;
    zp.wp[0] = 100;
    zp.active_zone[ZONE_TAG_WAL] = 1;
    zp.wp[1] = 1000; /* 24-sector WAL tail covers one 8-sector GC page. */
    assert(gc_workspace_required(&c, 800) == 0);
    assert(gc_workspace_required(&c, 1000) == 1);
    zp.wp[1] = 1020;
    assert(gc_workspace_required(&c, 800) == 1);
    assert(gc_workspace_required(&c, 1000) == 2);
    zp.wp[1] = 900; /* 124-sector tail: current 8 fits, next-victim floor does not. */
    c.gc_wal_reserve = 120;
    assert(gc_wal_tail_needs_roll(&c, 8));
    assert(gc_workspace_required(&c, 800) == 1);
    zp.wp[1] = 880; /* 144-sector tail covers current 8 + floor 120. */
    assert(!gc_wal_tail_needs_roll(&c, 8));
    assert(gc_workspace_required(&c, 800) == 0);
    c.gc_wal_reserve = 0;
    /* Normal GC requires data+WAL net gain. At reserve, a real data tail can
     * bootstrap foreground progress, but a fully-live victim never can. */
    assert(!gc_reclaim_gain_allowed(1000, 993, 3));
    assert(gc_reclaim_gain_allowed(1000, 993, 2));
    assert(!gc_reclaim_gain_allowed(1000, 1000, 2));
    assert(gc_reclaim_gain_allowed(1000, 900, 3));
    zp.zone_tag[0] = ZONE_TAG_GC_DATA;
    zp.zone_tag[1] = ZONE_TAG_WAL;
    zp.zone_tag[2] = ZONE_TAG_FREE;
    zp.zone_tag[3] = ZONE_TAG_FREE;
    zp.wp[1] = 800; /* Seed worst-case 128 + full-victim GC WAL 8. */
    assert(gc_seed_borrow_allowed(&c));
    zp.zone_tag[3] = ZONE_TAG_USER_DATA;
    assert(!gc_seed_borrow_allowed(&c)); /* never borrow the final free zone */
    zp.zone_tag[3] = ZONE_TAG_FREE;
    zp.wp[1] = 889; /* only 135 WAL sectors remain; 136 are required */
    assert(!gc_seed_borrow_allowed(&c));
    zp.wp[1] = 800;

    /* At free=reserve or free=1, rotate only when the WAL cannot cover one
     * full victim plus the following victim floor and the forced mapping
     * snapshot fits in the existing SSTable tail. */
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
    /* 100 sectors covers two 16-sector GC floors, but not the exact
     * 16-sector full-victim GC WAL plus 128-sector foreground seed WAL. */
    c.gc_wal_reserve = 16;
    zp.wp[1] = 924;
    assert(gc_wal_rotation_needed(&c));
    c.gc_wal_reserve = 120;
    zp.wp[1] = 760;
    assert(!gc_wal_rotation_needed(&c)); /* ample old tail: no churn */
    zp.wp[1] = 900;
    zp.zone_tag[3] = ZONE_TAG_USER_DATA;
    assert(gc_wal_rotation_needed(&c)); /* final free zone can rotate WAL */
    zp.zone_tag[2] = ZONE_TAG_USER_DATA;
    assert(!gc_wal_rotation_needed(&c)); /* no free zone to create WAL */
    zp.zone_tag[2] = ZONE_TAG_FREE;
    c.wal_rotation_credit = true;
    assert(!gc_wal_rotation_needed(&c));
    c.wal_rotation_credit = false;
    c.wal_rotation_pending = true;
    assert(!gc_wal_rotation_needed(&c));
    c.wal_rotation_pending = false;
    zp.zone_tag[3] = ZONE_TAG_FREE;

    /* A compaction may borrow a reserve zone only when it covers every live
     * table in the drained old active SSTable zone. */
    struct sstable_info compact_victims[4] = {
        { .phys = 10 }, { .phys = 20 }, { .phys = 30 }, { .phys = 40 }
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
    assert(zp.wp[0] == 500); /* old active tail was deliberately untouched */
    zp.zone_tag[2] = ZONE_TAG_FREE;
    zp.wp[2] = 0;
    zp.active_zone[ZONE_TAG_SSTABLE] = 0;
    struct sstable_info flush_inputs[3] = {
        { .phys = 10, .record_count = 1000 },
        { .phys = 20, .record_count = 1000 },
        { .phys = 30, .record_count = 1000 }
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
    /* Same reserve boundary, with a still-active partial USER_DATA zone:
     * existing tail is usable without consuming either reserved zone.
     * Clearing active (the former early-seal action) makes the same request
     * fail despite the unchanged physical tail. */
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
    assert(!foreground_wal_allowed(&c, 8)); /* GC needs the shared WAL tail */
    c.gc_wal_budget = 1000;
    assert(foreground_wal_allowed(&c, 8));
    c.gc_wal_budget = 0;
    assert(foreground_wal_allowed(&c, 8));
    c.gc_wal_reserve = 1000;
    assert(!foreground_wal_allowed(&c, 25));
    assert(foreground_wal_allowed(&c, 15));
    c.gc_wal_reserve = 0;
    /* An old SSTable hit must lose to a frozen map even if active misses. */
    struct fake_device dev = {0};
    struct bio bio = {0};
    c.dev = &dev;
    c.metadata_failed = false;
    c.frozen_memtable = &frozen;
    b.lba = 16;
    b.phys = 801;
    a.forward[0] = NULL;
    struct sstable_read_ctx *r = calloc(1, sizeof(*r));
    r->c = &c; r->orig_bio = &bio; r->lba = 16;
    r->best_found = 1; r->best_phys = 201;
    sstable_read_finish(r);
    assert(submitted == 1 && bio.bi_iter.bi_sector == 801 && pins == 1);
    pins = 0; /* simulate data read completion */
    /* Even a snapshot miss must recheck a newly visible tombstone. */
    b.phys = MAPPING_TOMBSTONE;
    r = calloc(1, sizeof(*r));
    r->c = &c; r->orig_bio = &bio; r->lba = 16;
    sstable_read_finish(r);
    assert(zeroed == 1 && completed == 1 && submitted == 1 && pins == 0);
    /* Frozen map released after publication: restart stale SSTable snapshot. */
    c.frozen_memtable = NULL;
    c.read_view_epoch = 1;
    struct sstable_info si = { .phys = 2048 };
    c.sstables = &si; c.nr_sstables = 1;
    r = calloc(1, sizeof(*r));
    r->c = &c; r->orig_bio = &bio; r->lba = 16;
    r->best_found = 1; r->best_phys = 201;
    sstable_read_finish(r);
    assert(restarted == 1 && submitted == 1 && pins == 1);
    assert(r->view_epoch == 1 && !r->best_found);
    free(r->candidates); free(r); pins = 0;
    /* Failure to allocate a refreshed snapshot must not return old data. */
    r = calloc(1, sizeof(*r));
    r->c = &c; r->orig_bio = &bio; r->lba = 16;
    r->best_found = 1; r->best_phys = 201;
    fail_alloc = true;
    sstable_read_finish(r);
    fail_alloc = false;
    assert(bio.bi_status == BLK_STS_RESOURCE && submitted == 1 && pins == 0);
    puts("PASS: frozen visibility/failure, new LBA refresh/growth, allocation failure, CAS error, GC workspace/reserve");
    puts("PASS: async read frozen override, tombstone, publication restart and OOM");
    return 0;
}
