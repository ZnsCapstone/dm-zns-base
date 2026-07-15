/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ZNS_SKIPLIST_H
#define ZNS_SKIPLIST_H

#include <linux/types.h>

#define SKIPLIST_MAX_LEVEL 20   // log2(FLUSH_THRESHOLD ~= 1,000,000) 기준 상한

// memtable
struct skiplist_node {
	u64 lba;
	u64 phys;
	unsigned int level;              // 이 노드가 몇 층까지 뻗어있는지
	struct skiplist_node *forward[];  // flexible array member, 크기 = level
};

struct skiplist {
	struct skiplist_node *head;  // sentinel, forward[]는 항상 SKIPLIST_MAX_LEVEL 크기
	unsigned int level;          // 지금까지 실제 쓰인 최대 층수
	unsigned int count;          // 엔트리 개수 (flush 트리거 판단에 씀)
};

/* sl은 호출자가 kzalloc으로 미리 할당해서 넘김. head sentinel만 여기서 준비 */
int skiplist_init(struct skiplist *sl);

/* head를 포함한 모든 노드를 kfree. sl 자체(컨테이너)는 호출자 책임 */
void skiplist_destroy(struct skiplist *sl);

/* skiplist의 random level 선택 */
int skiplist_random_level(void);

/* lba가 이미 있으면 옛 phys를 *old_phys_out에 담고 갱신 후 1 반환.
 * 없으면 새로 삽입하고 0 반환. 호출자가 락을 쥐고 있다고 가정(내부에서 락 안 잡음). */
int skiplist_upsert(struct skiplist *sl, u64 lba, u64 phys, u64 *old_phys_out);

/* lba를 찾아 *phys_out에 담고 1 반환. 없으면 0 반환. */
int skiplist_lookup(struct skiplist *sl, u64 lba, u64 *phys_out);

#endif
