// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "myzns"

/* TODO: 컨텍스트 구조체를 정의한다.
 *
 * - 이 구조체는 타깃 인스턴스 하나당 하나씩 만들어진다.
 * - ctr에서 할당하고 ti->private에 달면 map 등 모든 콜백에서 꺼내 쓸 수 있다.
 * - M0 pass-through에 필요한 필드가 무엇인지 생각해보자.
 */
struct myzns_c {
    struct dm_dev *dev; /* DM 프레임워크가 관리하는 디바이스 핸들.
                         * 실제 블록 디바이스(/dev/nullb0)를 가리키며,
                         * dm_get_device()로 열고 dm_put_device()로 닫는다. */
};

/* ctr — dmsetup create 시 한 번 호출된다.
 *
 * - argv[0]에 underlying 디바이스 경로가 넘어온다.
 * - 에러 발생 시 그 시점까지 할당한 자원을 역순으로 해제해야 한다.
 */
static int myzns_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    struct myzns_c *c;
    int ret;

    /* argv 개수 검사. "0 N myzns /dev/nullb0" 에서 /dev/nullb0 하나만 받으므로 1이어야 한다.
     * ti->error에 메시지를 담으면 dmsetup이 실패 시 그 문자열을 출력해준다. */
    if (argc != 1) {
        ti->error = "expected one argument: underlying device";
        return -EINVAL; /* 잘못된 인자 → 즉시 실패 */
    }

    /* 컨텍스트 구조체를 커널 힙에 할당한다.
     * kzalloc은 0으로 초기화까지 해준다
     * GFP_KERNEL: 슬립 가능한 일반 메모리 할당 플래그. 인터럽트 컨텍스트가 아니면 이걸 쓴다. */
    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        ti->error = "out of memory";
        return -ENOMEM; /* 메모리 부족 → 실패 */
    }

    /* argv[0]("/dev/nullb0")을 열어서 c->dev에 핸들을 저장한다.
     * dm_table_get_mode: 이 타깃이 읽기/쓰기 중 어떤 모드로 열려있는지 테이블에서 가져온다. */
    ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);

    if (ret) {
        ti->error = "failed to open underlying device";
        kfree(c); /* dm_get_device 실패 → kzalloc으로 할당한 c를 해제하고 반환 */
        return ret;
    }

    /* 이후 map/dtr 등 다른 콜백에서 ti->private을 캐스팅해 꺼내 쓴다. */
    ti->private = c;
    /* flush = 캐시 비우기 명령
     * 1로 설정하면 위쪽에서 내려오는 flush BIO를 underlying까지 통과시킨다. */
    ti->num_flush_bios = 1;
    /* discard = 블록을 더 이상 안 쓴다고 알리는 힌트 명령 (TRIM과 유사).
     * 1로 설정하면 discard BIO도 통과시킨다. */
    ti->num_discard_bios = 1;

    /* dmesg에 로그를 남긴다. DMINFO는 DM_MSG_PREFIX("myzns")를 앞에 붙여준다. */
    DMINFO("ctr: target attached on top of '%s'", argv[0]);
    return 0;
}

/* dtr — dmsetup remove 시 호출된다.
 *
 * - ctr에서 연 것을 정확히 역순으로 닫는다.
 */
static void myzns_dtr(struct dm_target *ti)
{
    /* ti->private은 void*라 명시적 캐스팅이 필요하다.
     * ctr에서 달아둔 myzns_c 구조체 포인터를 꺼낸다. */
    struct myzns_c *c = (struct myzns_c *) ti->private;

    /* ctr에서 dm_get_device로 열었던 핸들을 반납한다. 짝이 맞아야 한다. */
    dm_put_device(ti, c->dev);
    /* ctr에서 kzalloc으로 할당한 구조체를 해제한다. */
    kfree(c);
    DMINFO("dtr: target detached");
}

/* map — BIO가 들어올 때마다 호출된다.
 *
 * M0 목표: pass-through.
 * - BIO의 목적지를 underlying 디바이스로 바꾼다.
 * - 섹터 번호(LBA)는 그대로 둔다.
 * - 프레임워크가 이어서 처리하도록 적절한 값을 반환한다.
 */
static int myzns_map(struct dm_target *ti, struct bio *bio)
{
    /* dm-linear의 linear_map_bio()가 하는 일을 참고한 흔적.
     * linear는 오프셋 보정이 필요해서 별도 함수로 뺐지만,
     * M0는 1:1 매핑이라 그 로직이 필요 없어서 직접 인라인으로 처리한다. */
    // linear_map_bio(ti, bio);

    /* ti->private에 ctr이 달아둔 컨텍스트 구조체를 꺼낸다.
     * map은 void*를 자동으로 캐스팅해주므로 명시적 캐스팅 없이도 된다. */
    struct myzns_c *c = ti->private;

    /* BIO의 목적지 디바이스를 /dev/nullb0으로 바꾼다.
     * bio->bi_iter.bi_sector(섹터 번호)는 건드리지 않는다 — 1:1 매핑이므로.
     *
     * ❌ 오해: 여기는 sequential write 전환 부분이 아니다.
     * 이 줄은 단순히 "이 BIO를 /dev/nullb0으로 보내라"고 목적지만 바꾸는 것이다.
     * sequential write 전환은 M1에서 bi_sector를 활성 zone의 wp로 바꾸는 것인데,
     * 그 로직은 이 자리(bio_set_dev 다음)에 들어오게 된다. */
    bio_set_dev(bio, c->dev->bdev);

    /* "목적지를 바꿨으니 네가 submit해라"고 프레임워크에 알린다.
     * 프레임워크는 이 반환값을 보고 bio를 /dev/nullb0으로 실제 실행한다. */
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
    /* ti->private에서 컨텍스트를 꺼낸다. */
    struct myzns_c *c = ti->private;

    /* zone 정보를 직접 만들지 않고 underlying 디바이스(/dev/nullb0)에 위임한다.
     * - c->dev->bdev : zone 정보를 읽어올 실제 디바이스
     * - ti->begin    : 이 타깃의 시작 섹터 (1:1 매핑이므로 그대로 전달)
     * - args->next_sector : 다음으로 보고할 zone의 시작 섹터
     * - args, nr_zones    : 프레임워크가 넘겨준 컨텍스트, 최대 zone 개수 */
    return dm_report_zones(c->dev->bdev, ti->begin,
                    args->next_sector, args, nr_zones);
}

/* iterate_devices — DM 프레임워크가 하위 디바이스 정보를 물을 때 호출된다.
 *
 * - 이 콜백이 없으면 /sys/block/dm-X/queue/zoned, chunk_sectors 등이
 *   올바르게 노출되지 않아 blkzone report가 "unable to determine zone size"로 실패한다.
 */
static int myzns_iterate_devices(struct dm_target *ti,
                                  iterate_devices_callout_fn fn, void *data)
{
    struct myzns_c *c = ti->private;

    /* fn은 DM 프레임워크가 넘겨주는 콜백이다.
     * "이 타깃 아래에 이 디바이스가 있고, 0번 섹터부터 ti->len 섹터까지 쓴다"고 알린다.
     * 이 호출 덕분에 /dev/nullb0의 chunk_sectors, zoned 속성이
     * /dev/mapper/mymapper의 sysfs에도 그대로 노출된다. */
    return fn(ti, c->dev, 0, ti->len, data);
}

/* TODO: target_type 구조체를 채운다.
 *
 * - .name: dmsetup create 명령에서 사용할 타입 문자열
 * - .features: zoned HM 디바이스 위에 올라가려면 어떤 플래그가 필요한가?
 *   (이 플래그가 없으면 dmsetup create 시 커널이 연결을 거부한다)
 * - 위에서 작성한 콜백들을 연결한다.
 */
static struct target_type myzns_target = {
    .name            = "myzns",       /* dmsetup create 에서 타입명으로 사용 */
    .version         = {0, 1, 0},     /* major.minor.patch — dmsetup info 에서 확인 가능 */
    .features        = DM_TARGET_ZONED_HM, /* "나는 host-managed zoned 디바이스 위에 올라간다"
                                            * 이 플래그가 없으면 dmsetup create 시
                                            * 커널이 zoned 디바이스와의 연결을 거부한다. */
    .module          = THIS_MODULE,   /* 이 target_type이 속한 모듈.
                                       * rmmod 시 참조 카운트 관리에 사용된다. */
    .ctr             = myzns_ctr,
    .dtr             = myzns_dtr,
    .map             = myzns_map,
    .report_zones    = myzns_report_zones,
    .iterate_devices = myzns_iterate_devices,
};

/* TODO: init / exit 함수를 작성하고 module_init / module_exit으로 등록한다.
 *
 * init: dm_register_target()으로 myzns_target을 DM 프레임워크에 등록
 * exit: dm_unregister_target()으로 등록 해제
 */

/* ❌ 버그: static 누락. 커널 모듈 함수는 외부에 노출할 필요가 없으므로 static이어야 한다. */
static int __init myzns_init(void)
{
    /* myzns_target을 DM 프레임워크의 타입 리스트에 등록한다.
     * 이 시점부터 dmsetup create myzns ... 명령이 동작한다. */
    int r = dm_register_target(&myzns_target);

    if (r < 0)
        DMERR("register failed %d", r);
    else
        DMINFO("target registered");
    return r;
}

/* ❌ 버그: static __exit 누락.
 * __exit 어노테이션은 모듈 언로드 시에만 이 함수가 필요함을 커널에 알린다.
 * 내장 모듈(built-in)로 컴파일할 때 링커가 이 함수를 제거할 수 있게 해준다. */
static void __exit myzns_exit(void)
{
    /* 등록했던 myzns_target을 리스트에서 제거한다.
     * 이 시점부터 dmsetup create myzns ... 는 "unknown target type" 오류가 난다. */
    dm_unregister_target(&myzns_target);
    DMINFO("target unregistered");
}

module_init(myzns_init);
module_exit(myzns_exit);

MODULE_DESCRIPTION("My ZNS pass-through DM target");
MODULE_AUTHOR("KOVALT03");
MODULE_LICENSE("GPL");