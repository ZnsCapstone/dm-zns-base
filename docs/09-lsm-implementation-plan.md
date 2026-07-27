# 09. LSM-tree 매핑 테이블 구현 순서

전체 구조와 각 구조체가 왜 필요한지는 `docs/10-lsm-tree-architecture.md` 먼저 참고. 이 문서는 `CLAUDE.md`에 정리된 설계 결정을 실제 구현 순서로 풀어낸 것. 각 단계는 이전 단계가 동작해야 다음 단계로 넘어갈 수 있도록 의존성 순서대로 배치했다. 각 단계 끝의 "확인"을 통과한 뒤 다음으로 넘어갈 것 — 한 번에 다 짜고 마지막에 몰아서 디버깅하지 않기 위함.

설계 근거(왜 이렇게 정했는지)는 여기 반복하지 않는다. `CLAUDE.md` 참고. 버그별 원인/해결 이력은 `report/bugfix-log.md`에 누적 기록 중.

---

## 진행 상황 (2026-07-27 기준)

| 단계 | 상태 |
|---|---|
| 0단계 공통 자료구조 | ✅ 완료 |
| 1단계 zone pool(태그 기반) | ✅ 완료 |
| 2단계 skip list memtable | ✅ 완료 |
| 3단계 mapping_put/get | ✅ 완료 |
| 4단계 WAL append | ✅ 완료 |
| 5단계 WAL replay | ✅ 완료 |
| 6단계 SSTable flush | ✅ 완료 |
| 7단계 WAL checkpoint | ✅ 완료 |
| 8단계 SSTable 읽기 경로 | ✅ 완료 |
| 9단계 compaction | ✅ 완료 (zone 발행 순서 게이트 동반 수정 — 5번째 사건, `report/bugfix-log.md` 참고) |
| 10단계 GC | ✅ 완료 (GC 로직 정확성 확정 — `test-gc.sh` 50% 시나리오 통과). 단, M3 원 기준인 80%는 WAL zone 회수(13단계) 전까지 산술적으로 도달 불가 — 아래 10단계 "확인" 참고 |
| 11단계 통합 회귀 테스트 | ⬜ 미착수 |
| 12단계 zone 용량 초과 방어(신규) | ⬜ 미착수 |
| 13단계 WAL zone 회수(신규) | ⬜ 미착수 — **M3 성공 기준(80%) 달성의 전제조건** |

---

## 0단계 — 공통 자료구조 스켈레톤

- 레코드 구조체 정의만 먼저: SSTable 레코드(16B, `u64 lba, u64 phys`), WAL 레코드(32B, `type/reserved/a/b`).
- zone 태그 enum 정의: `ZONE_TAG_USER_DATA`, `ZONE_TAG_GC_DATA`, `ZONE_TAG_WAL`, `ZONE_TAG_SSTABLE` (혹은 필요에 맞게).
- `invalid_count[nr_zones]` 배열 자리만 확보 (아직 갱신 로직 없음).

**확인**: 컴파일만 통과하면 됨. 아직 기능 없음.

---

## 1단계 — Zone pool 관리 (태그 기반 할당)

M1은 `active_zone` 하나로 순차 append만 했는데, 이제 여러 역할(사용자 쓰기, GC 재배치, WAL, SSTable)이 zone을 나눠 써야 한다.

- zone 할당자: "태그 T에 맞는 활성 zone을 하나 내놓는다 / 꽉 차면 다음 zone으로 넘어간다 / 완전히 빈 zone 목록에서 새로 배정한다"는 기존 M1의 zone-full 처리 로직을 태그별로 일반화.
- 최소 4개의 활성 zone 슬롯: 사용자 쓰기용, GC 재배치용, WAL용, SSTable용. (SSTable/WAL도 "데이터 zone과 같은 pool 공유 + 태그 구분"이라 별도 전용 zone이 영구히 있는 게 아니라, 그때그때 pool에서 태그를 붙여 할당받는 구조.)
- zone reset 함수 (compaction/GC가 다 쓴 zone을 돌려줄 때 사용).

**확인**: 태그별로 zone을 요청 → 순차 append 시뮬레이션 → 꽉 차면 다음 zone 배정까지 유닛 테스트 수준으로 확인. 아직 매핑 테이블과 연결 안 해도 됨.

---

## 2단계 — Skip list (memtable), 1단계: 정확성

- 커널 표준 구현이 없으므로 직접 작성. 노드: `{lba, phys, level, forward[]}`.
- 삽입/조회/삭제(없음, upsert만) — **upsert는 기존 키가 있으면 옛 값을 반환**해야 함 (3단계에서 zone liveness에 씀).
- 정렬된 순서로 순회하는 iterator (SSTable flush에서 씀).
- 동시성은 기존 M1의 전역 `spinlock_t` 그대로 재사용 — 락-프리 버전은 여기서 안 함 (별도 후속 과제).

**확인**: 커널 모듈 밖 유저스페이스 테스트 하네스로 삽입/조회/순회가 정렬 순서대로 맞는지 검증 (커널에 안 올리고도 로직 검증 가능하면 더 빠름).

---

## 3단계 — `mapping_put`/`mapping_get` (memtable-only)

WAL/SSTable 없이 memtable만으로 매핑 테이블 완성 — M1의 `map[]` flat array를 skip list로 1:1 교체한 상태.

```
mapping_put(lba, phys):
    old = skiplist_upsert(memtable, lba, phys)
    if old: invalid_count[zone_of(old)]++     // 1단계 남겨둔 자리 여기서 채움
mapping_get(lba):
    return skiplist_lookup(memtable, lba)
```

- `.map()`을 M1의 `map[]` 대신 이 두 함수를 쓰도록 교체.
- **엔트리는 전부 고정 크기(`BLOCK_SECTORS`=4KB)라고 가정** — 테이블에 크기 필드가 없다.
  `ti->max_io_len`(ctr) + `dm_accept_partial_bio`(map)가 bio를 애초에 블록 하나
  이하로 자르므로, `mapping_put` 시점엔 항상 "어떤 4KB 블록 전체"라고 보장된다.
  이 전제는 관찰(ext4가 항상 4KB 정렬로 씀)에 기댄 것이지 코드가 독립적으로
  검증하진 않는다 — 정렬 안 된 쓰기가 실제로 들어오면 깨짐(상세: CLAUDE.md
  "매핑은 반드시 블록 정렬 키로 조회해야 함").
- **offset은 테이블 안에 없고, 조회 후에 매번 계산해서 더한다**: 읽기 키는
  항상 `block_lba = (lba/BLOCK_SECTORS)*BLOCK_SECTORS`로 내림한 값이고,
  `mapping_get`이 돌려준 `phys`에 `offset_in_block = lba - block_lba`를
  나중에 더해서 실제 응답 위치를 만든다 — 테이블은 "블록 시작점 → 블록
  시작점"만 알고 있다.

**확인**: `scripts/test-m1.sh`, `scripts/test-m2.sh`를 그대로 돌려서 통과하는지 확인 — flat array와 동작이 동일해야 함. 여기서 회귀가 생기면 이후 단계(WAL/SSTable)를 붙이기 전에 반드시 고칠 것.

---

## 4단계 — WAL append (replay 없이, append만)

- WAL PUT 레코드를 zone에 비동기로 append하는 경로만 우선 구현.
- `.map()` 쓰기 경로를 `DM_MAPIO_REMAPPED` → `DM_MAPIO_SUBMITTED`로 변경.
- 콜백 체인 중 `wal_put_done`까지만 (memtable insert 포함), flush/checkpoint는 아직 연결 안 함.

**확인**: `test-m1.sh`/`test-m2.sh` 재실행 — 이번엔 매 쓰기마다 WAL append가 섞여도 결과가 같아야 함. `blkzone report`로 WAL용 zone의 wp가 실제로 전진하는지 확인.

---

## 5단계 — WAL replay (모듈 로드 시 재구성)

- 모듈 insmod 시 WAL zone(들)을 스캔해서 마지막 CHECKPOINT 이후 PUT 레코드를 memtable에 재생.
- 아직 SSTable이 없으므로 이 시점엔 "체크포인트 없음 = 전부 재생"이 기본 케이스.

**확인**: 새 테스트 스크립트 필요 — insmod → 쓰기 몇 개 → **정상적인 rmmod 없이** (혹은 강제로) 모듈 제거/재삽입 시뮬레이션 → 재삽입 후 이전에 쓴 데이터가 여전히 읽히는지 확인. (`scripts/test-crash.sh` 정도로 새로 만들 것.)

---

## 6단계 — SSTable flush

- memtable 엔트리 수가 `FLUSH_THRESHOLD`(~1,000,000) 도달 시 트리거.
- skip list를 정렬 순서로 순회하며 헤더(`magic, seq_no, record_count, min_lba, max_lba`) + 레코드들을 SSTable zone에 순차 기록.
- flush 완료 후 새 memtable로 교체, `seq_no` 증가.

**확인**: `FLUSH_THRESHOLD`를 테스트용으로 작게(예: 1000) 낮춰서 flush가 실제로 발생하는지, SSTable zone에 헤더+레코드가 예상대로 쓰였는지 확인 (raw 덤프해서 눈으로 확인해도 됨).

---

## 7단계 — WAL CHECKPOINT 연동

- SSTable flush 완료 후 WAL에 CHECKPOINT(seq_no) 기록.
- WAL replay 로직을 "마지막 CHECKPOINT 이후만 재생"하도록 갱신 (5단계에서 만든 replay를 확장).

**확인**: 5단계의 crash-recovery 테스트를 다시 돌리되, 이번엔 flush가 여러 번 일어날 만큼 데이터를 쓴 뒤 크래시 시뮬레이션 → replay 시간이 "전체 재생"보다 짧아졌는지(체크포인트 이후만 재생하는지) 로그로 확인.

---

## 8단계 — SSTable 읽기 경로

- `mapping_get`을 memtable-only에서 memtable → 최신 SSTable → ... 순서로 확장.
- 각 SSTable: `min_lba`/`max_lba`로 먼저 걸러내고, 범위 안이면 binary search.
- 여러 SSTable에 같은 LBA가 있으면 `seq_no` 최댓값 우선.

**확인**: SSTable이 2개 이상 쌓인 상태에서(compaction 트리거 전) 같은 LBA를 두 번 다른 값으로 덮어써서 flush되게 만든 뒤, 읽었을 때 최신 값이 나오는지 확인.

---

## 9단계 — Compaction

- SSTable 개수 K=4 도달 시 백그라운드 workqueue에 큐잉.
- 가장 오래된 K개를 k-way merge → 같은 LBA는 `seq_no` 최댓값만 남김 → 새 SSTable로 기록.
- 새 SSTable이 완전히 durable하게 쓰인 뒤에만 옛 K개의 zone reset.
- merge 중 버려지는 entry의 옛 `phys` → `invalid_count[zone_of(phys)]++`.

**확인**: SSTable이 5개 이상 쌓이도록 충분히 쓰기 → compaction이 자동으로 돌아 SSTable 개수가 다시 줄어드는지, 읽기 결과가 compaction 전후로 동일한지 확인.

---

## 10단계 — GC (zone 재활용)  🚧 검증 중

- victim 선정: greedy — 닫힌(더 이상 활성 zone이 아닌) **데이터 zone**(`USER_DATA` + `GC_DATA` 둘 다 — GC가 재배치한 "차가운" 데이터도 나중에 다시 덮어써지면 죽은 공간이 생기므로) 중 `invalid_count`가 가장 큰 것. `invalid_count == 0`인 zone은 회수해도 이득이 없으므로 후보에서 제외(재배치=zone 하나 비우고 하나 채우는 순증가 0짜리 헛일이 됨).
- GC 트리거: 여유(완전히 빈) zone 수가 low-watermark(`gc_low_watermark`) 이하로 떨어지면 백그라운드 workqueue(`zns_gc_wq`, 한 트리거당 여러 victim을 필요한 만큼 반복 회수 — 단, 무한정 반복하지 않도록 트리거당 `nr_zones`회 상한 있음).
- 재배치: victim zone에 걸친 살아있는 LBA를 memtable(스냅샷 후 그대로 재배치, 이미 최신이라 별도 검증 불필요) + 전체 SSTable(레코드를 읽어 victim에 걸친 것만 후보 → 그 lba의 "진짜 현재 위치"인지 재확인해야 함, 아니면 이미 다른 곳에 덮어써진 stale entry) 스캔으로 찾아서, 매핑 레코드가 아니라 **실제 4KB 데이터**를 읽어 GC 전용 active zone(`ZONE_TAG_GC_DATA`)에 재기록 → `mapping_put()` 그대로 호출.
- 다 옮긴 뒤(전부 성공했을 때만) victim zone을 실제로 하드웨어 reset.
- 사용자 쓰기용 active zone과 GC 재배치용 active zone을 분리해서 운용(`ZONE_TAG_USER_DATA` vs `ZONE_TAG_GC_DATA`).
- **여유 zone 중 `gc_reserved_zones`개는 GC_DATA 전용으로 예약** — GC 자신도 재배치할 데이터를 담을 새 zone이 있어야 하는데, 일반 쓰기가 여유 zone을 전부 가져가버리면 GC가 매번 zone 할당에 실패해 영원히 회수를 못 하는 자기순환 데드락에 빠진다(실제로 겪음). 이 예비분으로 원천 차단.
- 쓰기 경로(`.map()`)의 zone 할당이 실패하면 즉시 ENOSPC로 bio를 실패시키지 않고, GC를 한 라운드 돌려(`zone_pool_alloc_with_gc_retry`) zone을 확보한 뒤 재시도 — GC가 백그라운드에서 트리거되는 시차 동안 도착한 쓰기가 억울하게 실패하는 걸 방지.
- GC는 항상 `zns_gc_wq`(단일 워크큐, `max_active=1`)를 통해서만 실행 — 쓰기 경로의 동기 재시도도 `gc_reclaim_one_victim`을 직접 부르지 않고 `queue_work`+`flush_work`로 우회해서, 여러 쓰기가 동시에 GC를 트리거해도 실제 실행은 항상 하나만 되도록 보장(안 그러면 두 실행이 같은 victim zone을 동시에 건드리는 경쟁이 생김).

**확인**: `scripts/test-gc.sh` — 용량의 일정 비율까지 채운 뒤 같은 영역에 1.2배 분량 random overwrite, "no space left" 없이 완료되는지.

**⚠ 채움 비율을 M3 기준의 80%가 아니라 50%로 낮춰서 검증한다.** 80%는 지금 구조에서 산술적으로 도달 불가능하기 때문 — 이유는 WAL 오버헤드다. WAL은 사용자 쓰기 1건당 1섹터(512B)를 소비하는데(레코드 자체는 32B지만 512B 미만으로는 디바이스에 못 씀 — `CLAUDE.md`의 "WAL 버퍼링 없음" 결정), 4KB 쓰기 기준 **데이터 대비 12.5% 추가**다. 게다가 **WAL zone은 현재 아무도 회수하지 않아 단조 증가**한다. 32 zone(64MB) 디바이스에서 80% 시나리오를 계산하면:

| 항목 | 소비 zone |
|---|---|
| 사용자 데이터 1638MiB | ~26 |
| 2단계 WAL (419,430 writes × 1섹터 = 205MiB) | ~4 |
| 3단계 WAL (503,316 writes × 1섹터 = 246MiB, 회수 안 됨) | ~4 |
| **합계** | **~34 / 32** |

즉 3단계를 시작하기도 전에 여유 zone이 2개 이하로 떨어지고, 그 2개는 `gc_reserved_zones`가 GC 전용으로 묶으므로 **사용자 쓰기가 가져갈 zone이 0개**가 된다. 실측에서도 정확히 이렇게 나왔다 — 3단계에서 25.5MiB만 쓰이고 ENOSPC.

이 상태에선 GC도 원리적으로 무력하다. 2단계는 순수 fill이라 겹쳐쓰기가 거의 없어 모든 zone의 `invalid_count`가 0에 가깝고, GC가 고른 victim은 `invalid_count`가 21(0.13%)인 zone이었다 — 실제 로그: `gc: reclaimed zone 0 (16362 memtable entries relocated)`. zone 하나(16,383블록)를 통째로 옆 zone에 복사하고 원본을 비운 것이라 **순증가 0**. GC는 여유 공간이 있어야만 일하는 메커니즘인데 그 여유가 없었다.

**단, 이 실측은 GC 메커니즘 자체가 정상임을 보여준다** — victim 선정 → memtable 스캔 → 16,362블록 재배치 → 하드웨어 zone reset → `mark_free`까지 에러 없이 완주했고, 행도 dispatch 경고도 `zone_pool_alloc failed`도 없었다. 그래서 **10단계는 50% 시나리오로 GC 로직의 정확성을 먼저 확정하고, 80%(M3 원 기준) 달성은 13단계(WAL zone 회수)로 분리**한다.

---

## 11단계 — 통합 회귀 테스트

- `scripts/test-m1.sh`, `scripts/test-m2.sh` 재실행 — LSM-tree로 교체된 이후에도 통과해야 함.
- `scripts/test-m3.sh` 신규 작성 — 10단계 확인 기준을 스크립트화. (→ `scripts/test-gc.sh`로 이미 작성함, milestone 7로 `test.sh`에 등록됨)
- `scripts/test-crash.sh` 신규 작성 — 5/7단계에서 임시로 만든 확인 절차를 정식 스크립트로 정리. (→ 이미 완료, `test.sh` milestone 3)

---

## 12단계 — 안전장치: zone_pool_alloc 용량 초과 방어 (신규, 미착수)

`zone_pool_alloc(zp, tag, nr, ...)`는 `nr`(요청 섹터 수)이 `zone_sectors`보다 크면 **어떤 zone을 새로 받아도 항상 실패**한다 — 그런데 그 과정에서 그냥 깔끔하게 바로 실패하는 게 아니라, free zone을 하나씩 계속 받아서 태그만 붙이고 못 쓴 채 버리기를 반복하다가(각 zone이 `wp=1`인 채로 방치됨) free zone이 바닥나야 비로소 `-ENOSPC`로 끝난다 — **요청 하나의 실패가 zone pool 전체를 낭비시키는** 최악의 실패 모드.

이게 실제로 일어날 수 있는 지점은 SSTable flush(`flush_memtable_async`)와 compaction(`compaction_work_fn`)의 병합 결과 — 둘 다 여러 레코드(16B)를 한 zone에 통째로 직렬화한다. 현재 기본값(`flush_threshold=1,000,000`, `compaction_k=4`, zone 64MB=131072섹터)으로 계산해보면:

- SSTable flush 1개: `nr_sectors ≈ 31,251` — zone_sectors(131,072)의 약 24%, 여유 충분.
- compaction 최악의 경우(4개 병합, 겹치는 LBA 없음): `nr_sectors ≈ 125,001` — zone_sectors의 약 95%, **여유 6,071섹터(4.6%)뿐**. `flush_threshold`나 `compaction_k`를 조금만 올려도 이 한계를 넘을 수 있음.

**방향(둘 중 선택, 아직 결정 안 함)**:
- (a) **간단한 방어**: `ctr()`에서 `flush_threshold × compaction_k`가 만들어낼 수 있는 최대 병합 크기를 계산해 zone 하나를 넘으면 insmod를 거부(또는 안전한 값으로 clamp). 코드 몇 줄이면 되고, "조용히 zone pool을 다 날려먹는" 실패 모드는 확실히 막음.
- (b) **근본 해결**: `zone_pool_alloc`이 "한 요청은 zone 하나 안에 들어간다"는 전제를 버리고, 요청이 zone 경계를 넘으면 여러 zone에 걸쳐 쓰도록 확장. SSTable flush/compaction 양쪽 다 손대야 하는 구조 변경 — M4에서 실제로 threshold를 올려야 할 필요가 생기면 그때 진행하는 걸 권장(지금은 이론적 위험이라 우선순위 낮음).

---

## 13단계 — WAL zone 회수 (신규, 미착수) — M3 80% 기준의 전제조건

**착수 조건: 10단계가 50% 시나리오로 통과해서 GC 로직이 맞다는 게 확정된 뒤.** 그 전에 시작하면 "GC가 틀린 건지 공간이 없는 건지"가 다시 섞여서 안 보인다.

**문제**: WAL zone은 지금 아무도 회수하지 않는다. 데이터 zone은 GC(10단계)가, SSTable zone은 compaction(9단계)이 회수하는데 WAL만 빠져 있어 **단조 증가**한다. 쓰기 1건당 1섹터씩 영구히 쌓이므로, 오래 돌수록 WAL이 디바이스를 잠식해 결국 GC가 아무리 잘 돌아도 ENOSPC에 도달한다(위 10단계 "확인"의 계산 참고 — 80% 시나리오에서 WAL만 8 zone, 디바이스의 25%).

**왜 지금 바로 못 하나**: WAL 절단의 판단 근거는 CHECKPOINT 레코드("이 seq_no까지는 SSTable로 flush 완료 = 그 이전 WAL은 replay에 불필요")인데, CHECKPOINT는 memtable flush 시에만 쓰인다. 그런데 `flush_threshold` 기본값이 1,000,000이라 지금 테스트 규모에선 flush가 아예 안 일어나고, 따라서 CHECKPOINT도 안 쓰이며, 절단할 근거 자체가 생기지 않는다.

**구현 방향**:
1. `flush_threshold`를 실제로 flush가 일어나는 값으로 낮춘다(테스트 규모 기준). 이건 파라미터 조정이지만, 그 순간부터 SSTable/compaction 경로가 GC와 **동시에** 돌기 시작하므로 사실상 새로운 통합 검증 구간이다 — 10단계까지는 `test-gc.sh`가 `flush_threshold`를 기본값으로 둬서 이 경로를 일부러 격리해왔다는 점에 주의.
2. 마지막 CHECKPOINT의 `wal_split_phys`보다 **완전히 앞선** WAL zone만 회수 대상으로 삼는다. 부분적으로 걸친 zone은 건드리지 않는다(경계 처리를 단순하게 유지).
3. 회수 시 **10단계에서 추가한 "진행 중인 쓰기가 없는 zone만" 가드를 WAL zone에도 똑같이 적용**해야 한다 — `dispatch_wp[z] == z*zone_sectors + wp[z]`. WAL 쓰기도 `.map()`에서 배정만 되고 실제 발행은 나중이라 정확히 같은 위험(`report/bugfix-log.md` 버그 #9)을 가진다.
4. 회수 순서는 compaction과 동일한 원칙 — CHECKPOINT가 durable해진 **다음에만** 옛 WAL zone을 reset. 순서를 뒤집으면 크래시 시 복구 불가.

**주의 — `replay_wal_zones`의 "zone 순서 = 할당 순서" 전제**: WAL zone을 회수하기 시작하면 회수된 zone이 free pool로 돌아가고, `zone_pool_acquire_free`는 zone_id가 가장 작은 free zone을 주므로 **"zone_id 순서 = WAL 기록 시간 순서"가 깨진다**. 지금 replay는 그 전제를 깔고 있어 그대로 두면 재생 순서가 뒤바뀔 수 있다. WAL zone 헤더에 순번(generation)을 넣고 그 순서로 replay하도록 같이 고쳐야 한다.

**부수 개선(선택)**: WAL 레코드는 32B인데 512B 섹터를 통째로 쓰고 있어 **480B(94%)가 패딩으로 낭비**된다. 한 섹터에 16개를 묶으면 오버헤드가 12.5% → 0.78%로 떨어진다. 다만 이건 `CLAUDE.md`에서 M4로 미뤄둔 group commit 이야기라, 13단계에서는 회수만 하고 batching은 별도로 두는 게 범위상 깔끔하다.

**확인**: `scripts/test-gc.sh`를 `FILL_PERCENT=80`으로 돌려 M3 원 기준을 만족하는지. 더불어 `test-crash.sh`가 여전히 통과하는지 반드시 같이 확인(WAL 회수는 crash recovery의 정확성에 직결).

---

## 참고 — 단계를 건너뛰고 싶어질 때

3단계(memtable-only)까지만 해도 M1/M2 테스트는 통과한다. 여기서 "일단 SSTable/compaction/GC 다 나중에 하고 넘어갈까" 싶어질 수 있는데, `report/m2_tmp.md` 사건(문서 참고)처럼 나중에 이름이 헷갈리는 별도 트랙으로 새지 않도록, 이 문서의 단계 번호를 그대로 진행 상황 체크리스트로 쓰는 걸 추천.

---

## 참고 — 범용(general-purpose) dm-target으로 확장하려면

이 프로젝트는 M4에서 dm-zoned/F2FS와 비교할 특정 벤치마크(4KB 정렬 I/O, ext4 위)를
지원하는 게 목표라, 그 워크로드가 실제로 요구하지 않는 것들은 지금 단계에서 일부러
안 만들고 스코프 밖으로 미뤄뒀다. "언젠가 진짜 범용 FTL/dm-target으로 확장한다면"
가정하고 다시 봐야 할 지점들을 여기 모아둔다 — 11단계 안에 들어있는 TODO가 아니라,
지금 스코프 결정이 깨지는 조건이 생기면(다른 워크로드, 다른 파일시스템 등) 그때
검토할 목록.

- **고정 크기(4KB) 블록 가정 → 부분/비정렬 쓰기 지원**: 지금 매핑 테이블은
  "엔트리 하나 = 정확히 BLOCK_SECTORS"라고 가정하고 크기 필드 자체가 없다.
  ext4가 관찰상 항상 4KB 정렬로 쓰길래 성립하는 가정이지, 코드가 독립적으로
  검증하진 않는다. 진짜로 지원하려면 두 방법 중 하나가 필요:
  (a) 매핑을 겹칠 수 있는 가변 길이 구간(extent)으로 바꾸고 조회 시 구간 병합
      로직을 추가 — skip list 자료구조 자체를 바꿔야 함, 또는
  (b) 부분/비정렬 쓰기가 오면 그 블록의 나머지를 먼저 읽어(read-modify-write)
      항상 완전한 4KB로 합쳐서 쓰기 — 매핑 포맷은 그대로 두되, 쓰기 경로에
      "쓰기 전에 먼저 읽는" 새 비동기 단계를 통째로 추가해야 함(지금 있는
      어느 단계 못지않은 규모).
- **SSTable이 zone 하나를 못 넘음**: → 12단계로 옮김(구체적인 수치와 방향
  정리 완료, 아직 미착수).
- **SSTable 직렬화 버퍼가 kzalloc 기반 → 물리적 연속 메모리 한계**:
  `flush_memtable_async`가 헤더+레코드 전체를 `kzalloc`로 한 번에 할당하는데,
  이건 물리적으로 연속된 메모리만 준다. flush_threshold가 커지면(수백만
  엔트리) 커널의 실무적 kmalloc 상한에 걸릴 수 있음 — `vmalloc` + 분할 bio로
  바꿔야 할 수 있음(코드 주석에도 이미 명시).
- **★ WAL zone 순서 = 할당 순서라는 전제 — 더 이상 가정이 아니라 실제로
  깨질 수 있는 상태 ★**: 7단계 체크포인트 replay 최적화가 "zone_id
  오름차순 = 시간순"이라는 전제에 기대고 있는데, 이건 GC/compaction이
  zone을 회수·재사용하지 않을 때만 성립했다. 그런데 10단계 GC가 이제
  실제로 동작 중이라 — GC가 회수한(예: 예전엔 USER_DATA였던) zone은
  FREE로 돌아가고, `zone_pool_acquire_free`는 태그를 안 가리므로 그 zone이
  나중에 WAL 태그로 재배정될 수 있다. 그러면 zone_id는 낮지만 시간상으로는
  더 나중에 할당된 WAL zone이 생겨서, `replay_wal_zones`가 "zone_id
  오름차순 = 최신순"이라고 믿고 훑는 순서가 실제 시간 순서와 어긋날 수
  있다 — 크래시 복구 시 최신 체크포인트를 못 찾거나 엉뚱한 지점부터
  재생할 위험.

  **아직 실제로 재현/확인은 안 했지만, "WAL zone 자체를 회수해야만
  일어나는 문제"가 아니라는 점에 주의** — GC는 WAL zone을 회수하지
  않지만, USER_DATA zone은 지금 당장 회수하고 있고 `zone_pool_acquire_free`
  는 free zone을 찾을 때 "예전에 무슨 태그였는지"를 안 가린다. 즉 GC가
  회수한 (예전 USER_DATA) zone이 그다음 WAL rollover 때 그대로 새 WAL
  zone으로 뽑힐 수 있다 — zone_id는 낮은데 할당 시점은 더 나중인 WAL
  zone이 생기는 조건이 이미 갖춰져 있다는 뜻. `scripts/test-gc.sh`처럼
  쓰기가 계속되며 GC도 계속 도는 워크로드에서는 실제로 일어났을
  가능성이 낮지 않음 — 크래시 복구 테스트를 GC 활성 상태에서 같이
  돌려봐야 실제 재현 여부를 확인할 수 있음. 고치려면 zone_id 대신
  별도의 단조증가 "할당 세대 번호"를 붙여서 zone_id와 무관하게 진짜
  시간 순서를 판별하도록 바꿔야 함.
- **discard/TRIM 미지원**: WAL/SSTable 레코드에 tombstone이 없다. 파일
  삭제(TRIM) 시 매핑을 무효화하는 경로가 아직 없어서, GC의 invalid_count가
  "덮어쓰기로 죽은 공간"만 잡고 "삭제로 죽은 공간"은 못 잡는다. discard
  경로를 지원하려면 tombstone 레코드 타입을 추가해야 함(설계 결정 로그에도
  "discard 지원 시 추가"로 이미 남겨둔 항목).
- **skip list가 전역 spinlock 하나로 보호**: lock-free 동시 삽입은 아직
  구현 안 됨(결정 로그에 이미 "별도 후속 과제"로 명시) — 코어 수가 많은
  환경에서 memtable 락 경합이 병목이 될 수 있음.
- ~~zone_pool_reset이 실제 하드웨어 reset을 안 보냄~~ — **해결됨**(9~10단계
  구현 중 `zone_reset_hw`로 `blkdev_zone_mgmt(REQ_OP_ZONE_RESET)`을 실제로
  내리도록 채움, compaction/GC 둘 다 사용).
