// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/random.h>
#include "skiplist.h"

int skiplist_init(struct skiplist *sl)
{
	/* sentinel은 모든 층의 시작점이다. atomic 경로에서도 초기화하므로 GFP_ATOMIC을 사용한다. */
	sl->head = kzalloc(sizeof(*sl->head) +
			    SKIPLIST_MAX_LEVEL * sizeof(struct skiplist_node *),
			    GFP_ATOMIC);
	if (!sl->head)
		return -ENOMEM;

	sl->head->level = SKIPLIST_MAX_LEVEL;
	sl->level = 1;
	sl->count = 0;
	sl->allocated_bytes = sizeof(*sl) + ksize(sl->head);
	return 0;
}

void skiplist_destroy(struct skiplist *sl)
{
	/* 모든 노드는 0층에 연결되므로 한 번의 순회로 해제할 수 있다. */
	struct skiplist_node *node = sl->head->forward[0];
	struct skiplist_node *next;

	while (node) {
		next = node->forward[0];
		kfree(node);
		node = next;
	}
	kfree(sl->head); /* sl 자체는 호출자가 해제한다. */
}

int skiplist_random_level(void)
{
	/* 각 추가 층을 50% 확률로 선택하되 SKIPLIST_MAX_LEVEL을 넘지 않는다. */
	int level = 1;

	while (level < SKIPLIST_MAX_LEVEL && (get_random_u32() & 1))
		level++;
	return level;
}

int skiplist_upsert(struct skiplist *sl, u64 lba, u64 phys, u64 *old_phys_out)
{
	/* 각 층의 삽입 위치 직전 노드를 저장한다. 호출자가 락으로 보호한다. */
	struct skiplist_node *update[SKIPLIST_MAX_LEVEL];
	struct skiplist_node *x = sl->head;
	struct skiplist_node *found, *n;
	int l, i;
	int new_level;

	/* 위층에서 찾은 위치를 이어받아 아래층의 탐색 범위를 줄인다. */
	for (l = sl->level - 1; l >= 0; l--) {
		while (x->forward[l] && x->forward[l]->lba < lba) {
			x = x->forward[l];
		}
		update[l] = x;
	}

	/* 같은 LBA가 있으면 물리 주소만 갱신한다. */
	found = x->forward[0];
	if (found && found->lba == lba) {
		/* 호출자가 이전 물리 위치를 무효화할 수 있도록 옛 주소를 반환한다. */
		*old_phys_out = found->phys;
		found->phys = phys;
		return 1; /* 이미 존재하는 키 — 갱신됨 */
	}

	new_level = skiplist_random_level();

	/* 기존 최고층보다 높은 층은 sentinel 뒤에 삽입한다. */
	if (new_level > sl->level) {
		for (i = sl->level; i < new_level; i++) {
			update[i] = sl->head;
		}
		sl->level = new_level;
	}

	/* forward[] 크기만큼 포인터 공간을 추가한다.
	 * mapping 갱신은 spinlock/완료 콜백에서도 실행되므로 sleep 가능한 할당은 금지한다. */
	n = kzalloc(sizeof(*n) + new_level * sizeof(struct skiplist_node *), GFP_ATOMIC);
	if (!n)
		return -ENOMEM;

	n->lba = lba;
	n->phys = phys;
	n->level = new_level;

	/* 각 층에서 직전 노드와 다음 노드 사이에 연결한다. */
	for (i = 0; i < new_level; i++) {
		n->forward[i] = update[i]->forward[i];
		update[i]->forward[i] = n;
	}

	sl->count++;
	sl->allocated_bytes += ksize(n);
	return 0; /* 신규 삽입됨 */
}

/* 호출자가 접근을 보호한다. 조회 중에는 매핑을 변경하지 않는다. */
int skiplist_lookup(struct skiplist *sl, u64 lba, u64 *phys_out)
{
	struct skiplist_node *x = sl->head;
	struct skiplist_node *found;
	int l;

	for (l = SKIPLIST_MAX_LEVEL - 1; l >= 0; l--) {
		while (x->forward[l] && x->forward[l]->lba < lba)
			x = x->forward[l];
	}

	found = x->forward[0];
	if (found && found->lba == lba) {
		*phys_out = found->phys;
		return 1;
	}
	return 0;
}
