# 10. LSM-tree 매핑 테이블 아키텍처 개요

`docs/09-lsm-implementation-plan.md`가 "몇 단계로 뭘 짤지"라면, 이 문서는 그 전에 "각 구조체가 왜 필요하고 전체가 어떻게 맞물리는지"를 설명한다. 설계 근거의 세부(수치, 트레이드오프)는 `CLAUDE.md` 결정 로그 참고.

---

## 출발점: 근본 문제 하나

ext4는 아무 LBA에나 씁니다. ZNS zone은 append만 허용합니다. 그래서 "LBA X는 지금 물리적으로 어디 있는가"라는 사실 하나가 반드시 어딘가에 기록되어야 합니다. 이 사실 하나가 `sstable_record {lba, phys}`입니다 — 나머지 구조체들은 전부 **이 사실 하나를 "어디에, 얼마나 오래, 얼마나 빨리 찾을 수 있게" 보관할지**를 해결하기 위한 장치입니다.

## 왜 이 사실들을 통째로 배열에 안 두나 (memtable/skip list)

M1은 `map[lba] = phys`를 디바이스 전체 크기만큼 RAM에 올려뒀습니다. 디바이스가 커지면 이 배열도 그만큼 커져서 결국 RAM이 못 버팁니다. 그래서 **최근 것만 RAM에, 오래된 건 zone(디스크)으로 밀어내는** 구조가 필요한데, 그 RAM 쪽 부분이 skip list(memtable)입니다 — 크기가 ~64MB로 고정돼 있고, 다 차면 밀어냅니다(flush).

## 왜 flush한 걸 그냥 배열로 안 두고 SSTable(헤더+레코드)로 두나

memtable이 다 차면 그 안의 사실들을 zone에 순차로 써야 하는데, 그냥 레코드만 쭉 쓰면 나중에 "이 SSTable에 LBA=X가 있나?"를 확인하려고 파일 전체를 읽어야 합니다. 그래서:

- `sstable_header`의 `min_lba`/`max_lba`로 "이 SSTable엔 X가 아예 없다"를 파일 안 읽고 걸러냄
- 레코드가 이미 LBA 정렬돼 있어서(memtable이 skip list라 순회하면 정렬됨) `record_count`만 알면 binary search 가능
- `seq_no`는 여러 SSTable에 같은 LBA가 걸쳐 있을 때 "이게 더 최근 것"을 가려내는 유일한 근거

즉 SSTable은 "옛날 사실들을 압축해서 zone에 밀어둔 것 + 빠르게 뒤질 수 있는 색인"입니다.

## 왜 WAL이 따로 필요한가

memtable은 RAM이라 크래시 나면 사라집니다. 근데 문제는 — ext4한테는 이미 "쓰기 성공"이라고 답했고, 실제 데이터 바이트도 이미 zone에 durable하게 박혀 있는데, `{lba, phys}` 사실이 RAM(memtable)에만 있다가 날아가면 **데이터는 zone 어딘가에 멀쩡히 있는데 그게 어디인지 아무도 모르는 상태**가 됩니다. 그래서 memtable에 넣기 전에 먼저 WAL(zone에 append)에 그 사실을 durable하게 기록해두고, 크래시 후 재부팅하면 WAL을 재생해서 memtable을 복원합니다.

`wal_record`가 32B로 SSTable 레코드(16B)와 다른 이유는, PUT 말고 **CHECKPOINT**라는 두 번째 종류의 레코드도 같이 담아야 해서입니다 — "이 시점까지의 PUT은 이미 SSTable로 안전하게 옮겨졌다"는 표시입니다. 이게 있어야 재생할 때 WAL 전체를 다시 읽지 않고 마지막 체크포인트 이후만 읽으면 됩니다.

## 왜 compaction이 필요한가

SSTable이 계속 쌓이면(memtable이 찰 때마다 하나씩 생기니까) 읽기 하나 할 때마다 뒤져야 할 SSTable 개수가 계속 늘어납니다(read amplification). 게다가 같은 LBA가 여러 SSTable에 흩어져 있으면(예전 값, 최신 값 둘 다 디스크에 남아있음) 공간도 낭비됩니다. Compaction은 여러 SSTable을 하나로 합치면서 오래된 값을 버려서, **SSTable 개수를 다시 줄이고**, 그 과정에서 "이 옛날 phys 위치는 이제 아무도 안 가리킨다"는 걸 알아냅니다.

## 왜 GC/invalid_count가 필요한가

여기서 compaction이 알아낸 "이제 안 쓰는 phys 위치"가 등장하는 이유입니다 — LBA가 새 위치로 덮어써질 때마다 **옛 물리 위치는 여전히 zone 안에 바이트로 남아있지만 아무도 가리키지 않는 죽은 공간**이 됩니다. ZNS zone은 부분 삭제가 안 되고 zone 전체를 reset해야만 재사용 가능하니, 각 zone이 "얼마나 죽었는지"(`invalid_count`)를 추적해뒀다가, 많이 죽은 zone을 골라 그 안의 몇 안 남은 산 데이터만 다른 zone으로 옮기고(재배치도 결국 `mapping_put` 호출 하나) zone 전체를 reset해서 되돌려받는 게 GC입니다.

## 한 번의 쓰기로 전체를 훑으면

```
ext4가 LBA=X에 4KB 쓴다
  → phys 위치 결정 (active zone의 wp)
  → wal_record{PUT, X, phys} 를 WAL zone에 append          ← 크래시 대비, 여기서 먼저 durable해짐
  → memtable(skip list)에 {X: phys} upsert                  ← 옛값 있으면 그 zone의 invalid_count++
  → memtable 다 찼으면:
        sstable_header + records 를 SSTable zone에 flush
        wal_record{CHECKPOINT, seq_no} append               ← 이제 이 WAL 구간은 재생 안 해도 됨
        SSTable 개수가 K=4 넘으면 compaction 백그라운드 큐잉
  → 실제 4KB 데이터를 phys 위치에 순차 append
  → (여유 zone이 부족해지면 GC가 백그라운드로 따로 돎, 위 흐름과 병렬)
```

## 한 줄 요약

- **skip list**: 최근 것을 빠르게 담아두는 RAM 색인
- **SSTable**: 오래된 것을 압축해서 zone에 색인째로 보관
- **WAL**: skip list가 사라져도 복구할 보험
- **compaction**: SSTable이 너무 많아지는 걸 막는 정리
- **GC**: zone 자체의 죽은 공간을 회수

다섯이 각자 다른 문제 하나씩을 맡고 있다.
