# 13. DM 타깃 핵심 개념 정리

---

## 전체 흐름 한눈에 보기

사용자가 파일에 데이터를 쓰면 커널 안에서 아래 경로를 따라 SSD까지 내려간다.

```
사용자 프로그램
  write(fd, buf, 4096)
       │
       ▼
  파일시스템 (ext4 등)
  "어느 블록에 쓸지 결정"
       │
       ▼  BIO 생성
  블록 레이어
       │
       ▼
  Device Mapper         ← /dev/mapper/mymapper
  myzns_map(ti, bio)    ← 우리 코드가 여기서 BIO를 가로챔
       │
       ▼  bi_bdev, bi_sector 변경 후
  nullb0 (ZNS 디바이스)
       │
       ▼
  실제 저장
```

중간에 Device Mapper가 끼어서 BIO를 가로채는 것이 핵심이다.  
우리는 이 가로채는 지점(`map`)에서 BIO를 조작해 ZNS의 제약을 처리한다.

---

## BIO란?

**BIO(Block I/O)** 는 커널이 블록 I/O 요청 하나를 표현하는 구조체다.  
파일시스템이 "섹터 N에 4 KiB 써"라고 결정하면 커널이 BIO를 만들어 블록 레이어로 넘긴다.

BIO를 택배 송장에 비유하면:

```
┌─────────────────────────────────┐
│ 수신처(bi_bdev)   : /dev/nullb0 │  ← 어느 디바이스로
│ 주소(bi_sector)   : 섹터 100    │  ← 몇 번 섹터부터
│ 크기(bi_size)     : 4096 바이트 │  ← 얼마나
│ 종류(bi_opf)      : WRITE       │  ← 읽기/쓰기
│ 데이터(bvec)      : [페이지들]  │  ← 실제 데이터
└─────────────────────────────────┘
```

`map()`에서 우리가 할 일은 이 송장의 **수신처와 주소를 바꾸는 것**이다.

### 주요 필드

| 필드 | 의미 |
|---|---|
| `bio->bi_iter.bi_sector` | 요청 시작 섹터 (LBA) |
| `bio->bi_opf` | `REQ_OP_READ` / `REQ_OP_WRITE` 등 |
| `bio->bi_iter.bi_size` | 요청 크기 (바이트) |

### M0와 M1에서 BIO를 다루는 방식

```c
// M0 — 목적지 디바이스만 바꾼다
bio_set_dev(bio, c->dev->bdev);   // bi_bdev 변경
// bi_sector는 그대로 → 1:1 매핑

// M1 — 목적지 + 섹터 번호 둘 다 바꾼다
bio_set_dev(bio, c->dev->bdev);
bio->bi_iter.bi_sector = wp;      // 임의 LBA → 활성 zone의 write pointer
```

---

## Device Mapper란?

**Device Mapper(DM)** 는 리눅스 커널에 내장된 블록 디바이스 중간 계층이다.  
`/dev/mapper/X` 라는 가상 블록 디바이스를 만들고, 그 디바이스로 들어오는 모든 BIO를 등록된 타깃 코드로 중계한다.

```
위쪽 (파일시스템 / 사용자)
         │
  /dev/mapper/mymapper   ← DM이 만든 가상 블록 디바이스
         │
  [DM 타깃: myzns_map()] ← 우리 코드
         │
  /dev/nullb0            ← 실제 디바이스
아래쪽 (ZNS SSD)
```

DM은 "플러그인 슬롯"(`struct target_type`)을 제공하고,  
우리는 함수 포인터를 채워서 `dm_register_target()`으로 등록한다.  
등록 후부터 DM이 적절한 시점에 우리 함수를 호출한다.

---

## 콜백 4종 상세

### ctr (constructor)

**언제**: `dmsetup create` 시 단 한 번  
**역할**: 타깃 인스턴스 초기화

```
echo "0 $SECTORS myzns /dev/nullb0" | dmsetup create mymapper
                                      ↑
                                      이 순간 ctr 호출
```

ctr이 하는 일:
1. 컨텍스트 구조체(`myzns_c`) 할당 → 이 인스턴스의 상태를 저장할 공간
2. underlying 디바이스(`/dev/nullb0`) 열기 → 핸들을 구조체에 저장
3. `ti->private`에 구조체 달기 → 이후 모든 콜백에서 꺼내 쓸 수 있도록
4. flush/discard BIO 통과 설정

에러 발생 시 **역순으로 해제**해야 한다. 열었던 것을 닫고, 할당한 것을 해제하는 순서다.

---

### dtr (destructor)

**언제**: `dmsetup remove` 시 단 한 번  
**역할**: ctr에서 열고 할당한 것을 정확히 역순으로 해제

```
dmsetup remove mymapper
↑
이 순간 dtr 호출
```

ctr과 dtr은 항상 짝을 이룬다:

```
ctr: kzalloc → dm_get_device → ti->private = c
dtr: dm_put_device → kfree
```

---

### map

**언제**: BIO가 들어올 때마다 (읽기/쓰기/flush/discard 전부)  
**역할**: 가상 디바이스로 들어온 BIO를 실제 디바이스로 보낼 주소로 변환

이 콜백이 DM 타깃의 **핵심 로직**이 들어가는 곳이다.

**반환값에 따라 프레임워크 동작이 달라진다:**

| 반환값 | 의미 |
|---|---|
| `DM_MAPIO_REMAPPED` | 주소를 바꿨으니 프레임워크가 submit |
| `DM_MAPIO_SUBMITTED` | 내가 직접 submit했으니 손대지 마 |
| `DM_MAPIO_KILL` | 이 BIO를 오류로 종료 |

**M0 vs M1 map 동작 비교:**

```
M0 (pass-through)
  BIO 들어옴 (sector=100, op=WRITE)
       │
  bio_set_dev(nullb0)    ← 목적지만 바꿈
       │
  nullb0 sector=100 WRITE → ZNS 제약 위반 가능성 있음

M1 (변환)
  BIO 들어옴 (sector=100, op=WRITE)
       │
  bio_set_dev(nullb0)
  bi_sector = wp         ← 섹터 번호도 wp로 바꿈
  매핑 테이블 기록: 100 → (zone, offset)
       │
  nullb0 sector=wp WRITE → 항상 순차, ZNS 제약 만족
```

---

### report_zones

**언제**: `blkzone report /dev/mapper/mymapper` 실행 시  
**역할**: 위쪽에 zone 정보(start, len, wptr, 상태 등)를 돌려줌

M0에서는 underlying 디바이스에 그대로 위임한다:

```c
return dm_report_zones(c->dev->bdev, ti->begin, args->next_sector, args, nr_zones);
```

**M1에서는 이 콜백이 제거된다.**  
M1에서 위쪽은 conventional 디바이스로 보이기 때문에 zone 정보가 없다.

---

### iterate_devices

**언제**: DM 프레임워크가 큐 속성을 수집할 때 (디바이스 생성 직후 등)  
**역할**: 이 타깃 아래에 어떤 디바이스가 있는지 프레임워크에 알림

이 콜백이 없으면 아래 속성들이 sysfs에 올바르게 노출되지 않는다:

```bash
cat /sys/block/dm-0/queue/zoned          # none  (host-managed 여야 함)
cat /sys/block/dm-0/queue/chunk_sectors  # 0     (131072 여야 함)
```

`chunk_sectors`가 0이면 프레임워크가 zone 크기를 모르기 때문에  
`blkzone report`가 `"unable to determine zone size"`로 실패한다.

---

## test-basic.sh 검증 흐름

테스트 스크립트가 각 콜백을 어떻게 검증하는지 매핑하면:

```
[insmod + dmsetup create]
  → ctr 호출 검증

[1/4] blkzone report
  → report_zones + iterate_devices 검증
  (zone 목록이 나오면 두 콜백 모두 정상)

[2/4] blkzone reset
  → map 경로로 reset 명령이 nullb0까지 전달되는지 검증

[3/4] dd (4 KiB 쓰기)
  → map에서 BIO가 올바르게 remapping되는지 검증
  (0 bytes copied면 nullb0이 reject한 것)

[4/4] wptr = 0x000008 확인
  → 실제로 nullb0에 8섹터가 써졌는지 검증

[dmsetup remove + rmmod]
  → dtr 호출 검증
```

---

## M0가 ZNS 테스트를 통과할 수 있는 이유

M0는 pass-through인데 왜 테스트가 통과할까?

테스트가 **zone reset → 섹터 0 쓰기** 순서를 강제하기 때문이다.

```
zone 0 reset → wp = 0
dd seek=0    → 섹터 0에 쓰기 요청
map()        → sector 그대로 0으로 내려감
nullb0       → wp=0이고 sector=0 → 순차 조건 만족 → 통과
```

하지만 일반적인 임의 쓰기를 보내면 M0는 바로 실패한다:

```
임의 LBA=500 쓰기 요청
map()  → sector 그대로 500으로 내려감
nullb0 → wp=0이고 sector=500 → 순차 조건 위반 → I/O error
```

이걸 해결하는 것이 M1의 과제다.
