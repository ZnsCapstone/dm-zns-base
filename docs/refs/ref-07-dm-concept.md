# Linux Device Mapper 개념 정리

> 출처: https://linuxvox.com/blog/linux-device-mapper/

## 개요

Device Mapper는 **물리적 블록 디바이스를 논리적 디바이스로 매핑하는 커널 프레임워크**다. 하드웨어 복잡성을 추상화하고 그 위에 LVM, RAID, 스냅샷, 암호화 같은 기능을 구현할 수 있게 한다.

## 계층 구조

```
애플리케이션 / 파일시스템
        ↓
논리적 블록 디바이스 (/dev/mapper/X)
        ↓
Device Mapper 프레임워크
    ├── Targets (기능 구현체)
    ├── Maps   (섹터 범위 → 타깃 매핑 테이블)
    └── Metadata (매핑 정보 저장)
        ↓
물리적 블록 디바이스 (SSD, HDD, ...)
```

## 핵심 구성 요소

### Targets

I/O를 실제로 어떻게 처리할지 정의하는 플러그인.

| 타깃 | 동작 |
|---|---|
| **Linear** | 논리 섹터 → 물리 섹터 1:1 매핑 (LVM PV 기본) |
| **Striped** | 여러 디바이스에 데이터를 분산 (RAID-0) |
| **Mirror** | 두 디바이스에 동일 데이터 기록 (RAID-1) |
| **Snapshot** | 특정 시점의 복사본 유지 |
| **Crypt** | AES 등으로 투명 암호화/복호화 |

### Maps (매핑 테이블)

논리 섹터 범위와 타깃을 연결하는 테이블. `dmsetup table` 명령으로 확인 가능.

```
0       2097152  linear  /dev/sda  0
2097152 4194304  linear  /dev/sdb  0
```

위 예시: 논리 섹터 0~2097151은 `/dev/sda`로, 2097152~6291455는 `/dev/sdb`로.

### Metadata

매핑 정보, 타입별 메타데이터를 저장하는 영역. 타깃마다 자체 메타데이터 형식을 정의한다.

## 주요 사용 사례

### LVM (Logical Volume Management)

```
물리 디스크 여럿 → PV(Physical Volume) → VG(Volume Group) → LV(Logical Volume)
```

물리 디스크 교체·확장 없이 논리 볼륨 크기를 변경할 수 있다. 내부적으로 DM linear + DM thin으로 구현.

### 스냅샷

```bash
# LVM 스냅샷 생성 (내부적으로 dm-snapshot 사용)
lvcreate -L 1G -s -n snap /dev/vg/data
```

Copy-on-Write 방식: 원본 블록이 처음 수정될 때만 스냅샷 영역에 원본을 복사.

### 암호화 (LUKS)

```bash
# dm-crypt로 암호화된 디바이스 열기
cryptsetup open /dev/sdb myenc
# → /dev/mapper/myenc 생성, 읽기/쓰기 시 투명하게 암호화/복호화
```

## userspace ↔ 커널 통신

dmsetup 등의 유저스페이스 도구는 `/dev/mapper/control` 디바이스에 ioctl을 보내 DM 프레임워크와 통신한다.

```
dmsetup create  → DM_DEV_CREATE ioctl
dmsetup remove  → DM_DEV_REMOVE ioctl
dmsetup table   → DM_TABLE_STATUS ioctl
```

## 이 프로젝트에서의 위치

`dm-zns-base`는 DM 프레임워크의 새로운 타깃 플러그인이다. 위 타깃들처럼 `.map` 콜백 하나로 I/O 경로를 제어한다. M1 이후에는 "임의 LBA 쓰기 → sequential zone append"라는 독자적인 변환 로직이 이 자리에 들어간다.
