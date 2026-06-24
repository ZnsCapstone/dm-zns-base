# Linux Zoned Block Device API

> 출처: https://zonedstorage.io/docs/linux/zbd-api

## 개요

리눅스 커널은 zoned block device를 위한 두 가지 사용자 인터페이스를 제공한다.

| 인터페이스 | 적합한 용도 |
|---|---|
| **sysfs 속성 파일** | 셸 스크립트, Python 등 스크립팅 |
| **ioctl() 시스템 호출** | C 프로그램, 저수준 라이브러리 |

## sysfs 인터페이스

디바이스의 zone 속성을 파일로 노출한다. 경로: `/sys/block/<장치명>/queue/`

| 파일 | 도입 커널 | 설명 |
|---|---|---|
| `zoned` | 4.10 | zone 모델: `none` / `host-aware` / `host-managed` |
| `chunk_sectors` | 4.10 | zone 크기 (512B 섹터 단위) |
| `nr_zones` | 4.20 | 전체 zone 개수 |
| `max_open_zones` | 5.9 | 동시에 열 수 있는 최대 zone 수 |
| `max_active_zones` | 5.9 | 동시에 활성화 가능한 최대 zone 수 |

```bash
# 예시
cat /sys/block/nullb0/queue/zoned          # host-managed
cat /sys/block/nullb0/queue/chunk_sectors  # 131072 (= 64 MiB)
cat /sys/block/nullb0/queue/nr_zones       # 32
```

## Zone 데이터 구조 (`struct blk_zone`)

커널 5.9 기준:

```c
struct blk_zone {
    __u64   start;      // zone 시작 섹터
    __u64   len;        // zone 길이 (섹터 단위)
    __u64   wp;         // write pointer 위치
    __u8    type;       // zone 타입
    __u8    cond;       // zone 상태 (condition)
    __u8    non_seq;    // 비순차 쓰기 리소스 활성화 여부
    __u8    reset;      // reset 권고 여부
    __u8    resv[4];
    __u64   capacity;   // 실제 사용 가능한 용량 (len과 다를 수 있음)
    __u8    reserved[24];
};
```

### Zone 타입 (`type` 필드)

| 값 | 의미 |
|---|---|
| `BLK_ZONE_TYPE_CONVENTIONAL` | 쓰기 포인터 없음, 임의 쓰기 가능 |
| `BLK_ZONE_TYPE_SEQWRITE_REQ` | 순차 쓰기 필수 (ZNS SSD, SMR HDD) |
| `BLK_ZONE_TYPE_SEQWRITE_PREF` | 순차 쓰기 권장 (host-aware HDD) |

### Zone 상태 (`cond` 필드)

| 값 | 의미 |
|---|---|
| `BLK_ZONE_COND_EMPTY` | 비어 있음, wp = zone 시작 |
| `BLK_ZONE_COND_IMP_OPEN` | 암시적으로 열림 (쓰기 후 자동) |
| `BLK_ZONE_COND_EXP_OPEN` | 명시적으로 열림 (OPEN 명령) |
| `BLK_ZONE_COND_CLOSED` | 닫힘 (wp 위치 보존) |
| `BLK_ZONE_COND_FULL` | 가득 참, wp = zone 끝 |
| `BLK_ZONE_COND_READONLY` | 읽기 전용 (하드웨어 결함) |
| `BLK_ZONE_COND_OFFLINE` | 오프라인 (심각한 결함) |

## ioctl() 명령어

| 명령어 | 도입 커널 | 기능 |
|---|---|---|
| `BLKREPORTZONE` | 4.10 | zone 정보 배열 조회 |
| `BLKRESETZONE` | 4.10 | wp를 zone 시작으로 초기화 |
| `BLKGETZONESZ` | 4.20 | zone 크기 조회 |
| `BLKGETNRZONES` | 4.20 | zone 개수 조회 |
| `BLKOPENZONE` | 5.5 | zone 명시적 열기 |
| `BLKCLOSEZONE` | 5.5 | zone 닫기 |
| `BLKFINISHZONE` | 5.5 | zone FULL 상태로 전환 |

### 코드 예시: zone 정보 조회

```c
// BLKREPORTZONE
struct blk_zone_report *hdr = malloc(sizeof(*hdr) + nr * sizeof(struct blk_zone));
hdr->sector   = start_sector;
hdr->nr_zones = nr;
ioctl(fd, BLKREPORTZONE, hdr);
// hdr->zones[0], [1], ... 에 결과
```

### 코드 예시: zone reset

```c
// BLKRESETZONE
struct blk_zone_range zrange = {
    .sector     = zone_start,
    .nr_sectors = zone_size,
};
ioctl(fd, BLKRESETZONE, &zrange);
```

## `blkzone` 유틸리티 (유저스페이스)

위 ioctl을 감싸는 커맨드라인 도구.

```bash
# zone 정보 출력
blkzone report /dev/nullb0

# zone 0 reset
blkzone reset -o 0 -c 1 /dev/nullb0

# zone 0~3 open
blkzone open -o 0 -c 4 /dev/nullb0
```

`blkzone report` 출력 예시:

```
start: 0x000000000, len 0x020000, cap 0x020000, wptr 0x000000 reset:0 non-seq:0, zcond: 0(emp) [type: 2(SEQ_WRITE_REQUIRED)]
start: 0x000020000, len 0x020000, cap 0x020000, wptr 0x020000 reset:0 non-seq:0, zcond: 0(emp) [type: 2(SEQ_WRITE_REQUIRED)]
```

## M0 테스트와의 연관

`test-basic.sh`의 `[4/4]` 단계가 `blkzone report` 출력에서 `wptr 0x000008`을 찾는다. 이 `wptr` 값이 바로 `struct blk_zone.wp` 필드다.
