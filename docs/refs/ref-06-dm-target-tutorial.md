# DM 타깃 작성 튜토리얼

> 출처: https://gauravmmh1.medium.com/writing-your-own-device-mapper-target-539689d19a89

## 개요

Device Mapper 타깃을 처음부터 작성하는 방법을 다루는 튜토리얼. "storage stack에 필요한 기능을 커널 모듈로 추가하는 일반적인 방법"을 보여준다.

## DM이 해결하는 문제

블록 디바이스 위에 추가 기능(암호화, 미러링, 스트라이핑 등)을 넣으려면 커널 스토리지 스택을 수정하거나 새 레이어를 삽입해야 한다. DM은 이를 위한 **플러그인 프레임워크**다. 기존 커널 코드를 건드리지 않고 새 타입을 등록해서 끼워 넣는다.

## 최소 타깃 구현 구조

```c
// 1. 컨텍스트 구조체
struct my_target_c {
    struct dm_dev *dev;
    // 타깃별 상태 추가
};

// 2. 콜백 구현
static int my_ctr(struct dm_target *ti, unsigned int argc, char **argv) { ... }
static void my_dtr(struct dm_target *ti) { ... }
static int my_map(struct dm_target *ti, struct bio *bio) { ... }

// 3. target_type 등록
static struct target_type my_target = {
    .name    = "my-target",
    .version = {1, 0, 0},
    .module  = THIS_MODULE,
    .ctr     = my_ctr,
    .dtr     = my_dtr,
    .map     = my_map,
};

// 4. 모듈 init/exit
static int __init my_init(void) { return dm_register_target(&my_target); }
static void __exit my_exit(void) { dm_unregister_target(&my_target); }
module_init(my_init);
module_exit(my_exit);
```

## 핵심 개념: DM 매핑 테이블

dmsetup에 넘기는 테이블 문자열의 형식:

```
<시작섹터> <길이> <타깃타입> <타깃인자...>
```

예시:
```bash
echo "0 4096 my-target /dev/sdb" | dmsetup create mydev
```

타입 이름이 `target_type.name`과 매칭되면 해당 `.ctr`이 호출된다.

## `.map` 반환값 의미

| 반환값 | 의미 |
|---|---|
| `DM_MAPIO_REMAPPED` | BIO의 디바이스/섹터를 변경했으니 DM이 실행 |
| `DM_MAPIO_SUBMITTED` | 타깃이 직접 처리 완료, DM은 손대지 말 것 |
| `DM_MAPIO_KILL` | 이 BIO를 오류로 처리 |
| `DM_MAPIO_REQUEUE` | BIO를 다시 큐에 넣기 |

## Passthrough 타깃의 `.map` 예시

```c
static int my_map(struct dm_target *ti, struct bio *bio)
{
    struct my_target_c *c = ti->private;
    bio_set_dev(bio, c->dev->bdev);         // 목적지 디바이스 변경
    bio->bi_iter.bi_sector += ti->begin;    // 오프셋 보정 (필요 시)
    return DM_MAPIO_REMAPPED;
}
```

`bio_set_dev`로 목적지를 바꾸는 것이 핵심. 섹터 번호를 바꾸려면 `bio->bi_iter.bi_sector`를 수정한다.

## 빌드 및 테스트 흐름

```bash
# 빌드
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# 적재
insmod my_target.ko

# 디바이스 생성
echo "0 $(blockdev --getsz /dev/sdb) my-target /dev/sdb" | dmsetup create mydev

# 확인
ls /dev/mapper/mydev
dmsetup ls

# 제거
dmsetup remove mydev
rmmod my_target
```

## 이 프로젝트에서의 활용

`dm-zns-base.c`는 이 튜토리얼의 passthrough 패턴을 그대로 따른다. M0는 `bio_set_dev`만 하고 섹터는 그대로 통과. M1에서는 `.map` 안에서 섹터 번호를 변환하고 매핑 테이블을 갱신하는 로직이 추가된다.
