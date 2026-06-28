# 12. M0 실습 — 스켈레톤

`11-m0-practice-guide.md`를 읽은 다음, 이 파일의 `TODO`를 직접 채운다.  
막혔을 때만 가이드를 참고한다.

---

## DM 타깃 콜백 개요

DM 타깃은 `struct target_type`에 콜백 함수 포인터를 채워서 DM 프레임워크와 계약을 맺는다.  
각 콜백이 언제, 왜 호출되는지 이해하고 구현에 들어간다.

| 콜백 | 언제 호출되나 | 역할 |
|---|---|---|
| `ctr` (constructor) | `dmsetup create` 시 한 번 | 타깃 인스턴스를 초기화한다. underlying 디바이스를 열고, 필요한 자료구조를 할당하고, `ti->private`에 달아둔다. |
| `dtr` (destructor) | `dmsetup remove` 시 한 번 | `ctr`에서 열고 할당한 것을 역순으로 해제한다. |
| `map` | BIO가 들어올 때마다 | 가상 디바이스로 들어온 I/O 요청을 받아서 실제로 어느 디바이스의 어느 섹터로 보낼지 결정한다. DM 타깃의 핵심 로직이 여기 들어간다. |
| `report_zones` | `blkzone report` 등 zone 쿼리 시 | 위쪽에 zone 정보를 돌려준다. zoned 디바이스를 그대로 노출하는 경우 underlying에 위임하면 된다. |
| `iterate_devices` | DM 프레임워크가 큐 속성을 수집할 때 | 이 타깃 아래에 있는 디바이스를 프레임워크에 알려준다. `chunk_sectors`, `zoned` 같은 속성이 sysfs에 올바르게 노출되려면 이 콜백이 있어야 한다. |

---

## 디렉터리 구성

```
~/dm-zns-base/m0/
├── Makefile       ← 커널 빌드 시스템에 위임하도록 작성
└── dm-myzns.c     ← 아래 스켈레톤을 채운다
```

---

## Makefile

```makefile
# TODO: obj-m에 빌드할 오브젝트를 지정하고,
#       커널 빌드 시스템 디렉터리(KDIR)를 이용해 빌드·클린 타깃을 작성한다.
```

---

## dm-myzns.c

```c
// SPDX-License-Identifier: GPL-2.0

/* TODO: 필요한 헤더를 include한다. */

#define DM_MSG_PREFIX "myzns"

/* TODO: 컨텍스트 구조체를 정의한다.
 *
 * - 이 구조체는 타깃 인스턴스 하나당 하나씩 만들어진다.
 * - ctr에서 할당하고 ti->private에 달면 map 등 모든 콜백에서 꺼내 쓸 수 있다.
 * - M0 pass-through에 필요한 필드가 무엇인지 생각해보자.
 */

/* ctr — dmsetup create 시 한 번 호출된다.
 *
 * 아래 명령을 실행하면 ctr이 호출된다:
 *   echo "0 $SECTORS myzns /dev/nullb0" | dmsetup create mymapper
 *
 * 이때 인자들이 어떻게 매핑되는지:
 *   "0 $SECTORS myzns  /dev/nullb0"
 *    ↑  ↑       ↑       ↑
 *    |  ti->len 타입명  argv[0]  ← ctr이 받는 인자
 *    ti->begin
 *
 * 구현 순서:
 *   1. argc를 검사한다. 우리 타깃은 인자를 몇 개 받아야 하는가?
 *      잘못된 인자가 오면 ti->error에 메시지를 담고 음수 에러코드를 반환한다.
 *
 *   2. 컨텍스트 구조체를 커널 힙에 할당한다.
 *      커널에서 메모리를 할당하는 함수는 무엇인가? (힌트: kzalloc)
 *      할당 실패 시 어떻게 처리해야 하는가?
 *
 *   3. argv[0]으로 넘어온 디바이스를 열어서 구조체에 저장한다.
 *      DM에서 디바이스를 여는 헬퍼 함수는 무엇인가? (힌트: dm_get_device)
 *      실패 시 2번에서 할당한 것을 어떻게 해야 하는가?
 *
 *   4. ti->private에 구조체를 달아서 이후 콜백에서 꺼내 쓸 수 있게 한다.
 *
 *   5. flush/discard BIO를 통과시키려면 ti의 어떤 필드를 설정해야 하는가?
 *
 * 에러 경로 원칙: 자원은 반드시 획득한 역순으로 해제한다.
 *   kzalloc → dm_get_device 순으로 획득했다면,
 *   dm_get_device 실패 시 → kfree 후 반환
 */
static int myzns_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    /* TODO */
    return 0;
}

/* dtr — dmsetup remove 시 한 번 호출된다.
 *
 * ctr의 정확한 역순이다. ctr에서 자원을 획득한 순서가
 *   kzalloc → dm_get_device → ti->private = c
 * 였다면, dtr에서는
 *   dm_put_device → kfree
 * 순으로 해제한다.
 *
 * 구현 순서:
 *   1. ti->private을 꺼내 컨텍스트 구조체 포인터로 캐스팅한다.
 *   2. dm_get_device로 열었던 디바이스를 닫는다. (힌트: dm_put_device)
 *   3. kzalloc으로 할당했던 구조체를 해제한다. (힌트: kfree)
 */
static void myzns_dtr(struct dm_target *ti)
{
    /* TODO */
}

/* map — BIO가 들어올 때마다 호출된다.
 *
 * BIO(Block I/O)는 커널이 블록 I/O 요청을 표현하는 구조체다.
 * 파일시스템이 "섹터 N에 4 KiB 써"라고 하면 커널이 BIO를 만들어 여기로 넘긴다.
 * 우리는 이 BIO를 보고 "실제로 어느 디바이스의 어느 섹터로 보낼지" 결정한다.
 *
 * BIO의 주요 필드:
 *   bio->bi_iter.bi_sector  — 요청된 시작 섹터 (LBA)
 *   bio->bi_opf             — 읽기(REQ_OP_READ) / 쓰기(REQ_OP_WRITE) 등
 *
 * M0 목표: pass-through.
 *   1. ti->private에서 컨텍스트 구조체를 꺼낸다.
 *   2. BIO의 목적지 디바이스를 underlying 디바이스로 바꾼다. (힌트: bio_set_dev)
 *      섹터 번호(bi_sector)는 건드리지 않는다 — 1:1 매핑이므로.
 *   3. 적절한 반환값으로 프레임워크가 이어서 처리하게 한다.
 *
 * 반환값 종류:
 *   DM_MAPIO_REMAPPED  — 목적지를 바꿨으니 프레임워크가 submit해라
 *   DM_MAPIO_SUBMITTED — 내가 직접 submit했으니 프레임워크는 손대지 마라
 *   DM_MAPIO_KILL      — 이 BIO를 오류로 종료해라
 *
 * M1에서는 이 함수가 완전히 달라진다.
 * 임의 LBA 쓰기를 받으면 bi_sector를 활성 zone의 wp로 바꾸고
 * 매핑 테이블에 기록해야 한다.
 */
static int myzns_map(struct dm_target *ti, struct bio *bio)
{
    /* TODO */
    return DM_MAPIO_REMAPPED;
}

/* report_zones — blkzone report 등 zone 쿼리 시 호출된다.
 *
 * - zone 정보를 직접 생성하지 않고 underlying 디바이스에 위임한다.
 * - 이 콜백이 없거나 잘못 구현되면 blkzone report가 실패한다.
 */
static int myzns_report_zones(struct dm_target *ti,
                               struct dm_report_zones_args *args,
                               unsigned int nr_zones)
{
    /* TODO */
    return 0;
}

/* iterate_devices — DM 프레임워크가 하위 디바이스 정보를 물을 때 호출된다.
 *
 * - 이 콜백이 없으면 /sys/block/dm-X/queue/zoned, chunk_sectors 등이
 *   올바르게 노출되지 않아 blkzone report가 "unable to determine zone size"로 실패한다.
 */
static int myzns_iterate_devices(struct dm_target *ti,
                                  iterate_devices_callout_fn fn, void *data)
{
    /* TODO */
    return 0;
}

/* TODO: target_type 구조체를 채운다.
 *
 * - .name: dmsetup create 명령에서 사용할 타입 문자열
 * - .features: zoned HM 디바이스 위에 올라가려면 어떤 플래그가 필요한가?
 *   (이 플래그가 없으면 dmsetup create 시 커널이 연결을 거부한다)
 * - 위에서 작성한 콜백들을 연결한다.
 */
static struct target_type myzns_target = {
    /* TODO */
};

/* TODO: init / exit 함수를 작성하고 module_init / module_exit으로 등록한다.
 *
 * init: dm_register_target()으로 myzns_target을 DM 프레임워크에 등록
 * exit: dm_unregister_target()으로 등록 해제
 */

MODULE_DESCRIPTION("My ZNS pass-through DM target");
MODULE_AUTHOR("your name");
MODULE_LICENSE("GPL");
```

---

## 테스트 체크리스트

구현 후 아래를 순서대로 확인한다.

```bash
# 이전 실행 잔재 정리 (재실행 시 충돌 방지)
sudo dmsetup remove mymapper 2>/dev/null
sudo rmmod dm-myzns 2>/dev/null

cd ~/dm-zns-base/m0 && make
sudo insmod dm-myzns.ko

SECTORS=$(sudo blockdev --getsz /dev/nullb0)
echo "0 $SECTORS myzns /dev/nullb0" | sudo dmsetup create mymapper
```

- [ ] `cat /sys/block/dm-0/queue/zoned` → `host-managed`
- [ ] `cat /sys/block/dm-0/queue/chunk_sectors` → `131072`
- [ ] `sudo blkzone report /dev/mapper/mymapper | head -3` → zone 목록 출력
- [ ] `sudo blkzone reset -o 0 -c 1 /dev/mapper/mymapper` → 성공
- [ ] `sudo dd if=/dev/zero of=/dev/mapper/mymapper bs=4096 count=1 seek=0 oflag=direct` → 1 record out
- [ ] `sudo blkzone report /dev/mapper/mymapper | head -1` → `wptr 0x000008`

```bash
sudo dmsetup remove mymapper
sudo rmmod dm-myzns
```
