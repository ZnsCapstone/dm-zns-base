#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "zns-base"

// zns-base DM target 하나가 사용하는 상태 묶음
struct zns_base_c {
    struct dm_dev *dev;
}


static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char ** argv){
    struct zns_base_c *c; // 내 실제 ssd
    int ret;

    if(argc != 1){
        ti -> error = "expected one argument: underlying device";
        return -EINVAL;
    }

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if(!c){
        ti -> error = "out of memory";
        return -ENOMEM;
    }

    // 이 코드는 DM target이 아래쪽 underlying device를 열어서 자기 상태에 저장하는 부분
    // 이 호출이 성공하면 c->dev -> /dev/null0에 대한 dm_dev 핸들이다.
    ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c -> dev);
    if(ret){
        ti->error = "fail";
        kfree(c);
        return ret;
    }

    ti -> private = c;
    // 캐시된 데이터를 실제 저장장치에 확실히 반영하라. 
    // 즉 이 target은 flush bio를 하위 디바이스 하나로 전달한다
    ti -> num_discard_bios = 1;
    // 이 sector 범위는 이제 필요 없음
    // 이 target은 discard bio를 하위 디바이스 하나로 전달한다
    ti -> num_flush_bios = 1; 

    DMINFO("ctr: target attached on top of '%s'", argv[0]);
	return 0;
}

// dtr은 destructor의 줄임말입니다. 생성자인 ctr의 반대 역할입니다.
// dmsetup remove znsdev 이 명령어를 치면 호출된다. 따라서 /dev/mapper/znsdev를 제거할 때 Device Mapper가 호출합니다.
static void zns_base_dtr(struct dm_target *ti){
    struct zns_base_c *c = ti -> private;

    // ctr에서 열었던 하위 디바이스를 닫는 코드입니다.
    // 의미는 이 target은 이제 underlying device를 더 이상 사용하지 않는다
    dm_put_device(ti, c -> dev);
    kfree(c);
    DMINFO("dtr: target detached");
}

// 이 함수는 실제 I/O 요청이 들어올 때마다 호출되는 핵심 함수
// 사용자가 /dev/mapper/znsdev에 read/write를 하면 그 요청이 bio 형태로 내려오고, Device Mapper가 이 zns_base_map()을 호출합니다.
static int zns_base_map(struct dm_target *ti, struct bio *bio){
    struct zns_base_c *c = ti -> private;

    // 들어온 bio의 목적지 디바이스를 바꿉니다.
    // 변경 전 목적지 : bio 목적지 = /dev/mapper/znsdev
    // 변경 후 목적지 : bio 목적지 = /dev/nvme0n1
    // 즉 DM 가상 디바이스로 들어온 요청을 실제 underlying ZNS SSD로 넘깁니다.
    // 하지만 중요한 점은, 이 코드는 sector 번호는 바꾸지 않습니다.
    //  하지만 ZNS SSD는 일반 SSD처럼 아무 sector에나 랜덤 write를 하면 안 됩니다. 각 zone 안에서는 write pointer 위치에 맞춰 순차적으로 써야 합니다.
    // 그래서 나중에 이 함수 안에 이런 로직을 넣어야 합니다.
    /* write bio인지 확인
        ↓
    상위 logical sector 확인
        ↓
    현재 쓸 수 있는 zone write pointer 위치 확인
        ↓
    bio->bi_iter.bi_sector를 sequential physical sector로 변경
        ↓
    logical sector -> physical sector mapping table 갱신
        ↓
    bio_set_dev(bio, c->dev->bdev)*/

    bio_set_dev(bio, c -> dev -> bdev);
    return DM_MAPIO_REMAPPED;
}

// 이 함수는 상위 DM 디바이스의 zone 정보를 물어볼 때 호출되는 함수
// 예를 들어서 사용자가 blkzone report /dev/mapper/znsdev 이런 명령어를 치면 커널은 /dev/mapper/znsdev의 zone 정보를 알아야 한다.
// blkzone report /dev/mapper/znsdev는 zoned block device의 zone 정보를 출력하는 명령어입니다.
static int zns_base_report_zones(struct dm_target *ti, struct dm_report_zones_args *args, unsigned int nr_zones){
    struct zns_base_c *c = ti -> private;

    return dm_report_zones(c -> dev -> bdev, ti -> begin, args -> next_sector, args, nr_zones);
}

// 이 함수는 Device Mapper에게 “이 target 아래에 어떤 실제 디바이스가 있는지” 알려주는 함수입니다.
static int zns_base_iterate_devices(struct dm_target *ti, iterate_devices_callout_fn fn, void *data){
    struct zns_base_c *c = ti -> private;

    // fn은 Device Mapper가 넘겨준 callback 함수입니다.
    // 즉 DM 프레임워크가 하위 디바이스 정보를 알려달라고 요청하면서 넘겨준 함수
    return fn(ti, c -> dev, 0, ti -> len, data);
}

// 이 구조체는 Device Mapper에게 zns-base라는 target 타입을 등록하기 위한 설명서입니다.
// 여기서 struct target_type은 네가 새로 정의한 게 아닙니다. 이미 커널 Device Mapper 헤더에 정의되어 있는 구조체 타입입니다.
// 즉 이 코드는 커널이 제공하는 struct target_type 타입의 변수 zns_base_target을 하나 만들고, 그 안의 필드들을 초기화하겠다
static struct target_type zns_base_target = {
    .name           = "zns-base",
    .version        = {0, 1, 0},
    // 이 target이 host-managed zoned device 위에서 동작할 수 있다는 표시입니다.
    // 이 플래그가 없으면 Device Mapper가 이 target은 host-managed zoned device를 다룰 수 있다고 선언하지 않았네 라고 판단해서 생성이 실패할 수 있습니다.
    .features       = DM_TARGET_ZONED_HM, 
    // 이 target이 현재 커널 모듈에 속해 있다는 뜻입니다.
    // 커널이 모듈 참조 카운트를 관리할 때 씁니다. 예를 들어 /dev/mapper/znsdev가 아직 사용 중인데 모듈을 rmmod로 빼면 안 됩니다. THIS_MODULE 덕분에 그런 참조 관리가 됩니다.
    .module         = THIS_MODULE,
    .ctr            = zns_base_ctr,
    .dtr            = zns_base_dtr,
    .map            = zns_base_map,
    .report_zones   = zns_base_report_zones,
    .iterate_devices= zns_base_iterate_devices,
};

// 이 함수는 커널 모듈이 로드될 때 실행되는 초기화 함수입니다.
static int __init zns_base_init(void){
    // dm_register_target()은 Device Mapper 프레임워크에 새로운 target 타입을 등록하는 함수입니다.
    // 즉 이 줄의 의미는 Device Mapper야, 이제부터 "zns-base"라는 target 타입을 사용할 수 있게 등록해줘.
    int ret = dm_register_target(&zns_base_target);

    // 커널에서는 보통 성공하면 0, 실패하면 음수 errno를 반환합니다.
    if(ret < 0)
        DMERR("target registration failed: %d", ret);
    else
        DMINFO("target registerd");
    return ret;
}

// 이 부분은 모듈 종료 함수 등록
static void __exit zns_base_exit(void){
    dm_unregister_target(&zns_base_target);
    DMINFO("target unregisterd");
}

// 이건 커널에게 이 모듈이 로드될 때 zns_base_init()을 실행해라 라고 알려주는 매크로입니다.
module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS practice");
MODULE_AUTHOR("KEPER1212")
MODULE_LICENSE("GPL");