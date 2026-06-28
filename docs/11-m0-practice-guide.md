# 11. M0 직접 구현 실습 가이드

scaffold(`src/dm-zns-base.c`)를 참고 없이 처음부터 작성해보는 실습이다.  
각 콜백을 직접 타이핑하면서 "왜 이게 필요한가"를 체득하는 것이 목표다.

---

## 목차

1. [사전 준비](#1-사전-준비)
2. [Step 0 — 디렉터리와 Makefile](#2-step-0--디렉터리와-makefile)
3. [Step 1 — 헤더와 기본 뼈대](#3-step-1--헤더와-기본-뼈대)
4. [Step 2 — 컨텍스트 구조체](#4-step-2--컨텍스트-구조체)
5. [Step 3 — ctr (생성자)](#5-step-3--ctr-생성자)
6. [Step 4 — dtr (소멸자)](#6-step-4--dtr-소멸자)
7. [Step 5 — map (BIO 처리)](#7-step-5--map-bio-처리)
8. [Step 6 — report_zones](#8-step-6--report_zones)
9. [Step 7 — iterate_devices](#9-step-7--iterate_devices)
10. [Step 8 — target_type 구조체](#10-step-8--target_type-구조체)
11. [Step 9 — init / exit](#11-step-9--init--exit)
12. [Step 10 — 빌드 및 테스트](#12-step-10--빌드-및-테스트)
13. [자주 막히는 지점](#13-자주-막히는-지점)

---

## 1. 사전 준비

```bash
# 커널 헤더 (최초 1회)
sudo apt install linux-headers-$(uname -r)

# null_blk ZNS 디바이스 생성
sudo bash scripts/nullblk-up.sh

# 상태 확인
cat /sys/block/nullb0/queue/zoned   # host-managed 이어야 함
```

---

## 2. Step 0 — 디렉터리와 Makefile

```bash
mkdir ~/dm-zns-base/m0 && cd ~/dm-zns-base/m0
```

`Makefile` 생성:

```makefile
obj-m += dm-myzns.o

KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

`obj-m += dm-myzns.o` 한 줄이 핵심이다. 커널 빌드 시스템(`-C $(KDIR)`)이 이 변수를 읽어서 `dm-myzns.c`를 `dm-myzns.ko`로 컴파일한다.

---

## 3. Step 1 — 헤더와 기본 뼈대

`dm-myzns.c`를 새로 만들고 아래를 먼저 작성한다.

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "myzns"
```

| 헤더 | 이유 |
|---|---|
| `module.h` | `module_init`, `module_exit`, `MODULE_*` 매크로 |
| `init.h` | `__init`, `__exit` 어노테이션 |
| `bio.h` | `struct bio`, `bio_set_dev()` |
| `device-mapper.h` | `struct target_type`, `dm_register_target()` 등 DM API 전부 |

`DM_MSG_PREFIX`는 `DMINFO()`, `DMERR()` 매크로가 출력할 때 앞에 붙는 태그다.  
`dmesg | grep myzns` 로 우리 모듈 로그만 필터링할 수 있다.

---

## 4. Step 2 — 컨텍스트 구조체

타깃 인스턴스 하나당 상태를 보관하는 구조체다.

```c
struct myzns_c {
    struct dm_dev *dev;   /* underlying 디바이스 핸들 (/dev/nullb0) */
};
```

M0는 pass-through라 `dev` 하나면 충분하다.  
M1에서 이 구조체에 매핑 테이블, write pointer 캐시 등이 추가된다.

---

## 5. Step 3 — ctr (생성자)

`dmsetup create` 시 한 번 호출된다.

```c
static int myzns_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    struct myzns_c *c;
    int ret;

    if (argc != 1) {
        ti->error = "expected one argument: underlying device";
        return -EINVAL;
    }

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        ti->error = "out of memory";
        return -ENOMEM;
    }

    ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
    if (ret) {
        ti->error = "failed to open underlying device";
        kfree(c);
        return ret;
    }

    ti->private = c;
    ti->num_flush_bios = 1;
    ti->num_discard_bios = 1;

    DMINFO("attached on top of '%s'", argv[0]);
    return 0;
}
```

**`dmsetup create` 명령과 인자의 관계**:

```
echo "0 $SECTORS myzns /dev/nullb0" | dmsetup create mymapper
              ↑                ↑
          ti->len           argv[0]  ← ctr 이 받는 인자
```

**주의**: `kzalloc` 성공 후 `dm_get_device`가 실패하면 `kfree(c)`를 하고 리턴해야 한다.  
에러 경로에서 역순으로 해제하지 않으면 메모리 누수가 생긴다.

---

## 6. Step 4 — dtr (소멸자)

`dmsetup remove` 시 호출된다.

```c
static void myzns_dtr(struct dm_target *ti)
{
    struct myzns_c *c = ti->private;

    dm_put_device(ti, c->dev);   /* dm_get_device와 짝 */
    kfree(c);                    /* kzalloc과 짝 */
    DMINFO("detached");
}
```

`ctr`에서 열었던 것을 정확히 역순으로 닫는다.

---

## 7. Step 5 — map (BIO 처리)

BIO가 들어올 때마다 호출되는 핵심 콜백이다.  
M0는 pass-through이므로 목적지 디바이스만 바꾼다.

```c
static int myzns_map(struct dm_target *ti, struct bio *bio)
{
    struct myzns_c *c = ti->private;

    bio_set_dev(bio, c->dev->bdev);
    return DM_MAPIO_REMAPPED;
}
```

**반환값 의미**:

| 값 | 의미 |
|---|---|
| `DM_MAPIO_REMAPPED` | 주소 변경 완료, DM 프레임워크가 submit |
| `DM_MAPIO_SUBMITTED` | 내가 직접 submit했음, 프레임워크는 손대지 마 |
| `DM_MAPIO_KILL` | 이 BIO를 오류로 종료 |

지금 M0에서는 `bio->bi_iter.bi_sector`(섹터 번호)를 건드리지 않는다.  
`/dev/mapper/mymapper`의 섹터 N = `/dev/nullb0`의 섹터 N, 1:1 매핑이다.

**M1에서 이 함수가 완전히 달라진다**: 임의 LBA 쓰기를 받으면 활성 zone의 wp 위치로 섹터 번호를 바꾸고 매핑 테이블을 갱신해야 한다.

---

## 8. Step 6 — report_zones

`blkzone report /dev/mapper/mymapper` 시 커널이 이 함수를 호출한다.  
M0는 `/dev/nullb0`에 그대로 위임한다.

```c
static int myzns_report_zones(struct dm_target *ti,
                               struct dm_report_zones_args *args,
                               unsigned int nr_zones)
{
    struct myzns_c *c = ti->private;

    return dm_report_zones(c->dev->bdev, ti->begin,
                           args->next_sector, args, nr_zones);
}
```

M1에서는 위쪽을 conventional로 바꾸므로 이 콜백 자체가 제거된다.  
conventional 디바이스엔 zone이 없기 때문이다.

---

## 9. Step 7 — iterate_devices

DM 프레임워크가 "이 타깃 아래에 뭐가 있어?"를 물을 때 호출된다.  
`/dev/nullb0`의 `chunk_sectors`(zone 크기)와 `zoned=host-managed` 속성이  
`/dev/mapper/mymapper`의 sysfs에도 보이게 하는 콜백이다.

```c
static int myzns_iterate_devices(struct dm_target *ti,
                                  iterate_devices_callout_fn fn, void *data)
{
    struct myzns_c *c = ti->private;

    return fn(ti, c->dev, 0, ti->len, data);
}
```

**이 콜백이 없으면**:

```bash
cat /sys/block/dm-0/queue/zoned          # none  (원래 host-managed 여야 함)
cat /sys/block/dm-0/queue/chunk_sectors  # 0     (원래 131072 여야 함)
blkzone report /dev/mapper/mymapper      # "unable to determine zone size"
```

---

## 10. Step 8 — target_type 구조체

지금까지 만든 콜백들을 하나로 묶는 **플러그인 명세서**다.

```c
static struct target_type myzns_target = {
    .name            = "myzns",
    .version         = {0, 1, 0},
    .features        = DM_TARGET_ZONED_HM,
    .module          = THIS_MODULE,
    .ctr             = myzns_ctr,
    .dtr             = myzns_dtr,
    .map             = myzns_map,
    .report_zones    = myzns_report_zones,
    .iterate_devices = myzns_iterate_devices,
};
```

`DM_TARGET_ZONED_HM`: "이 타깃은 host-managed zoned 디바이스 위에 올라간다"는 허용 플래그.  
이 플래그가 없으면 `dmsetup create` 시점에 커널이 zoned 디바이스와의 연결을 거부한다.

`.name = "myzns"` 가 `dmsetup create`에서 타입명으로 사용된다:

```bash
echo "0 $SECTORS myzns /dev/nullb0" | dmsetup create mymapper
#                 ↑↑↑↑↑
#                 이 문자열로 DM이 리스트에서 우리 target_type을 검색
```

---

## 11. Step 9 — init / exit

모듈 로드(`insmod`)와 언로드(`rmmod`) 시 호출되는 진입점이다.

```c
static int __init myzns_init(void)
{
    int ret = dm_register_target(&myzns_target);

    if (ret < 0)
        DMERR("registration failed: %d", ret);
    else
        DMINFO("registered");
    return ret;
}

static void __exit myzns_exit(void)
{
    dm_unregister_target(&myzns_target);
    DMINFO("unregistered");
}

module_init(myzns_init);
module_exit(myzns_exit);

MODULE_DESCRIPTION("My ZNS pass-through DM target");
MODULE_AUTHOR("your name");
MODULE_LICENSE("GPL");
```

`module_init` / `module_exit` 매크로가 `.ko` 파일의 특정 ELF 섹션에 함수 포인터를 박아 넣는다.  
`insmod`가 이 섹션을 읽어서 자동으로 호출하기 때문에, 우리는 이 두 매크로만 지키면 된다.

---

## 12. Step 10 — 빌드 및 테스트

### 빌드

```bash
cd ~/dm-zns-base/m0
make
# dm-myzns.ko 생성 확인
```

### 수동 테스트

```bash
# 이전 실행 잔재 정리 (재실행 시 충돌 방지)
sudo dmsetup remove mymapper 2>/dev/null
sudo rmmod dm-myzns 2>/dev/null

# 모듈 적재
sudo insmod dm-myzns.ko

# DM 타깃 생성
SECTORS=$(sudo blockdev --getsz /dev/nullb0)
echo "0 $SECTORS myzns /dev/nullb0" | sudo dmsetup create mymapper

# 속성 확인
cat /sys/block/dm-0/queue/zoned          # host-managed
cat /sys/block/dm-0/queue/chunk_sectors  # 131072

# zone 목록 확인
sudo blkzone report /dev/mapper/mymapper | head -3

# zone 0 리셋 후 순차 쓰기
sudo blkzone reset -o 0 -c 1 /dev/mapper/mymapper
sudo dd if=/dev/zero of=/dev/mapper/mymapper bs=4096 count=1 seek=0 oflag=direct

# write pointer가 8로 증가했는지 확인
sudo blkzone report /dev/mapper/mymapper | head -1
# wptr 0x000008  → 성공 (4 KiB = 8 섹터)

# 정리
sudo dmsetup remove mymapper
sudo rmmod dm-myzns
```

---

## 13. 자주 막히는 지점

| 증상 | 원인 | 해결 |
|---|---|---|
| `insmod` 실패 | 커널 버전 불일치 | `uname -r` vs `modinfo dm-myzns.ko \| grep vermagic` 비교 |
| `dmsetup create` 실패 | `DM_TARGET_ZONED_HM` 누락 | `target_type.features` 확인 |
| `blkzone report` zone 정보 없음 | `iterate_devices` / `report_zones` 콜백 누락 | Step 6·7 확인 |
| `dd` 실패 (0 bytes copied) | wp가 0이 아닌 위치 | `blkzone reset` 먼저 실행 |
| `rmmod` 실패 "Module is in use" | DM 타깃이 아직 살아 있음 | `dmsetup remove mymapper` 먼저 실행 |

```bash
# 로그 필터
sudo dmesg | grep -E 'myzns|device-mapper|blk_update_request' | tail -20
```

---

## scaffold와 비교 포인트

직접 구현 후 `src/dm-zns-base.c`와 대조할 때 확인할 것:

- `DM_MSG_PREFIX` 값만 다르고 구조는 동일한가?
- `ctr`의 에러 경로 순서가 맞는가? (`kzalloc` → `dm_get_device` → `ti->private`)
- `map`에서 `bio->bi_iter.bi_sector`를 건드리지 않고 `bio_set_dev`만 호출하는가?
- `iterate_devices`에서 `ti->len`(타깃 길이)을 정확히 전달하는가?
