# dm-zoned (공식 커널 문서)

> 출처: https://docs.kernel.org/admin-guide/device-mapper/dm-zoned.html

## 개요

`dm-zoned`는 **SMR HDD / ZBC·ZAC 호환 zoned block device를 일반 블록 디바이스처럼 보이게** 하는 DM 타깃이다. Linux 4.13부터 메인라인에 포함. 순차 쓰기 제약을 커널 레이어에서 흡수해 ext4 같은 zone-unaware 파일시스템이 그대로 동작한다.

## 이 프로젝트와의 차이 (Prior Art 비교)

| 항목 | dm-zoned | 본 과제 (dm-zns-base) |
|---|---|---|
| 대상 하드웨어 | SMR HDD (conventional zone 있음) | ZNS SSD (sequential-only) |
| conventional zone 의존 | 필수 (메타데이터용) | 없음 — sequential zone만으로 구현 |
| 매핑 자료구조 | 비트맵 기반 | LSM-Tree 기반 |
| 메인라인 여부 | ✓ | 졸업프로젝트 산출물 |

## 동작 원리

### Zone 분류

```
물리 디바이스
├── Conventional zone(s)  → 메타데이터 저장 (superblock, 매핑 테이블, 비트맵)
└── Sequential zone(s)    → 사용자 데이터
```

ZNS SSD에는 conventional zone이 없어서 dm-zoned를 그대로 쓸 수 없다. 별도 일반 SSD나 conventional namespace를 동반해야 한다.

### 메타데이터 구조

- **Superblock**: 첫 번째 conventional zone의 첫 블록
- **매핑 테이블**: chunk 단위로 어느 sequential zone에 있는지
- **비트맵**: 각 블록이 유효한지 추적

메타데이터를 두 세트(Primary + Secondary)로 유지하고 세대 카운터로 최신 세트를 판별 → crash 후 복구 가능.

### 쓰기 처리

```
임의 위치 쓰기 요청
        ↓
정렬된 순차 쓰기?  → YES → sequential zone에 직접 기록
        ↓ NO
buffer zone 할당 → buffer zone에 기록 → 나중에 reclaim(GC)
```

### 읽기 처리

유효성 비트맵 참조 → sequential zone 또는 buffer zone 중 최신 데이터 위치로 리디렉션.

### Reclaim (GC)

- 기본: free random zone < 50% 시 자동 시작
- 수동: `dmsetup message /dev/dm-X 0 reclaim`
- 과정: valid 데이터를 다른 zone으로 복사 → 원 zone reset → 재사용

## 성능 특성

- 메모리 사용량: 10 TB 디스크 기준 4.5 MB 이하 (비트맵 기반이라 경량)
- 논리 섹터 크기: 4096 바이트
- GC로 인한 write amplification 존재

## 사용법

```bash
# 포맷 (dmzadm 유틸리티 사용)
dmzadm --format /dev/sdX

# DM 타깃 시작
dmzadm --start /dev/sdX [/dev/sdY]   # Y는 별도 conventional 디바이스 (선택)

# 상태 확인
dmsetup status /dev/dm-X
# → "N zones N meta zones N DM zones N rand zones N seq zones N empty zones N ..."
```

## 이 프로젝트 구현 시 참고 포인트

- dm-zoned의 소스(`drivers/md/dm-zoned-*.c`)는 DM 타깃 구현의 실제 예시. `.map`에서 BIO를 어떻게 분류하고 리디렉션하는지 구조 참고 가능.
- 본 과제는 비트맵 대신 LSM-Tree로 매핑을 유지하고, conventional zone 없이 sequential zone만 쓴다는 점이 핵심 차별점.
