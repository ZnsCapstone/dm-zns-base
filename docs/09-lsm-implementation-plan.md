# 09. LSM-Tree 매핑 테이블 — 구현

전체 구조와 각 구조체가 **왜** 필요한지는 [10-lsm-tree-architecture.md](10-lsm-tree-architecture.md)
먼저 참고. 이 문서는 그 구조를 실제로 **어떻게 구현했는지**를 영역별로 정리한다.

매핑 테이블은 M1의 `map[]` flat array를 LSM-Tree(memtable + WAL + SSTable +
compaction)로 교체하고, 여기에 GC와 크래시 복구를 더한 것이다. 상위 계층(ext4)에는
평범한 블록 디바이스로 보이고, 아래에서 LBA→물리 매핑을 관리하며 모든 쓰기를
zone의 write pointer 순서대로만 디바이스에 내보낸다.

## 구현 요약

| 영역 | 핵심 구현 | 검증 스크립트 |
|---|---|---|
| Zone pool (태그 기반 할당) | 하나의 pool을 용도 태그로 공유, `wp`/`dispatch_wp` 분리 추적 | — |
| Memtable (skip list) | 정렬 유지 in-RAM 매핑, upsert가 옛 값 반환 | — |
| 매핑 테이블 | `mapping_put`/`mapping_get`로 `map[]` 대체 | `test-m1`, `test-m2` |
| WAL | 매 쓰기 매핑을 32B 레코드로 즉시 append | `test-m1`, `test-m2` |
| 영속성 | generation 순서 WAL replay + CHECKPOINT | `test-crash`, `test-checkpoint` |
| SSTable | 정렬 flush + on-disk binary search 읽기 | `test-sstable-read` |
| Compaction | 오래된 K개 k-way merge, 중복 제거 | `test-compaction` |
| GC | 무효율 최대 데이터 zone 이주 후 회수 | `test-gc`, `test-gc-crash` |
| WAL zone 회수 | CHECKPOINT durable 후 옛 WAL zone reset | `test-wal-reclaim` |
| 발행 순서 게이트 | zone별 실제 발행 순서 강제 (순차쓰기 보장) | 전 경로 공통 |
| 안전장치 | zone 용량 초과 즉시 실패, SSTable I/O 크기 상한 | — |

설계 결정의 근거는 `CLAUDE.md`, 구현 중 겪은 버그 사건의 원인/해결 이력은
`report/bugfix-log.md`(로컬)에 별도 정리돼 있다.

---

## 1. Zone pool — 태그 기반 할당

여러 역할(사용자 쓰기, GC 이주, WAL, SSTable)이 zone을 나눠 쓰므로, M1의
"활성 zone 하나에 순차 append"를 태그별로 일반화했다.

- **enum zone_tag**: `FREE`, `USER_DATA`, `GC_DATA`, `WAL`, `SSTABLE`. 전용 zone을
  영구히 떼어두지 않고, 그때그때 pool에서 태그를 붙여 배정한다(ZenFS 방식) — 다 쓴
  zone은 GC/compaction이 회수해 다시 FREE로 돌린다.
- **`zone_pool_alloc(tag, nr, ...)`**: 해당 태그의 활성 zone에서 `nr` 섹터를 배정하고,
  꽉 차면 FREE zone을 새로 잡아 넘어간다. 새로 잡은 zone은 섹터 0을 태그 헤더용으로
  예약(`wp=1`)한다.
- **`wp[]` vs `dispatch_wp[]`**: `wp`는 "배정된" 섹터, `dispatch_wp`는 "실제 디바이스에
  발행된" 섹터(절대 좌표). 이 둘의 분리가 [발행 순서 게이트](#10-발행-순서-게이트-dispatch-gate)의
  기반이다.
- **`zone_reset_hw`**: `blkdev_zone_mgmt(REQ_OP_ZONE_RESET)`으로 실제 하드웨어 zone을
  비운다. compaction/GC가 회수 시 사용.

---

## 2. Memtable — skip list

커널 표준 구현이 없어 직접 작성했다(`src/skiplist.c`). 노드는 `{lba, phys, level,
forward[]}`.

- **upsert가 옛 값을 반환**한다 — 기존 키를 덮어쓸 때 옛 `phys`를 얻어 그 zone의
  `invalid_count`를 즉시 올릴 수 있다(GC victim 선정 근거).
- 정렬 순서 iterator를 제공한다(SSTable flush가 정렬된 채로 기록).
- 동시성은 전역 `spinlock_t` 하나로 보호(lock-free 삽입은 M4 과제).

---

## 3. 매핑 테이블 — `mapping_put` / `mapping_get`

`.map()`이 M1의 `map[]` 대신 이 두 함수를 쓴다.

```
mapping_put(lba, phys):
    old = skiplist_upsert(memtable, lba, phys)
    if old (덮어쓰기): invalid_count[zone_of(old)]++
mapping_get(lba):
    return skiplist_lookup(memtable, lba)   // SSTable 폴백은 6번에서 확장
```

- **엔트리는 전부 고정 크기(`BLOCK_SECTORS` = 4KB)** — 테이블에 크기 필드가 없다.
  `ti->max_io_len` + `dm_accept_partial_bio`가 bio를 블록 하나 이하로 잘라주므로
  `mapping_put` 시점엔 항상 "어떤 4KB 블록 전체"임이 보장된다.
- **조회 키는 항상 블록 정렬 LBA** — `block_lba = (lba/BLOCK_SECTORS)*BLOCK_SECTORS`로
  내림해서 찾고, 돌려받은 `phys`에 `offset_in_block`을 더해 응답한다. 커널이 블록
  정렬 안 된 위치(ext4 슈퍼블록 프로브 등)로 읽어도 정확히 대응된다.

---

## 4. WAL — 쓰기 로그

memtable은 휘발성이라, 매 쓰기의 매핑을 디스크에 즉시 남겨 크래시 복구의 단서로
삼는다.

- **32B 고정 레코드**(`type/reserved` + union): `PUT{lba, phys}` / `CHECKPOINT{seq_no,
  split_gen, split_off}`. 512/4096에 나머지 없이 나눠떨어지는 크기.
- **버퍼링 없이 매 쓰기마다 즉시 append**(정확성 우선, group commit은 M4).
- `.map()` 쓰기 경로는 `DM_MAPIO_REMAPPED` → `DM_MAPIO_SUBMITTED`로 바뀐다 — 타깃이
  bio 완료를 직접 책임지는 비동기 콜백 체인 구조. WAL이 durable해진 뒤
  (`wal_put_done`) 비로소 memtable에 반영하고 원본 데이터 bio를 발행한다(매핑이
  데이터보다 먼저 복구 가능해야 함).

---

## 5. 영속성 — WAL replay & CHECKPOINT

재적재(re-insmod) 시 zone reset 없이 상태를 복원한다.

- **복구 스캔**: `blkdev_report_zones` → 각 zone의 태그 헤더(섹터 0)를 읽어
  `zone_tag`/`wp`/`dispatch_wp`/`wal_gen`을 복원한다.
- **generation 순서 replay**: WAL zone을 **물리(zone_id) 순서가 아니라 generation
  순서**로 재생한다. WAL zone을 회수하기 시작하면 zone_id 순서 ≠ 기록 순서가 되기
  때문 — zone 헤더의 `gen`(단조 증가 배정 순번)이 진짜 시간 순서를 알려준다.
- **CHECKPOINT**: memtable을 SSTable로 flush할 때 그 시점의 WAL 스트림 위치를
  `(split_gen, split_off)`로 남긴다. replay는 이 지점보다 앞선 PUT은 이미 SSTable에
  반영됐다고 보고 건너뛴다 — 복구 시간을 "전체 재생"에서 "마지막 CHECKPOINT
  이후만"으로 줄인다.

---

## 6. SSTable — flush & 읽기 경로

**Flush**: memtable 엔트리가 `flush_threshold`(기본 1,000,000)를 넘으면
`wal_put_done`이 새 빈 memtable로 교체하고, 옛 memtable을 백그라운드로 flush한다
(fire-and-forget — 매핑이 WAL에 이미 durable하므로 데이터 bio는 안 기다림).

- 정렬 순회로 `[헤더 + 레코드]`를 직렬화해 SSTABLE zone에 기록.
- 헤더: `magic, seq_no, record_count, min_lba, max_lba`. 레코드: 고정 16B(`lba, phys`).
- flush durable → CHECKPOINT append → 옛 memtable 해제.

**읽기 경로**: `mapping_get`이 memtable을 놓치면 SSTable로 폴백한다.

- `min_lba`/`max_lba`로 후보 SSTable을 먼저 걸러낸다(bloom filter 대체).
- 후보 안에서는 **on-disk binary search** — 레코드가 고정 16B(512B당 32개)라 필요한
  512B 섹터 몇 개만 읽어 O(log n). SSTable 전체를 통짜로 읽지 않는다.
- 같은 LBA가 여러 SSTable에 걸치면 `seq_no` 최댓값 우선.

---

## 7. Compaction

SSTable 개수가 `compaction_k`(기본 4)에 도달하면 백그라운드 워커
(`zns_compaction_wq`, `max_active=1`)가 병합한다.

- 가장 오래된 K개를 **k-way merge** — 각 SSTable이 이미 LBA 정렬돼 있어 재정렬 불필요.
- 같은 LBA는 `seq_no` 최댓값만 남기고, 밀려난 옛 매핑의 `phys`는
  `invalid_count[zone_of(phys)]++`로 GC에 힌트를 남긴다.
- **회수 순서**: 새 병합 SSTable이 완전히 durable해진 **다음에만** 옛 K개의 zone을
  reset. 중간 크래시에도 읽기가 seq_no 우선이라 결과가 불변이므로, compaction 전용
  WAL 레코드가 필요 없다.
- 매핑 메타데이터(SSTable zone)만 회수하며 실제 데이터 zone은 건드리지 않는다.

---

## 8. GC — 데이터 zone 회수

무효 페이지가 쌓인 데이터 zone의 산 데이터를 이주시키고 zone을 회수한다
(`zns_gc_wq`, `max_active=1`).

- **victim 선정**: greedy — 닫힌 데이터 zone(`USER_DATA` + `GC_DATA`) 중 `invalid_count`
  최대. `invalid_count==0`(순증가 0)·활성 zone·발행 미완 zone은 제외.
- **트리거**: free zone이 `gc_low_watermark`(기본 2) 이하로 떨어지면 큐잉. 또한 쓰기
  경로의 zone 배정이 실패하면 `zone_pool_alloc_with_gc_retry`가 GC를 한 라운드 돌려
  확보 후 재시도 — 백그라운드 트리거 시차 동안 도착한 쓰기가 억울하게 ENOSPC를
  맞는 걸 막는다.
- **이주**: victim에 걸친 산 LBA를 memtable(즉시)·SSTable(현재 위치 재확인 후 stale
  제외) 스캔으로 찾아, **실제 4KB 데이터**를 GC 전용 zone(`GC_DATA`)에 재기록하고
  `mapping_put`한다. 재기록이 durable해진 뒤 WAL에도 남겨(`gc_wal_log_put`), 이주
  매핑이 SSTable flush 전 크래시 시 replay가 옛(곧 reset될) 위치가 아니라 새 위치로
  복원하게 한다.
- **회수**: 전부 성공했을 때만 victim을 하드웨어 reset(안전 후퇴).
- **Hot/Cold 분리**: 이주 데이터(`GC_DATA`)와 사용자 쓰기(`USER_DATA`)의 active zone을
  분리해 재무효화를 늦춘다.
- **예비 zone**: free zone 중 `gc_reserved_zones`(기본 2)개는 GC 이주·WAL 전용으로
  예약한다. 일반 쓰기가 여유 zone을 전부 가져가면 GC가 이주할 zone을 못 얻어 회수
  자체를 못 하는 자기순환 데드락에 빠지기 때문(GC 경로는 이 예비분까지 쓸 수 있다).

`test-gc.sh`로 검증한다(용량을 채운 뒤 같은 영역을 overwrite해도 ENOSPC 없이 완료).
GC는 여유 zone이 있어야 동작하는 메커니즘이라, 초기에는 WAL zone이 회수되지 않고
단조 증가해 달성 가능한 채움 비율이 제한됐다 — 이는 아래 [WAL zone 회수](#9-wal-zone-회수)로
해소했다.

---

## 9. WAL zone 회수

데이터 zone은 GC가, SSTable zone은 compaction이 회수하지만 WAL zone은 초기에 아무도
회수하지 않아 **단조 증가**했다(쓰기 1건당 1섹터 영구 누적). 오래 돌수록 WAL이
디바이스를 잠식해 결국 ENOSPC에 이르므로, 회수 경로를 추가했다.

- **회수 대상 판정**(전부 `c->lock` 하): ① `wal_gen[z] < wal_durable_split_gen` —
  이 zone의 레코드가 전부 durable SSTable에 반영됨(replay 불필요). ② 활성 WAL zone이
  아님. ③ `dispatch_wp[z] == 시작+wp` — 아직 발행 안 된 배정분이 없음.
- **durable 판정**: `flush_chain_end`의 in-flight 카운터가 0이 될 때(= 발행된
  CHECKPOINT가 전부 durable) durable 지점을 올린다. bio 완료가 out-of-order일 수
  있으므로 개별 CHECKPOINT 완료 순서에 기대지 않는다.
- **회수 순서**: CHECKPOINT durable → 그 다음에만 옛 WAL zone reset(compaction과 동일
  원칙). 데이터/SSTable zone과 달리 정상 운영 중 WAL zone을 읽는 경로가 없어 회수 vs
  동시 읽기 race가 없다.

이 회수가 [5. generation 순서 replay](#5-영속성--wal-replay--checkpoint)를 전제로 한다 —
회수로 zone_id 순서가 뒤섞여도 gen으로 올바른 재생 순서를 찾는다.

---

## 10. 발행 순서 게이트 (dispatch gate)

여러 비동기 쓰기가 같은 zone에 순서대로 쌓여야 하는데, WAL 완료 콜백이 불리는
순서는 우리가 WAL을 submit한 순서와 같다는 보장이 없다(커널/디바이스가 정함). 이걸
그대로 두면 같은 zone에 순서가 뒤바뀌어 발행돼 **ZNS 순차쓰기 위반(EIO)**이 난다.

- "배정 순서"(`wp[]`)와 별도로 **"실제 발행 순서"(`dispatch_wp[]`)**를 zone별 절대
  섹터로 추적한다.
- 어떤 쓰기든 `phys == dispatch_wp[zone]`(자기 차례)일 때만 즉시 발행하고, 아니면
  대기열에 걸어뒀다가 앞선 쓰기가 나갈 때 자동으로 풀어준다.
- 비동기 경로(WAL/헤더/CHECKPOINT/데이터/SSTable flush)는 `zone_dispatch_write`,
  동기 경로(compaction/GC의 `submit_bio_wait`)는 `zone_dispatch_wait_turn`으로 자기
  차례까지 블로킹 대기한다.
- **배정 = 발행 트랜잭션**: 배정받은 phys가 중간 실패로 버려지면 그 zone의
  `dispatch_wp`가 영구히 멈추므로, 포기 경로(ENOSPC/OOM)는 반드시
  `zone_dispatch_cancel`로 자리를 넘겨준다.

또한 `submit_bio`조차 atomic 컨텍스트(bio 완료 콜백)에서 직접 부르면 큐 혼잡 시 그
안의 admission control(`wbt`)이 스케줄링을 시도해 죽으므로, **실제 제출은 항상 전용
워크큐 워커(process 컨텍스트)로 미룬다**(`submit_bio_deferred`).

---

## 11. 안전장치

### zone 용량 초과 즉시 실패

`zone_pool_alloc`은 요청 `nr`이 zone 하나 용량(`zone_sectors - 1`, 섹터 0은 헤더)을
넘으면 **어떤 zone을 새로 받아도 항상 실패**한다. 방어 없이는 FREE zone을 하나씩
받아 태그만 붙이고 못 쓴 채 버리길 반복해 요청 하나가 pool 전체를 낭비시키므로,
`nr > zone_sectors - 1`이면 **즉시 `-ENOSPC`**로 실패하게 했다.

실제로 이 한계에 닿을 수 있는 지점은 compaction 병합 결과다(tiered라 반복될수록
커짐). 현재 기본값에선 여유가 있지만, `flush_threshold`/`compaction_k`를 올리면 zone
하나를 넘을 수 있어 방어가 필요하다. 요청을 여러 zone에 걸쳐 쓰는 확장은
[향후 과제](#향후-확장--스코프-밖)로 둔다.

### SSTable I/O 크기 상한

SSTable이 커지면 한 bio가 담을 수 있는 `BIO_MAX_VECS`(256페이지 = 1MB)를 넘겨 커널
BUG를 냈다(compaction 병합 결과가 특히 큼). 읽기 경로도 "SSTable 전체를 통짜 bio +
통짜 할당"이라 같은 위험 + 거대 `GFP_ATOMIC` 할당 + read amplification을 가졌다.

- **write**(flush/compaction): 버퍼는 `kvmalloc`(수 MB면 vmalloc 폴백), 제출은
  `≤BIO_MAX_VECS` 페이지씩 청크로(`sstable_io_sync` 또는 async 청크 체인).
- **point lookup**(`.map` READ, GC 검증): on-disk binary search로 필요한 섹터만.
- **full scan**(compaction 병합 소스, GC victim 스캔): `kvmalloc` + 청크 read.

**남은 한계**: flush 버퍼는 여전히 atomic 컨텍스트(`wal_put_done` 콜백)에서
`kzalloc(GFP_ATOMIC)`로 잡으므로 `flush_threshold`가 매우 크면 그 할당 자체가 실패할
수 있다(그 경우 이 세대 flush를 포기 — 데이터는 WAL에 남아 복구 가능, 크래시 아님).
근본 해결은 flush 직렬화를 process 컨텍스트로 옮기는 것(현 범위 밖).

---

## 향후 확장 / 스코프 밖

M4에서 비교할 벤치마크(4KB 정렬 I/O, ext4 위)가 요구하지 않는 것들은 의도적으로
스코프 밖에 두었다. 다른 워크로드·파일시스템으로 넓힐 때 다시 볼 지점:

- **고정 4KB 블록 가정 → 부분/비정렬 쓰기**: 매핑에 크기 필드가 없어 "엔트리 하나 =
  4KB"를 가정한다. 지원하려면 가변 길이 extent 매핑으로 바꾸거나, 쓰기 전에 블록
  나머지를 읽어 합치는 read-modify-write 단계를 추가해야 한다.
- **SSTable이 zone 하나를 못 넘음**: 현재는 초과 요청을 즉시 실패시켜 pool 낭비만
  막는다([11. 안전장치](#11-안전장치)). 진짜로 zone 경계를 넘겨 쓰려면 flush/compaction
  양쪽을 다중 zone 지원으로 확장해야 한다.
- **discard/TRIM 미지원**: WAL/SSTable에 tombstone이 없어, 파일 삭제로 죽은 공간을 GC가
  못 잡는다(덮어쓰기로 죽은 공간만 잡음). tombstone 레코드 타입 추가가 필요.
- **skip list 전역 spinlock**: lock-free 동시 삽입 미구현 — 코어가 많으면 memtable 락
  경합이 병목.
- **읽기 vs 회수 race**: 읽기가 스냅샷한 phys를 compaction/GC가 그 사이 reset할 수
  있다. 참조 카운트/epoch 기반 통합 회수-안전 메커니즘이 필요(미도입).
