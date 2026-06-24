# null_blk 커널 모듈

> 출처: https://docs.kernel.org/block/null_blk.html

## 개요

`null_blk`는 **실제 저장 매체 없이 블록 디바이스를 에뮬레이션하는 커널 모듈**이다. 원래 블록 레이어 벤치마킹을 위해 만들어졌고, RAM을 백킹 스토어로 써서 실제 I/O 지연 없이 블록 디바이스 의미론(zone, write pointer 등)을 완전히 재현한다.

이 프로젝트에서는 **ZNS SSD 없이 ZNS 동작을 검증**하기 위해 사용한다.

## 두 가지 생성 방법

### 방법 1 — 모듈 파라미터 (레거시)

```bash
modprobe null_blk nr_devices=1 zoned=1 zone_size=64 gb=2
# → /dev/nullb0 자동 생성
```

단점: 모듈 로드 시점에만 설정 가능, 세밀한 제어 어려움.

### 방법 2 — configfs (권장, 이 프로젝트 사용)

```bash
modprobe null_blk nr_devices=0      # 자동 생성 비활성화
mkdir /sys/kernel/config/nullb/nullb0
echo 1    > /sys/kernel/config/nullb/nullb0/zoned
echo 64   > /sys/kernel/config/nullb/nullb0/zone_size     # MiB
echo 2048 > /sys/kernel/config/nullb/nullb0/size          # MiB
echo 1    > /sys/kernel/config/nullb/nullb0/memory_backed
echo 1    > /sys/kernel/config/nullb/nullb0/power         # 이 시점에 /dev/nullb0 생성
```

장점: 런타임에 파라미터 설정, 여러 인스턴스를 다른 설정으로 동시에 운용 가능.

## 주요 파라미터

### 공통

| 파라미터 | 기본값 | 설명 |
|---|---|---|
| `queue_mode` | 2 | 0=Bio-based, 1=Single-queue, 2=Multi-queue |
| `gb` / `size` | 250 GB / configfs는 MiB | 디바이스 총 크기 |
| `bs` | 512 B | 블록 크기 |
| `nr_devices` | 1 | 생성할 디바이스 수 (0 = configfs 모드) |
| `memory_backed` | 0 | 1이면 RAM을 실제 저장소로 사용 (데이터 보존) |
| `mbps` | 0 | 대역폭 제한 (0 = 무제한) |

### Zoned 모드 전용

| 파라미터 | 기본값 | 설명 |
|---|---|---|
| `zoned` | 0 | 1이면 host-managed zoned 모드 |
| `zone_size` | 256 MiB | 각 zone 크기 (MiB) |
| `zone_nr_conv` | 0 | conventional zone 개수 |
| `zone_max_open` | 0 | 최대 open zone 수 (0 = 제한 없음) |
| `zone_max_active` | 0 | 최대 active zone 수 (0 = 제한 없음) |

### Multi-queue 전용

| 파라미터 | 기본값 | 설명 |
|---|---|---|
| `submit_queues` | 1 | 제출 큐 수 |
| `hw_queue_depth` | 64 | 큐 깊이 |
| `irqmode` | 1 | 완료 인터럽트 모드 (0=None, 1=SoftIRQ, 2=Timer) |
| `no_sched` | 0 | I/O 스케줄러 비활성화 |

## 이 프로젝트의 기본 설정

```
크기: 2 GiB (2048 MiB)
zone 크기: 64 MiB
zone 수: 32
타입: 전부 SEQWRITE_REQ (conventional zone 없음)
백킹: memory_backed=1
```

**64 MiB × 32 zones로 설정한 이유:**
- 64 MiB는 상용 ZNS SSD zone 크기(수십~수백 MiB)와 같은 자릿수
- 32 zones면 GC 시나리오(zone 가득 참 → reset → 재사용)를 충분히 만들 수 있음
- 2 GiB 총 용량은 ext4 mkfs + 데이터 쓰기에 충분하면서 VM 메모리 부담 적음

## null_blk의 한계

| 항목 | null_blk | 실제 ZNS SSD / FEMU |
|---|---|---|
| zone 의미론 (wp, reset, SEQ_WRITE_REQUIRED) | 동일 | 동일 |
| NVMe ZNS 명령 (Zone Append 등) | 없음 | 있음 |
| I/O 지연 | ~0 (메모리 직결) | 수 µs ~ ms |
| fio 성능 수치의 의미 | 없음 | 있음 |

**null_blk = 코드 정확성 검증용, FEMU/실 SSD = 성능 평가용.**

## 상태 확인 및 정리

```bash
# 상태 확인
cat /sys/block/nullb0/queue/zoned          # host-managed
cat /sys/block/nullb0/queue/chunk_sectors  # 131072
cat /sys/block/nullb0/queue/nr_zones       # 32
blkzone report /dev/nullb0 | head -3

# 제거 (configfs 방식)
echo 0 > /sys/kernel/config/nullb/nullb0/power
rmdir /sys/kernel/config/nullb/nullb0
rmmod null_blk
```
