# Device Mapper Admin Guide (공식 커널 문서)

> 출처: https://docs.kernel.org/admin-guide/device-mapper/index.html

## 개요

Device Mapper(DM)는 리눅스 커널의 블록 계층에서 **물리적 블록 디바이스를 가상 블록 디바이스로 매핑**하는 프레임워크다. LVM2, dm-crypt, RAID 등 리눅스의 핵심 스토리지 기능 대부분이 DM 위에 구현되어 있다.

## 아키텍처

```
사용자 공간 (dmsetup, lvm2 등)
        ↓ ioctl (device-mapper ioctl API)
커널: Device Mapper 프레임워크
        ↓
[타깃 플러그인: linear, crypt, zoned, ...]
        ↓
물리 블록 디바이스 (SSD, HDD, ...)
```

## 핵심 개념

### 매핑 테이블 (Mapping Table)

DM의 기본 단위. 논리 섹터 범위를 어떤 타깃으로 보낼지 정의한다.

```
시작섹터 길이 타깃타입 타깃인자...
0        N    linear   /dev/sda 0
```

`dmsetup create` 명령에 이 테이블을 넘기면 `/dev/mapper/이름` 가상 디바이스가 생성된다.

### 타깃 (Target)

실제 I/O 처리 로직. 구조체(`target_type`)로 등록되며, 상황별 콜백 함수를 구현한다.

| 콜백 | 호출 시점 |
|---|---|
| `.ctr` | `dmsetup create` |
| `.dtr` | `dmsetup remove` |
| `.map` | BIO가 들어올 때마다 |
| `.report_zones` | zone 정보 쿼리 시 |
| `.iterate_devices` | 큐 속성 전파 시 |

## 주요 내장 타깃 목록

| 타깃 | 용도 |
|---|---|
| `linear` | 여러 장치를 하나로 이어 붙이기 (LVM PV) |
| `striped` | RAID-0 스타일 스트라이핑 |
| `mirror` | RAID-1 미러링 |
| `snapshot` | 시점 기반 스냅샷 |
| `crypt` | 투명 암호화 (LUKS 기반) |
| `verity` | 읽기 무결성 검증 (Android verified boot 등) |
| `integrity` | 읽기/쓰기 무결성 태그 |
| `thin` | thin provisioning |
| `cache` | 빠른 장치를 느린 장치의 캐시로 |
| `zoned` | SMR/ZNS HDD/SSD passthrough + 변환 |
| `delay` | I/O에 의도적 지연 주입 (테스트용) |
| `zero` | 읽으면 항상 0, 쓰면 버림 |
| `error` | 모든 I/O를 오류로 |

## dmsetup 주요 명령어

```bash
# 타깃 생성
echo "0 N 타깃타입 인자..." | dmsetup create 이름

# 목록 조회
dmsetup ls
dmsetup status

# 제거
dmsetup remove 이름

# 테이블 확인
dmsetup table 이름

# 메시지 전송 (타깃이 message 콜백 구현 시)
dmsetup message /dev/dm-0 0 명령어
```

## 이 프로젝트와의 연관

`dm-zns-base`는 위 타깃 목록에 추가되는 새로운 타깃이다. `dm_register_target()`으로 "zns-base"를 등록하고, M0에서는 passthrough, M1 이후에는 임의→순차 변환 로직을 `.map` 콜백에 구현한다.
