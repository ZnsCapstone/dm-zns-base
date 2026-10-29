/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ZNS_SKIPLIST_H
#define ZNS_SKIPLIST_H

#include <linux/types.h>

#define SKIPLIST_MAX_LEVEL 20   /* log2(FLUSH_THRESHOLD ~= 1,000,000) 기준 상한 */

/* 외부 락으로 보호하는 LBA 순서의 memtable이다. */
struct skiplist_node {
	u64 lba;
	u64 phys;
	unsigned int level; /* 노드가 가진 층수 */
	struct skiplist_node *forward[]; /* level개 포인터를 가진 가변 배열 */
};

struct skiplist {
	struct skiplist_node *head; /* sentinel, forward[]는 항상 SKIPLIST_MAX_LEVEL 크기 */
	unsigned int level; /* 현재 리스트의 최고 층수 */
	unsigned int count; /* 서로 다른 LBA의 개수 */
	u64 allocated_bytes; /* 컨테이너와 실제 slab 할당량의 합. 호출자가 락으로 보호한다. */
};

/* 호출자가 할당한 sl에 sentinel과 초기 상태를 준비한다. 실패 시 -ENOMEM을 반환한다. */
int skiplist_init(struct skiplist *sl);

/* 초기화된 리스트의 sentinel과 노드를 해제한다. sl 자체는 호출자가 해제한다. */
void skiplist_destroy(struct skiplist *sl);

/* 1부터 SKIPLIST_MAX_LEVEL 사이의 층수를 확률적으로 선택한다. */
int skiplist_random_level(void);

/* 기존 LBA는 옛 주소를 *old_phys_out에 저장하고 1, 신규 삽입은 0을 반환한다.
 * 할당 실패는 -ENOMEM이다. 내부 락은 없으므로 호출자가 접근을 직렬화한다. */
int skiplist_upsert(struct skiplist *sl, u64 lba, u64 phys, u64 *old_phys_out);

/* LBA를 찾으면 *phys_out에 주소를 저장하고 1, 없으면 0을 반환한다. */
int skiplist_lookup(struct skiplist *sl, u64 lba, u64 *phys_out);

#endif
