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
    assert(gc_workspace_required(&c, 800) == 2);
    zp.active_zone[ZONE_TAG_GC_DATA] = 0;
    zp.wp[0] = 100;
    assert(gc_workspace_required(&c, 800) == 1);
    assert(gc_workspace_required(&c, 1000) == 2);
    zp.zone_tag[0] = ZONE_TAG_GC_DATA;
    zp.zone_tag[1] = ZONE_TAG_USER_DATA;
    zp.active_zone[ZONE_TAG_USER_DATA] = ZONE_NONE;
    int new_zone;
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
