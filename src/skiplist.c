// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/random.h>
#include "skiplist.h"

int skiplist_init(struct skiplist *sl)
{
	/* head는 실제 데이터를 담지 않는 sentinel 노드. 모든 층에서 "시작점" 역할을
	 *하므로 있을 수 있는 최대 층수(SKIPLIST_MAX_LEVEL)만큼 forward[]를 항상
	 * 다 갖고 있어야 한다 — 나중에 어떤 노드가 몇 층으로 뽑히든 head는 그 층의
	 * 시작점으로 바로 쓸 수 있어야 하기 때문. kzalloc이라 forward[]는 전부
	 * NULL로 시작 = 아직 어느 층에도 데이터 노드가 없는 빈 리스트 상태. */
	sl->head = kzalloc(sizeof(*sl->head) +
			    SKIPLIST_MAX_LEVEL * sizeof(struct skiplist_node *),
			    GFP_KERNEL);
	if (!sl->head)
		return -ENOMEM;

	sl->head->level = SKIPLIST_MAX_LEVEL;  /* head 자신의 forward[] 크기 (고정값) */
	sl->level = 1;   /* "지금 실제로 쓰이는" 최고 층수. 데이터가 없으니 최소값 1부터 */
	sl->count = 0;   /* 엔트리 개수. memtable flush 트리거 판단에 쓰임 */
	return 0;
}

void skiplist_destroy(struct skiplist *sl)
{
	/* 0층(forward[0])은 모든 노드를 빠짐없이 정렬 순서로 잇고 있는 유일한 층이라,
	 * 이거 하나만 따라가면 몇 층짜리든 상관없이 전체 노드를 다 방문할 수 있다.
	 * (1층, 2층... 을 따로 순회할 필요가 없음 — 거긴 일부 노드만 골라서 도는 지름길일 뿐) */
	struct skiplist_node *node = sl->head->forward[0];
	struct skiplist_node *next;

	while (node) {
		next = node->forward[0];  /* node를 kfree하기 전에 다음 노드 주소부터 챙겨둠 */
		kfree(node);
		node = next;
	}
	kfree(sl->head);  /* 마지막으로 sentinel 자신도 해제. sl 컨테이너 자체는 호출자 책임 */
}

int skiplist_random_level(void)
{
	/* 동전 던지기: 50% 확률로 한 층씩 계속 올라간다. 그러니 결과 분포는
	 * level=1일 확률 1/2, level=2일 확률 1/4, level=3일 확률 1/8 ... 식으로
	 * 절반씩 줄어든다 — 대부분 노드가 낮은 층에 머물고, 아주 일부만 위층까지
	 * 뻗는 게 skip list가 노리는 모양 그대로다. SKIPLIST_MAX_LEVEL에서 강제로 멈춘다
	 * (안 그러면 이론상 무한정 올라갈 수 있음, 확률은 갈수록 0에 수렴하지만). */
	int level = 1;

	while (level < SKIPLIST_MAX_LEVEL && (get_random_u32() & 1))
		level++;
	return level;
}

int skiplist_upsert(struct skiplist *sl, u64 lba, u64 phys, u64 *old_phys_out)
{
	/* update[l] = "l층에서, 삽입/갱신 지점 바로 앞 노드".
	 * 검색하면서 지나온 위치를 기억해두는 용도 — 나중에 새 노드를 끼워넣을 때
	 * "누구 뒤에 이어붙여야 하는지"를 다시 찾지 않고 바로 쓸 수 있게 해준다. */
	struct skiplist_node *update[SKIPLIST_MAX_LEVEL];
	struct skiplist_node *x = sl->head;
	struct skiplist_node *found, *n;
	int l, i;
	int new_level;

	/* --- 1. 검색: 위층(sl->level-1)부터 0층까지 내려오면서 lba 바로 앞자리를 찾는다 ---
	 * 각 층에서 "다음 노드가 lba보다 작은 동안"만 전진하고, lba 이상인 노드를
	 * 만나면(혹은 그 층 끝이면) 멈추고 한 층 내려간다. x는 층이 바뀌어도
	 * head로 되돌리지 않고 그대로 들고 내려간다 — 위층에서 이미 확인한 구간은
	 * 아래층에서 다시 훑을 필요가 없기 때문(이게 O(log n)의 핵심). */
	for (l = sl->level - 1; l >= 0; l--) {
		while (x->forward[l] && x->forward[l]->lba < lba) {
			x = x->forward[l];
		}
		update[l] = x; // 삽입되거나 갱신될 자리
	}

	/* --- 2. 판정: 0층에서 x 바로 다음 노드가 lba와 같은 키인가? ---
	 * 위 검색이 끝난 시점의 x는 항상 "lba보다 작은 값 중 가장 큰 노드"이므로,
	 * x->forward[0]이 lba와 같다면 그게 바로 우리가 찾던 기존 엔트리다. */
	found = x->forward[0];
	if (found && found->lba == lba) {
		/* update(갱신) 경로: 새 노드를 만들 필요 없이 그 자리에서 phys만 교체.
		 * 옛 phys를 호출자에게 돌려주는 이유 — 이 값이 가리키던 물리 위치는
		 * 이 순간부터 아무도 안 쓰는 죽은 공간이 되므로, GC의 zone별
		 * invalid_count를 갱신하려면 호출자(mapping_put)가 이 옛 값을 알아야 함. */
		*old_phys_out = found->phys;
		found->phys = phys;
		return 1; // 이미 존재하는 키 — 갱신됨
	}

	/* --- 3. insert(신규삽입) 경로: 새 노드가 몇 층짜리가 될지 랜덤으로 결정 --- */
	new_level = skiplist_random_level();

	/* 새로 뽑힌 층수가 지금까지 리스트 전체의 최고층(sl->level)보다 높다면,
	 * 그 초과분 층들은 위 1번 검색 루프가 아예 순회하지 않았으므로 update[]가
	 * 비어있다(스택 배열이라 쓰레기값). 이 층들에선 지금까지 아무 노드도 없었다는
	 * 뜻이니, "그 층의 시작점"은 항상 head다 — 그래서 head로 채워준다. */
	if (new_level > sl->level) {
		for (i = sl->level; i < new_level; i++) {
			update[i] = sl->head;
		}
		sl->level = new_level;  /* 리스트 전체의 최고층 갱신 */
	}

	/* forward[]는 "노드"가 아니라 "포인터"의 배열이므로 sizeof(struct skiplist_node *)
	 * 여야 한다. sizeof(struct skiplist_node)로 하면 노드 하나 크기만큼(포인터의
	 * 몇 배) 잡아서 매번 불필요하게 큰 메모리를 낭비하게 된다. */
	n = kzalloc(sizeof(*n) + new_level * sizeof(struct skiplist_node *), GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	n->lba = lba;
	n->phys = phys;
	n->level = new_level;  /* 이 노드가 몇 층까지 뻗어있는지 기록 (destroy 등에서 참고 가능) */

	/* 새 노드를 0층부터 new_level-1층까지 전부 끼워넣는다. 각 층 i에서:
	 * "새 노드의 forward[i]는 update[i]가 원래 가리키던 다음 노드"로,
	 * "update[i]의 forward[i]는 새 노드"로 — 즉 update[i]와 그 다음 노드
	 * 사이에 새 노드를 끼워넣는 표준적인 연결리스트 삽입이다. */
	for (i = 0; i < new_level; i++) {
		n->forward[i] = update[i]->forward[i];
		update[i]->forward[i] = n;
	}

	sl->count++;  /* 실제로 새 엔트리가 늘어난 경우에만 증가 (갱신 경로는 여기 안 옴) */
	return 0;     // 신규 삽입됨
}

/* TODO(직접 구현): upsert의 "1. 검색"과 완전히 같은 top-down 순회를 하되,
 * update[]도 필요 없고(삽입 안 하니까) 찾았을 때 갱신도 안 함 — 조회만.
 * x->forward[0]이 lba와 같으면 *phys_out에 담고 1, 아니면 0. */
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
