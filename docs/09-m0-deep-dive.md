# 09. M0 완전 이해 — 커널 모듈부터 실험까지

## 목차

1. [리눅스 커널 모듈이란](#1-리눅스-커널-모듈이란)
2. [Device Mapper 프레임워크](#2-device-mapper-프레임워크)
3. [엔트리 포인트 — 커널이 우리 코드를 어떻게 찾는가](#3-엔트리-포인트--커널이-우리-코드를-어떻게-찾는가)
4. [M0 코드 상세 분석](#4-m0-코드-상세-분석)
5. [null_blk — ZNS 시뮬레이터](#5-null_blk--zns-시뮬레이터)
6. [실험 흐름 end-to-end](#6-실험-흐름-end-to-end)
7. [스크립트 상세 분석](#7-스크립트-상세-분석)
7. [진단 치트시트](#7-진단-치트시트)

---

## 1. 리눅스 커널 모듈이란

### 커널에 코드를 꽂는 방법

리눅스 커널은 실행 중에 기능을 추가하거나 제거할 수 있다. 이 단위가 **커널 모듈(`.ko` 파일)** 이다. 소켓처럼 꽂고 빼는 개념이며, 기존 커널 코드를 수정하거나 재컴파일하지 않는다.

```
리눅스 커널 (실행 중)
├── 블록 I/O 서브시스템
├── 파일시스템 레이어
├── Device Mapper 프레임워크   ← 이미 내장
│   └── [타입 등록 슬롯들]    ← 여기에 우리 코드를 꽂는다
└── ...

$ sudo insmod dm-zns-base.ko
→ 커널이 파일을 로드 → zns_base_init() 자동 호출
→ dm_register_target()으로 "zns-base" 타입 등록 완료
```

### 커널 소스 vs 커널 헤더

모듈을 개발할 때 커널 소스 전체를 읽을 필요는 없다. 두 레벨로 나뉜다.

| 레벨 | 무엇을 보는가 | 위치 |
|---|---|---|
| **모듈 개발** (우리 수준) | **헤더 파일** — 구조체·함수 선언(계약서) | `/usr/src/linux-headers-$(uname -r)/include/linux/device-mapper.h` |
| **프레임워크 내부 이해** | 커널 소스 — 실제 구현 | `drivers/md/dm.c`, `dm-target.c` 등 |

헤더 파일이 "이 콜백을 채우면 이 상황에 호출된다"는 약속을 정의한다. 선언만 알면 쓸 수 있다. 더 깊이 알고 싶을 때 커널 소스를 찾아보는 구조다.

---

## 2. Device Mapper 프레임워크

### DM이 하는 일

DM은 리눅스 커널에 내장된 **블록 디바이스 중간 계층**이다. 가상 블록 디바이스(`/dev/mapper/X`)를 만들고, 그 디바이스로 들어오는 모든 I/O를 등록된 타깃 코드로 중계한다.

```
[파일시스템 or 사용자 프로세스]
         ↓  read / write
[/dev/mapper/myzns-base]      ← DM이 만든 가상 블록 디바이스
         ↓
[DM 타깃: zns_base_map()]     ← 우리 코드가 BIO를 가로채서 처리
         ↓
[/dev/nullb0]                 ← 실제(또는 시뮬레이션) 디바이스
```

### BIO란?

**BIO(Block I/O)** 는 커널 내부에서 블록 I/O 요청을 표현하는 구조체다.

```c
struct bio {
    struct block_device *bi_bdev;   // 목적지 디바이스
    sector_t             bi_iter.bi_sector; // 시작 섹터 (LBA)
    unsigned int         bi_opf;    // 읽기(REQ_OP_READ) / 쓰기(REQ_OP_WRITE) 등
    // + 데이터 버퍼, 길이, 콜백 ...
};
```

파일시스템이 "섹터 100에 4 KiB 써"라고 하면 커널이 BIO를 만들어 DM으로 넘긴다. 우리는 `map()`에서 이 BIO를 받아 목적지와 섹터를 바꿔서 돌려준다.

### 콜백 테이블 구조 (`target_type`)

```c
// device-mapper.h 발췌
struct target_type {
    const char        *name;         // "zns-base" — dmsetup이 매칭하는 타입명
    uint64_t           features;     // 능력 플래그
    unsigned           version[3];
    struct module     *module;

    dm_ctr_fn          ctr;          // 생성(dmsetup create) 시 호출
    dm_dtr_fn          dtr;          // 제거(dmsetup remove) 시 호출
    dm_map_fn          map;          // BIO가 들어올 때마다 호출  ← 핵심
    dm_report_zones_fn report_zones; // blkzone report 등 zone 쿼리 시
    dm_iterate_devices_fn iterate_devices; // 큐 속성 전파 시

    /* ... 기타 선택적 콜백들 ... */

    struct list_head   list;         // 커널 내부 연결 리스트 링크 (건드리지 않음)
};
```

`list` 필드가 핵심이다. `dm_register_target()`은 이 `list`를 이용해 등록된 타입들의 연결 리스트에 우리 구조체를 이어 붙인다. `dmsetup create`가 타입명으로 검색할 때 이 리스트를 순회한다.

---

## 3. 엔트리 포인트 — 커널이 우리 코드를 어떻게 찾는가

### `module_init` / `module_exit` 매크로

```c
module_init(zns_base_init);
module_exit(zns_base_exit);
```

이 두 줄이 커널과의 유일한 약속이다. 전처리기가 펼치면 대략 다음과 같다.

```c
// <linux/module.h> 내부 매크로가 펼쳐진 결과 (단순화)
static initcall_t __initcall_zns_base_init
    __attribute__((__section__(".initcall6.init"))) = zns_base_init;
```

`.ko` 파일(ELF 형식)의 특정 섹션에 함수 포인터를 박아 넣는다. `insmod`가 이 섹션을 읽어서 자동으로 호출한다. 이 매크로만 지키면 커널 소스를 몰라도 연결된다.

### `dm_register_target` — DM 프레임워크 내부 (커널 소스 레벨)

`dm_register_target`이 하는 일을 추상화하면:

```c
// 커널 내부 (drivers/md/dm-target.c) — 실제 코드의 단순화
static LIST_HEAD(_targets);   // 등록된 타입들의 연결 리스트

int dm_register_target(struct target_type *t)
{
    list_add(&t->list, &_targets);   // 우리 구조체를 리스트에 추가
    return 0;
}
```

`dmsetup create` 시:

```c
// 커널 내부 (단순화)
struct target_type *dm_get_target_type(const char *name)
{
    list_for_each_entry(tt, &_targets, list) {
        if (strcmp(name, tt->name) == 0)
            return tt;   // "zns-base" 매칭!
    }
}

// 찾으면 우리 ctr 호출
tt->ctr(ti, argc, argv);   // = zns_base_ctr()
```

BIO가 들어오면:

```c
// 커널 내부 (drivers/md/dm.c — 단순화)
static blk_qc_t dm_submit_bio(struct bio *bio)
{
    struct dm_target *ti = /* 이 디바이스에 연결된 타깃 */;

    r = ti->type->map(ti, bio);   // = zns_base_map()

    if (r == DM_MAPIO_REMAPPED)
        submit_bio(bio);   // 바뀐 디바이스로 실제 실행
}
```

### 전체 연결 흐름

```
커널 소스 (drivers/md/dm.c 등)
    ↓ 커널 컴파일 시 내장
커널 헤더 (device-mapper.h)   ← 우리가 실제로 읽는 계약서
    ↓ #include
우리 모듈 소스 (dm-zns-base.c)
    ↓ make (커널 빌드 시스템에 위임)
dm-zns-base.ko
    ↓ insmod
실행 중인 커널 안으로 동적 링크
```

---

## 4. M0 코드 상세 분석

### 컨텍스트 구조체

```c
struct zns_base_c {
    struct dm_dev *dev;   // underlying 디바이스 핸들 (/dev/nullb0)
};
```

타깃 인스턴스 하나당 이 구조체 하나. `ctr`에서 할당하고 `ti->private`에 달아두면 `map` 등 다른 콜백에서 꺼내 쓴다.

### `zns_base_ctr` — 생성자

```c
static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
```

`echo "0 N zns-base /dev/nullb0" | dmsetup create myzns` 실행 시 호출된다.

```
"0   N   zns-base   /dev/nullb0"
 ↑   ↑   ↑           ↑
 |   |   타입명        argv[0] ← ctr이 받는 인자
 |   섹터 수
 시작 섹터
```

```c
c = kzalloc(sizeof(*c), GFP_KERNEL);   // 커널용 malloc (0으로 초기화)

ret = dm_get_device(ti, argv[0],       // "/dev/nullb0" 열기
                    dm_table_get_mode(ti->table),
                    &c->dev);          // 핸들을 c->dev에 저장

ti->private = c;           // 이후 콜백에서 꺼내 쓸 수 있도록 달기
ti->num_flush_bios = 1;    // flush BIO를 1개 허용
ti->num_discard_bios = 1;  // discard BIO를 1개 허용
```

`ti`(dm_target)는 DM 프레임워크가 주는 **이 타깃 인스턴스의 정보 덩어리**다. 여기에 설정을 쓰면 DM이 그에 맞게 동작한다.

### `zns_base_dtr` — 소멸자

```c
static void zns_base_dtr(struct dm_target *ti)
{
    struct zns_base_c *c = ti->private;
    dm_put_device(ti, c->dev);   // 디바이스 핸들 반납
    kfree(c);                    // 메모리 해제
}
```

`dmsetup remove` 시 호출. `kzalloc`으로 할당한 것은 반드시 `kfree`로 해제한다.

### `zns_base_map` — BIO 처리 (M0의 핵심, 현재는 passthrough)

```c
static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
    struct zns_base_c *c = ti->private;

    bio_set_dev(bio, c->dev->bdev);   // BIO의 목적지를 /dev/nullb0으로 변경
    return DM_MAPIO_REMAPPED;         // "주소 바꿨으니 그냥 내려보내"
}
```

지금 M0는:
1. BIO를 받는다
2. 목적지 디바이스만 `/dev/nullb0`으로 바꾼다
3. **섹터 번호(LBA)는 그대로** — `/dev/mapper/myzns`의 섹터 0 = `/dev/nullb0`의 섹터 0
4. `DM_MAPIO_REMAPPED` 반환 → 프레임워크가 알아서 실행

반환값 의미:

| 값 | 의미 |
|---|---|
| `DM_MAPIO_REMAPPED` | 주소 변경 완료, 프레임워크가 실행 |
| `DM_MAPIO_SUBMITTED` | 내가 직접 처리했으니 프레임워크는 손대지 마 |
| `DM_MAPIO_KILL` | 이 BIO를 오류로 종료 |

**M1에서 이 함수가 바뀐다.** 임의 LBA 쓰기를 받으면 → 활성 zone의 wp 위치로 섹터 번호를 바꾸고 → 매핑 테이블에 기록해야 한다.

### `zns_base_report_zones` — zone 정보 위임

```c
static int zns_base_report_zones(struct dm_target *ti,
                                 struct dm_report_zones_args *args,
                                 unsigned int nr_zones)
{
    struct zns_base_c *c = ti->private;
    return dm_report_zones(c->dev->bdev, ti->begin,
                           args->next_sector, args, nr_zones);
}
```

`blkzone report /dev/mapper/myzns` 시 커널이 이 함수를 호출한다. M0는 그냥 `/dev/nullb0`에 위임해서 zone 정보를 그대로 올려준다. M1에서는 위쪽을 conventional로 바꾸므로 이 콜백 자체를 제거한다(conventional 디바이스엔 zone이 없으니).

### `zns_base_iterate_devices` — 속성 전파

```c
static int zns_base_iterate_devices(struct dm_target *ti,
                                    iterate_devices_callout_fn fn, void *data)
{
    struct zns_base_c *c = ti->private;
    return fn(ti, c->dev, 0, ti->len, data);
}
```

DM 프레임워크가 "이 타깃 아래에 뭐가 있어?"를 물을 때 호출된다. `/dev/nullb0`의 `chunk_sectors`(= zone 크기), `zoned=host-managed` 같은 속성이 `/dev/mapper/myzns`의 sysfs에도 보이게 하는 콜백이다. 이 콜백이 없으면:

```
/sys/block/dm-0/queue/zoned         → none   (원래는 host-managed여야 함)
/sys/block/dm-0/queue/chunk_sectors → 0      (원래는 131072여야 함)
blkzone report → "unable to determine zone size"
```

### `target_type` 구조체 — 플러그인 명세서

```c
static struct target_type zns_base_target = {
    .name            = "zns-base",
    .version         = {0, 1, 0},
    .features        = DM_TARGET_ZONED_HM,   // "zoned HM 디바이스를 받겠다"
    .module          = THIS_MODULE,
    .ctr             = zns_base_ctr,
    .dtr             = zns_base_dtr,
    .map             = zns_base_map,
    .report_zones    = zns_base_report_zones,
    .iterate_devices = zns_base_iterate_devices,
};
```

`DM_TARGET_ZONED_HM` 플래그는 "이 타깃은 host-managed zoned 디바이스 위에 올라간다"는 허용 플래그다. 이 플래그가 없으면 `dmsetup create` 시점에 커널이 zoned 디바이스와의 연결을 거부한다.

### M0 전체 흐름 시각화

```
insmod dm-zns-base.ko
└─ zns_base_init()
   └─ dm_register_target(&zns_base_target)
      └─ DM 내부 리스트에 "zns-base" 등록 완료

echo "0 N zns-base /dev/nullb0" | dmsetup create myzns
└─ DM: "zns-base" 리스트 검색 → 찾음
   └─ zns_base_ctr(ti, 1, ["/dev/nullb0"])
      ├─ kzalloc → c 할당
      ├─ dm_get_device → c->dev = /dev/nullb0 핸들
      ├─ ti->private = c
      └─ /dev/mapper/myzns 생성 완료

사용자: write(fd, buf, 4096) at offset 0
└─ 커널 블록 레이어: BIO 생성 (dev=myzns, sector=0, op=WRITE)
   └─ dm_submit_bio()  [커널 내부]
      └─ zns_base_map(ti, bio)
         ├─ bio_set_dev(bio, nullb0)   ← 목적지 변경
         └─ return DM_MAPIO_REMAPPED
            └─ submit_bio(bio)         ← /dev/nullb0 sector 0에 실제 쓰기

dmsetup remove myzns
└─ zns_base_dtr(ti)
   ├─ dm_put_device(ti, c->dev)
   └─ kfree(c)

rmmod dm-zns-base
└─ zns_base_exit()
   └─ dm_unregister_target(&zns_base_target)
      └─ DM 리스트에서 제거
```

---

## 5. null_blk — ZNS 시뮬레이터

### null_blk이 뭔가

`null_blk`는 **실제 저장 매체 없이 블록 디바이스를 흉내 내는 커널 모듈**이다. 메모리를 백킹 스토어로 쓴다. 원래는 I/O 스택 벤치마크용이었지만, zoned 모드를 지원해서 ZNS SSD 없이도 ZNS 의미론(write pointer, sequential-write 제약, zone reset)을 그대로 재현한다.

```
일반 SSD / ZNS SSD   →   null_blk (메모리)
  실제 플래시 매체        메모리로 대체
  NVMe 명령 경로          커널 블록 레이어만
  수 µs ~ ms 지연         ~0 지연
  ZNS 의미론 있음         ZNS 의미론 동일하게 있음  ← 이것만 필요
```

정확성 검증(코드가 맞는가)은 null_blk에서 가능하다. 성능 측정은 FEMU나 실 SSD에서 해야 의미가 있다.

### null_blk 생성 방법 (configfs)

`nullblk-up.sh`가 하는 일을 단계별로 보면:

```bash
# 1. null_blk 모듈 로드 (레거시 자동 생성 비활성화)
modprobe null_blk nr_devices=0

# 2. configfs 마운트 (설정 파일시스템)
mount -t configfs none /sys/kernel/config

# 3. 인스턴스 디렉터리 생성
mkdir /sys/kernel/config/nullb/nullb0

# 4. 파라미터 설정
echo 1    > /sys/kernel/config/nullb/nullb0/zoned         # ZNS 모드 활성화
echo 64   > /sys/kernel/config/nullb/nullb0/zone_size     # zone 크기 (MiB)
echo 2048 > /sys/kernel/config/nullb/nullb0/size          # 총 크기 (MiB)
echo 1    > /sys/kernel/config/nullb/nullb0/memory_backed # 메모리 백킹

# 5. 전원 인가 → /dev/nullb0 생성
echo 1    > /sys/kernel/config/nullb/nullb0/power
```

결과물: `/dev/nullb0`, 2 GiB, 64 MiB zone × 32 zones.

### null_blk zone 구조

```
/dev/nullb0 (2 GiB)
├── Zone 0  [0x000000 ~ 0x01FFFF 섹터]  64 MiB  wp=0x000000 (EMPTY)
├── Zone 1  [0x020000 ~ 0x03FFFF 섹터]  64 MiB  wp=0x020000 (EMPTY)
├── ...
└── Zone 31 [0x3E0000 ~ 0x3FFFFF 섹터]  64 MiB  wp=0x3E0000 (EMPTY)
```

각 zone에는 **write pointer(wp)** 가 있다. 쓰기는 반드시 wp 위치에서만 가능하다. wp 이전 위치나 임의 위치에 쓰면 `blk_update_request: I/O error`로 거절된다. zone reset을 하면 wp가 zone 시작으로 돌아간다.

### null_blk 상태 확인

```bash
cat /sys/block/nullb0/queue/zoned          # host-managed
cat /sys/block/nullb0/queue/chunk_sectors  # 131072 (= 64 MiB / 512 B)
cat /sys/block/nullb0/queue/nr_zones       # 32

sudo blkzone report /dev/nullb0 | head -3
# start: 0x000000000, len 0x020000, cap 0x020000, wptr 0x000000 ..., zone cond:0(EMPTY)
# start: 0x000020000, len 0x020000, ...
```

---

## 6. 실험 흐름 end-to-end

### 사전 준비

```bash
# 커널 헤더 설치 (처음 한 번)
sudo apt install linux-headers-$(uname -r)

# null_blk 생성
sudo bash scripts/nullblk-up.sh

# 상태 확인
cat /sys/block/nullb0/queue/zoned   # host-managed 확인
```

### 빌드

```bash
cd src && make
# → dm-zns-base.ko 생성
# 내부적으로: make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```

Makefile이 하는 일:

```makefile
obj-m += dm-zns-base.o          # dm-zns-base.c → dm-zns-base.ko

KDIR ?= /lib/modules/$(shell uname -r)/build
all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
```

`-C $(KDIR)`: 커널 빌드 시스템 디렉터리로 이동해서 빌드. 커널 빌드 인프라(Kconfig, 심볼 테이블 등)를 그대로 활용한다.

### 자동 테스트 (`test-basic.sh`)

```bash
sudo bash scripts/test-basic.sh
```

스크립트가 수행하는 단계:

```
[1] insmod dm-zns-base.ko
[2] dmsetup create myzns-base (0 ~ N 섹터, zns-base 타입, /dev/nullb0)
    → /dev/mapper/myzns-base 생성

[3/4] blkzone report /dev/mapper/myzns-base
      → zone 정보 출력되면 OK (.report_zones + .iterate_devices 정상)

[2/4] blkzone reset -o 0 -c 1 /dev/mapper/myzns-base
      → zone 0의 wp를 0으로 리셋 (이전 실행 잔재 제거)

[3/4] dd if=/dev/zero of=/dev/mapper/myzns-base bs=4096 count=1 seek=0 oflag=direct
      → sector 0에 4 KiB 순차 쓰기 (wp=0이므로 통과해야 함)

[4/4] blkzone report 후 wp 확인
      → wptr 0x000008 (= 4 KiB = 8 섹터) 이면 OK

[종료 시 자동 cleanup]
      → dmsetup remove, rmmod
```

성공 시 출력:

```
=== ALL CHECKS PASSED ===
```

### 수동으로 각 단계 실행

자동 스크립트 없이 직접 해보고 싶을 때:

```bash
# 1. 모듈 적재
sudo insmod src/dm-zns-base.ko

# 2. DM 타깃 생성
SECTORS=$(sudo blockdev --getsz /dev/nullb0)
echo "0 $SECTORS zns-base /dev/nullb0" | sudo dmsetup create myzns

# 3. 확인
sudo dmsetup ls
sudo blkzone report /dev/mapper/myzns | head -3
cat /sys/block/dm-0/queue/zoned          # host-managed
cat /sys/block/dm-0/queue/chunk_sectors  # 131072

# 4. zone 0 리셋 후 순차 쓰기
sudo blkzone reset -o 0 -c 1 /dev/mapper/myzns
sudo dd if=/dev/zero of=/dev/mapper/myzns bs=4096 count=1 seek=0 oflag=direct

# 5. wp 확인
sudo blkzone report /dev/mapper/myzns | head -1
# wptr 0x000008 이면 성공

# 6. 정리
sudo dmsetup remove myzns
sudo rmmod dm-zns-base
sudo bash scripts/nullblk-down.sh
```

### 개발 반복 사이클

```bash
# 코드 수정 후
cd src && make && cd ..
sudo bash scripts/test-basic.sh   # 스크립트가 잔재 정리 후 재테스트
```

test-basic.sh는 실행 시작 시 기존 DM 타깃과 모듈을 자동으로 제거하므로 매번 수동 정리할 필요가 없다.

---

## 7. 진단 치트시트

### blkzone report 실패: "unable to determine zone size"

```bash
cat /sys/block/dm-0/queue/zoned          # none 이면 문제
cat /sys/block/dm-0/queue/chunk_sectors  # 0 이면 문제
```

원인: `.iterate_devices` 콜백 누락 또는 `DM_TARGET_ZONED_HM` 플래그 누락.

### dd 쓰기 실패: "0 bytes copied"

```bash
sudo dmesg | grep blk_update_request
# blk_update_request: I/O error, dev nullb0, sector X op 0x1:(WRITE)
```

원인: zone의 wp가 sector X가 아닌 위치. 해결:

```bash
sudo blkzone reset -o 0 -c 1 /dev/mapper/myzns   # zone 0 리셋
```

### insmod 실패: "Invalid module format"

빌드한 커널 헤더 버전과 실행 중인 커널 버전 불일치.

```bash
uname -r
modinfo src/dm-zns-base.ko | grep vermagic
# 두 값이 다르면 재부팅 또는 올바른 헤더로 재빌드
```

### dmsetup create 실패

```bash
sudo dmesg | tail -20   # ctr 거부 이유 확인
```

### 유용한 dmesg 필터

```bash
sudo dmesg | grep -E 'zns-base|device-mapper|null_blk|blk_update_request' | tail -30
```

---

---

## 7. 스크립트 상세 분석

### 전체 스크립트 목록

| 스크립트 | 역할 | 실행 권한 |
|---|---|---|
| `scripts/nullblk-up.sh` | zoned null_blk 디바이스 생성 | root |
| `scripts/nullblk-down.sh` | null_blk 디바이스 제거 | root |
| `scripts/test-basic.sh` | M0 smoke test (빌드 → 적재 → 검증 → 정리) | root |
| `scripts/setup-sudo.sh` | 위 스크립트를 패스워드 없이 실행하도록 sudoers 설정 | root |

---

### `nullblk-up.sh` — zoned null_blk 생성

**목적**: `/dev/nullb0`을 host-managed zoned 블록 디바이스로 만든다.

```bash
#!/usr/bin/env bash
set -uo pipefail

NB_NAME=${NB_NAME:-nullb0}          # 환경변수로 오버라이드 가능
NB_SIZE_MB=${NB_SIZE_MB:-2048}      # 총 크기 (기본 2 GiB)
NB_ZONE_SIZE_MB=${NB_ZONE_SIZE_MB:-64}  # zone 크기 (기본 64 MiB)

DEV="/dev/$NB_NAME"
CONFIG="/sys/kernel/config/nullb/$NB_NAME"
```

**단계별 동작**:

```
[1] root 확인
    id -u == 0이 아니면 즉시 종료

[2] 이미 실행 중인지 확인
    /dev/nullb0 이 존재하고 queue/zoned == "host-managed" 이면 → 아무것도 안 하고 종료
    (멱등성: 두 번 실행해도 안전)

[3] null_blk 모듈 로드
    modprobe null_blk nr_devices=0
    - nr_devices=0 : 레거시 방식으로 자동 생성하지 않음
    - configfs를 통해 수동으로 인스턴스를 만들 것이기 때문

[4] configfs 마운트
    mount -t configfs none /sys/kernel/config
    (이미 마운트돼 있으면 건너뜀)

[5] 기존 stale 인스턴스 제거
    /sys/kernel/config/nullb/nullb0 디렉터리가 있으면
    → power=0 쓰기 후 rmdir

[6] 새 인스턴스 파라미터 설정
    mkdir /sys/kernel/config/nullb/nullb0
    echo 1    → zoned          (ZNS 모드 활성화)
    echo 64   → zone_size      (MiB 단위)
    echo 2048 → size           (MiB 단위)
    echo 1    → memory_backed  (RAM을 스토리지로 사용)
    echo 1    → power          (이 순간 /dev/nullb0 생성됨)

[7] 디바이스 출현 확인
    sleep 0.2 후 /dev/nullb0 존재 여부 체크

[8] 상태 출력
    zoned / chunk_sectors / nr_zones / size 출력
```

실행 예시:

```bash
sudo bash scripts/nullblk-up.sh
# [OK] /dev/nullb0 is ready
#     zoned         = host-managed
#     chunk_sectors = 131072
#     nr_zones      = 32
#     size(sectors) = 4194304

# 커스텀 파라미터
sudo NB_SIZE_MB=4096 NB_ZONE_SIZE_MB=128 bash scripts/nullblk-up.sh
```

---

### `nullblk-down.sh` — null_blk 제거

**목적**: `nullblk-up.sh`로 만든 null_blk 인스턴스를 해제하고, 인스턴스가 하나도 남지 않으면 모듈도 언로드한다.

```bash
#!/usr/bin/env bash
set -uo pipefail

NB_NAME=${NB_NAME:-nullb0}
CONFIG="/sys/kernel/config/nullb/$NB_NAME"
```

**단계별 동작**:

```
[1] root 확인

[2] configfs 디렉터리 존재 확인
    /sys/kernel/config/nullb/nullb0 가 있으면:
    → echo 0 > power    (디바이스 전원 끄기 → /dev/nullb0 사라짐)
    → rmdir             (configfs 항목 삭제)

[3] 남은 인스턴스 확인
    /sys/kernel/config/nullb/ 가 비어 있으면 (다른 null_blk 인스턴스 없음)
    → rmmod null_blk    (모듈 언로드)
    아직 사용 중이면 "(still in use)" 출력하고 넘어감
```

실행 예시:

```bash
sudo bash scripts/nullblk-down.sh
# [*] Powering off nullb0
# [*] Removed nullb0 from configfs
# [*] No instances left; unloading null_blk
# [OK] Done.
```

`nullblk-up.sh`와 대칭적으로 설계돼 있다. 두 스크립트를 반복 실행해도 항상 안전하다.

---

### `test-basic.sh` — M0 smoke test

**목적**: 빌드 → 모듈 적재 → DM 타깃 생성 → 4단계 검증 → 자동 정리. M0의 성공 기준을 한 번에 확인한다.

```bash
UNDERLYING=${UNDERLYING:-/dev/nullb0}
DM_NAME=${DM_NAME:-myzns-base}
DM_DEV=/dev/mapper/$DM_NAME
MOD_NAME=dm-zns-base

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../src" && pwd)
KO_PATH="$SRC_DIR/$MOD_NAME.ko"
```

**단계별 동작**:

```
[사전] trap cleanup EXIT 등록
       스크립트가 어떤 이유로든 종료되면 cleanup() 자동 실행
       cleanup(): dmsetup remove + rmmod (잔재 없이 깨끗하게)

[0] 사전 조건 확인
    - root인지
    - /dev/nullb0 존재하는지 (없으면 nullblk-up.sh 먼저 실행하라고 안내)
    - dm-zns-base.ko 없으면 make 자동 실행

[준비] 기존 잔재 제거
    dmsetup remove myzns-base  (이미 있으면 제거)
    rmmod dm-zns-base          (이미 로드돼 있으면 제거)
    → 반복 실행 시 충돌 방지

[적재] insmod dm-zns-base.ko
[생성] blockdev --getsz /dev/nullb0 으로 섹터 수 확인
       echo "0 $sectors zns-base /dev/nullb0" | dmsetup create myzns-base

[1/4] blkzone report /dev/mapper/myzns-base | head -3
      zone 목록이 출력되면 OK
      실패하면 → .report_zones / .iterate_devices 문제

[2/4] blkzone reset -o 0 -c 1 /dev/mapper/myzns-base
      zone 0의 write pointer를 0으로 리셋
      반복 실행 시 이전 테스트의 wp 위치가 남아있을 수 있어서 필요

[3/4] dd if=/dev/zero of=/dev/mapper/myzns-base bs=4096 count=1 seek=0 oflag=direct
      - seek=0    : 파일 오프셋 0 (= 섹터 0) 에 쓰기
      - oflag=direct : 페이지 캐시 우회, 블록 레이어 직접 호출
      wp=0이므로 순차 쓰기 조건 만족 → 통과해야 함

[4/4] blkzone report | head -1 로 wp 확인
      "wptr 0x000008" 문자열이 있으면 OK
      (4 KiB = 8 섹터 → wp가 8 증가했다는 뜻)

[EXIT] trap이 발동 → cleanup() 실행
       dmsetup remove myzns-base
       rmmod dm-zns-base
```

실행 흐름 시각화:

```
nullblk-up.sh (사전)
       ↓
test-basic.sh 실행
       ├─ insmod dm-zns-base.ko
       ├─ dmsetup create myzns-base
       │
       ├─ [1/4] blkzone report   → zone 목록 보이면 OK
       ├─ [2/4] blkzone reset    → wp = 0
       ├─ [3/4] dd write 4KiB   → 섹터 0에 순차 쓰기
       └─ [4/4] blkzone report   → wp = 8 이면 OK
       │
       └─ EXIT trap → dmsetup remove + rmmod (항상 실행)
```

`set -uo pipefail` 의미:
- `-u`: 미정의 변수 참조 시 즉시 오류
- `-o pipefail`: 파이프 중 하나라도 실패하면 전체 실패
- (`-e`는 없음: 일부 명령 실패를 의도적으로 허용하는 구간이 있기 때문)

---

### `setup-sudo.sh` — sudoers 설정

**목적**: scripts/*.sh를 실행할 때마다 비밀번호를 입력하지 않도록 `/etc/sudoers.d/dm-zns-base` 파일을 설치한다. 선택 사항이며 개발 편의용이다.

**단계별 동작**:

```
[1] root 확인

[2] 현재 사용자 이름 결정
    SUDO_USER 환경변수 → logname → 기본값 "splab" 순서로 시도
    (sudo로 실행되면 SUDO_USER에 원래 사용자명이 들어 있음)

[3] sudoers 내용을 임시 파일에 작성
    $USER ALL=(root) NOPASSWD: /usr/bin/bash .../nullblk-up.sh
    $USER ALL=(root) NOPASSWD: /usr/bin/bash .../nullblk-down.sh
    $USER ALL=(root) NOPASSWD: /usr/bin/bash .../test-basic.sh

[4] visudo -c -f $TMP 로 문법 검사
    문법 오류 시 sudoers 파일 깨짐을 방지하기 위해 반드시 검사 후 설치

[5] install -m 0440 으로 /etc/sudoers.d/dm-zns-base 에 복사
    권한 0440 (root만 읽기) — sudoers 파일의 요구 사항
```

실행 예시:

```bash
sudo bash scripts/setup-sudo.sh
# [OK] Installed /etc/sudoers.d/dm-zns-base (for user: splab)

# 이후 비밀번호 없이 실행 가능
bash scripts/nullblk-up.sh    # sudo 없이도 내부에서 sudo가 패스워드 안 물어봄
```

제거할 때:

```bash
sudo rm /etc/sudoers.d/dm-zns-base
```

---

### 스크립트 간 의존 관계

```
setup-sudo.sh          (선택, 최초 1회)
      ↓
nullblk-up.sh          ← /dev/nullb0 준비
      ↓
test-basic.sh          ← 빌드 + 적재 + 검증 (반복 실행)
      ↓
nullblk-down.sh        ← 실험 종료 후 정리
```

코드 수정 → 재테스트 사이클에서는 `nullblk-up.sh`와 `nullblk-down.sh`를 매번 실행하지 않아도 된다. `test-basic.sh`가 잔재를 스스로 정리하고 재적재하기 때문이다. null_blk은 테스트 간 zone 상태가 남아있지만 `blkzone reset` 단계([2/4])에서 초기화한다.

---

## M0 → M1 전환 시 바뀌는 것 (예고)

| 항목 | M0 (현재) | M1 (과제) |
|---|---|---|
| 위쪽 face | zoned (HM) | conventional |
| `.features` | `DM_TARGET_ZONED_HM` | 제거 |
| `.report_zones` | 있음 (위임) | 제거 |
| `.iterate_devices` | 있음 (속성 전파) | 수정 (zone 속성 노출 안 함) |
| `zns_base_map` 쓰기 | 그대로 통과 | 임의 LBA → 활성 zone wp에 append + 매핑 테이블 갱신 |
| `zns_base_map` 읽기 | 그대로 통과 | 매핑 테이블 lookup → underlying 좌표로 변환 |
| 매핑 자료구조 | 없음 | LSM-Tree 또는 sorted log |
