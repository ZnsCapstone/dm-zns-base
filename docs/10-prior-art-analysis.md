# 10. Prior Art 분석 — dm-zoned vs dm-zap, 그리고 마일스톤의 목적

## 목차

1. [전제: 왜 이 문제가 어려운가](#1-전제-왜-이-문제가-어려운가)
2. [dm-zoned 상세 분석](#2-dm-zoned-상세-분석)
3. [dm-zap 상세 분석](#3-dm-zap-상세-분석)
4. [dm-zoned vs dm-zap 비교](#4-dm-zoned-vs-dm-zap-비교)
5. [본 과제의 포지셔닝](#5-본-과제의-포지셔닝)
6. [마일스톤별 목적 분석](#6-마일스톤별-목적-분석)

---

## 1. 전제: 왜 이 문제가 어려운가

### ZNS SSD의 제약

ZNS(Zoned Namespace) SSD는 디스크를 zone 단위로 나누고, 각 zone에 **write pointer(wp)** 를 둔다. 쓰기는 반드시 wp 위치에서만 가능하다. wp 이전이나 임의 위치에 쓰면 드라이버가 즉시 거절한다(`blk_update_request: I/O error`).

```
Zone 0: [wp=0x00000]──────────────────────── (EMPTY)
         ↑ 여기서만 쓸 수 있음

Zone 0: [data][data][wp=0x00010]────────── (OPEN)
                      ↑ 여기서만 쓸 수 있음

임의 위치 쓰기 시도: 0x00008에 write → REJECT
```

ZNS SSD가 이 제약을 두는 이유: 내부 GC를 없애서 tail latency 안정화, WAF 감소, 수명 연장.

### ext4가 요구하는 것

ext4는 임의 위치 덮어쓰기를 전제로 설계된 파일시스템이다.

- inode 업데이트: 고정 위치에 반복 쓰기
- 저널: 고정 영역에 반복 쓰기
- 파일 내용 수정: 기존 블록 위치에 덮어쓰기

즉, ext4와 ZNS SSD는 **근본적으로 충돌**한다.

### 해결 방향 두 가지

```
방향 A: 파일시스템을 zone-aware로 수정
         → F2FS zoned mode, btrfs zoned mode
         → 파일시스템 자체를 바꿔야 함

방향 B: 블록 계층에서 임의 쓰기를 순차 쓰기로 변환
         → ext4는 수정 없이 그대로 사용
         → dm-zoned, dm-zap, 본 과제가 여기에 해당
```

본 과제는 방향 B. ext4를 그대로 두고 커널 블록 레이어에서 문제를 흡수한다.

---

## 2. dm-zoned 상세 분석

### 배경과 대상 하드웨어

Linux 4.13에 메인라인 포함. 원래 **SMR(Shingled Magnetic Recording) HDD**를 위해 설계됐다. SMR HDD는 ZNS SSD보다 먼저 나왔고, 중요한 차이가 있다.

```
SMR HDD zone 구성:
├── Conventional zone  (임의 쓰기 가능, 수 개)  ← dm-zoned가 의존하는 부분
└── Sequential zone    (순차 쓰기만, 대다수)

ZNS SSD zone 구성:
└── Sequential zone    (순차 쓰기만, 전부)      ← conventional 없음
```

dm-zoned는 conventional zone의 존재를 전제로 설계됐다. 이것이 ZNS SSD에서의 결정적 한계다.

### 동작 구조 상세

#### Zone 역할 분배

```
물리 디바이스 (SMR HDD)
├── Conventional zone 0    ← superblock + 매핑 테이블 (Primary)
├── Conventional zone 1    ← superblock + 매핑 테이블 (Secondary, 백업)
├── Sequential zone 0      ← 사용자 데이터
├── Sequential zone 1      ← 사용자 데이터
├── Sequential zone 2      ← buffer zone (임시)
└── ...

/dev/mapper/dm-zoned       ← 위쪽에는 일반 블록 디바이스처럼 보임
```

#### 매핑 자료구조

- **Superblock**: conventional zone 첫 블록. 전체 구성 정보 저장.
- **매핑 테이블**: "논리 chunk X는 physical sequential zone Y의 offset Z에 있다"
- **비트맵**: 각 4 KiB 블록이 valid인지 invalid인지 1비트씩 기록

비트맵 기반이라 메모리 사용량이 매우 낮다 (10 TB 디스크 기준 4.5 MB).

#### 쓰기 흐름

```
임의 LBA 쓰기 요청 수신
        │
        ├── 정렬된 순차 쓰기인가? (wp에 딱 맞게)
        │       YES → sequential zone에 직접 기록
        │
        └── NO (임의 위치)
                → buffer zone 할당
                → buffer zone의 wp에 append
                → 매핑 테이블 갱신 (이 LBA → buffer zone 위치)
                → 기존 매핑이 있었다면 비트맵에서 old 위치를 invalid 처리
```

#### 읽기 흐름

```
LBA X 읽기 요청
    → 매핑 테이블에서 X의 위치 조회
    → sequential zone 또는 buffer zone의 물리 위치로 변환
    → 읽기 수행
```

#### GC (Reclaim)

```
트리거: free buffer zone < 50% (기본값)

1. victim zone 선택 (valid 블록이 적은 buffer zone)
2. victim zone의 valid 블록들을 새 sequential zone에 복사
3. 매핑 테이블 갱신 (복사된 블록들의 위치 업데이트)
4. victim zone reset → wp = 0 → 재사용 가능
```

#### 메타데이터 이중화

Primary와 Secondary 두 세트를 번갈아 쓰고 세대 카운터(generation counter)로 최신 세트를 판별한다. crash 후 재시작 시 세대 카운터가 높은 쪽을 사용해 복구한다.

### dm-zoned의 한계

**한계 1: conventional zone 필수**

ZNS SSD에는 conventional zone이 없다. 따라서 dm-zoned를 ZNS SSD에서 쓰려면:
- 별도 일반 SSD를 메타데이터용으로 동반, 또는
- ZNS namespace 옆에 conventional namespace를 별도 설정

어느 쪽이든 **ZNS SSD 단독으로는 동작 불가**.

**한계 2: 비트맵 기반 매핑의 granularity**

chunk 단위 매핑이라 fine-grained 제어가 어렵다. LSM-Tree처럼 compaction이 GC와 자연스럽게 결합되지 않는다.

---

## 3. dm-zap 상세 분석

### 배경

Western Digital이 개발한 연구 프로토타입. dm-zoned의 conventional zone 의존성을 없애고 **ZNS SSD 단독**으로 동작하는 것이 목표. 현재 메인라인 미포함, 유지보수 중단 상태.

### 핵심 설계 결정: 부분 변환

dm-zap은 전체 디바이스를 conventional로 변환하지 않는다. **일부 zone만** conventional로 변환하고 나머지는 sequential 그대로 노출한다.

```
/dev/nvme0n1 (ZNS SSD, N개 zone)
├── Zone 0~1:   메타데이터 (내부 관리용)
├── Zone 2~R-1: sequential zone → 외부에 conventional로 보임  ← dm-zap이 관리
└── Zone R~N-1: sequential zone → 외부에도 sequential로 보임  ← passthrough

/dev/mapper/dm-zap
├── Conventional zone 0 ~ R-3  ← 임의 쓰기 가능
└── Sequential zone 0 ~ N-R-1  ← 순차 쓰기만
```

즉, **위쪽(사용자)에 여전히 mixed 인터페이스**(conventional + sequential)가 노출된다. ext4 같은 zone-unaware 파일시스템은 여전히 그대로 올릴 수 없다. 대신 zone-aware 파일시스템이나, 파일시스템 없이 블록 디바이스를 직접 사용하는 애플리케이션이 이 "일부 conventional" 영역을 활용하는 시나리오를 위해 설계됐다.

### 동작 구조 상세

#### 디스크 레이아웃

```
Zone 0: 슈퍼블록 + 매핑 데이터 (Primary)
Zone 1: 백업 메타데이터 (Secondary)
Zone 2 ~ R-1: 관리 대상 zone (외부에 conventional로 광고)
Zone R ~ N-1: passthrough zone (외부에 sequential로 그대로 노출)
```

#### 메타데이터 구조

**Zone Header** (512 bytes, 각 관리 zone 시작):
```
MAGIC: {'z','a','p','0'}
VERSION
SEQUENCE_NR   ← 세대 카운터 역할
TYPE          ← 이 zone의 역할
```

**Zone Footer** (512 bytes, 각 관리 zone 끝):
```
매핑 데이터 시작 offset 기록
```

**Mapping Entry** (8 bytes, 논리-물리 주소 쌍):
```
START   : 논리 주소 시작 섹터
LENGTH  : 매핑 길이
ADDRESS : 물리 주소 (zone 번호 + offset)
```

#### 쓰기 흐름

```
conventional zone 영역에 임의 LBA 쓰기 수신
    → 활성 관리 zone의 wp에 append
    → mapping entry 추가 (논리 LBA → 물리 위치)
    → zone이 가득 차면 → 다음 빈 zone을 활성으로 전환
```

#### 읽기 흐름

```
LBA X 읽기 수신
    → mapping entry 검색 (X를 포함하는 엔트리 찾기)
    → 물리 위치 (zone, offset) 계산
    → 실제 읽기 수행
```

#### GC (7가지 알고리즘)

| 알고리즘 | 핵심 아이디어 | write amplification | 복잡도 |
|---|---|---|---|
| Greedy | valid 블록 가장 적은 zone 선택 | 높음 | O(N) |
| Cost-Benefit | valid 비율 × age 함께 고려 | 낮음 | O(N) |
| Fast Cost-Benefit | Cost-Benefit 빠른 근사 | 낮음 | O(N) |
| Approximative CB | 더 빠른 근사 | 낮음 | O(1) 근사 |
| Constant Greedy | 상수 시간 Greedy | 높음 | O(1) |
| Constant CB | 상수 시간 Cost-Benefit | 낮음 | O(1) |
| FeGC / FaGC+ | 학술 논문 기반 변형 | 최적화 | 복잡 |

GC 트리거: `reclaim_limit` 파라미터로 여유 zone 비율 임계값 설정. `op_rate`로 과프로비저닝 비율 설정 (일부 zone을 항상 비워 GC 여유 확보).

### dm-zap의 한계

**한계 1: 부분 변환만**

위쪽에 여전히 sequential zone이 노출된다. ext4를 그냥 올릴 수 없다. zone-aware 파일시스템이나 직접 블록 접근 애플리케이션(ex. RocksDB with zonefs)을 위한 보조 수단에 가깝다.

**한계 2: 메인라인 미포함, 유지보수 중단**

마지막 브랜치가 kernel 5.15 기준. 현재 커널에서 동작 보장 없음. 연구 프로토타입 수준으로 남아 있다.

**한계 3: 매핑 자료구조의 비효율**

Mapping Entry를 zone header/footer에 인라인으로 저장한다. compaction과 GC가 별개의 과정이라 LSM-Tree처럼 자연스럽게 결합되지 않는다.

---

## 4. dm-zoned vs dm-zap 비교

### 한눈에 보기

| 항목 | dm-zoned | dm-zap |
|---|---|---|
| **대상 하드웨어** | SMR HDD | ZNS SSD |
| **conventional zone 의존** | 필수 | 없음 |
| **위쪽 노출 인터페이스** | 완전한 conventional | 일부 conventional + 일부 sequential |
| **ext4 바로 사용 가능?** | ✓ (완전한 conventional이므로) | ✗ (sequential zone이 남아 있으므로) |
| **매핑 자료구조** | chunk 비트맵 | zone header/footer + mapping entry |
| **GC 알고리즘** | 단일 (valid 블록 이동) | 7가지 선택 가능 |
| **메타데이터 영속화** | ✓ (conventional zone에 저장) | ✓ (zone 0~1에 저장) |
| **크래시 복구** | ✓ (이중 메타데이터) | ✓ (세대 카운터) |
| **메인라인 포함** | ✓ (Linux 4.13+) | ✗ (연구 프로토타입) |
| **유지보수 상태** | 활성 | 사실상 중단 |

### 핵심 차이: "위쪽을 어디까지 conventional로 만드는가"

```
dm-zoned:
  물리 디바이스:  [conv zone][conv zone][seq zone][seq zone][seq zone]...
                  ↑ 메타데이터         ↑ 데이터
  논리 디바이스:  [    완전한 conventional 블록 디바이스    ]
  → ext4 바로 사용 가능

dm-zap:
  물리 디바이스:  [meta][meta][seq][seq][seq][seq][seq][seq]...
                               ↑ 외부엔 conventional  ↑ 외부엔 sequential
  논리 디바이스:  [conventional][conventional][sequential][sequential]...
  → ext4 사용 불가 (sequential zone이 위쪽에도 노출됨)
```

### 해결하는 문제의 범위

```
문제 스펙트럼:
"zone-unaware FS가 ZNS SSD에서 아무 수정 없이 동작해야 한다"

dm-zoned: 이 문제를 완전히 해결하지만 conventional zone 의존으로 ZNS SSD 단독 불가
dm-zap:   ZNS SSD 단독 가능하지만 zone-unaware FS를 완전히 지원하지 못함
본 과제:  ZNS SSD 단독 + zone-unaware FS 완전 지원 (둘 다 달성이 목표)
```

---

## 5. 본 과제의 포지셔닝

### 두 prior art의 교집합을 목표로

```
                conventional zone 없이
                ZNS SSD 단독 동작
                        │
              ┌─────────┼─────────┐
              │         │         │
           dm-zap      목표    dm-zoned
              │                   │
     부분 conventional       완전 conventional
     (ext4 불가)              (ZNS SSD 불가)
```

본 과제는 두 prior art가 각각 포기한 조건을 동시에 달성한다:
- dm-zoned처럼 **위쪽은 완전한 conventional** → ext4 수정 없이 사용
- dm-zap처럼 **conventional zone 없이** → ZNS SSD 단독

### 핵심 차별점: 전체 변환 + conventional zone 없이

```
물리 디바이스:  [seq][seq][seq][seq][seq]...  (ZNS SSD, 전부 sequential)
                 ↑ 일부를 메타 zone으로 사용 (dm-zap 방식을 차용)

논리 디바이스:  [완전한 conventional 블록 디바이스]
                 ↑ dm-zoned처럼 위쪽 전체가 conventional
```

이것이 가능하려면:
- 임의 LBA 쓰기를 **전부** sequential append로 변환해야 함
- 매핑 테이블이 모든 LBA → (zone, offset)을 추적해야 함
- GC로 zone을 재활용해야 함

그 매핑 자료구조로 **LSM-Tree**를 채택. 이유:
- LSM-Tree 자체가 append-only라 ZNS SSD 쓰기 패턴과 완벽히 일치
- compaction이 자연스럽게 GC와 결합됨 (오래된/invalid 매핑 제거)

---

## 6. 마일스톤별 목적 분석

각 마일스톤이 왜 이 순서로 설계됐는지, prior art 분석과 연결해서 이해한다.

---

### M0 — Zoned Passthrough (제공된 scaffold)

**목적**: DM 프레임워크 이해 + ZNS 인프라 검증

M0는 아무 변환도 하지 않는 passthrough다. 존재 이유:

1. **DM 프레임워크 학습**: `.ctr`, `.dtr`, `.map`, `.report_zones`, `.iterate_devices`가 어떻게 연결되는지 실제로 빌드하고 실행해보며 이해
2. **ZNS 인프라 검증**: null_blk + blkzone + DM이 올바르게 연동되는지 확인
3. **M1의 출발점 확보**: M0가 통과하면 "DM 레이어는 정상"이라는 기준선이 생긴다. M1에서 뭔가 잘못됐을 때 DM 문제인지 변환 로직 문제인지 분리할 수 있다.

```
M0 상태:
  위쪽: zoned (HM) — DM_TARGET_ZONED_HM 플래그
  아래쪽: zoned
  .map: bio_set_dev만 하고 섹터 번호는 그대로

성공 기준:
  blkzone report 정상 → .report_zones + .iterate_devices 연결 확인
  zone reset → wp 초기화 경로 동작 확인
  4 KiB 순차 쓰기 → wp 증가 확인
```

M0는 dm-zoned나 dm-zap의 기능을 하지 않는다. "DM 위에서 zoned 디바이스를 다룰 수 있는 뼈대"를 확보하는 것이 전부다.

---

### M1 — Random Write 변환 (핵심 작업)

**목적**: prior art 두 개가 각각 포기한 조건을 동시에 달성하는 핵심 메커니즘 구현

M1에서 두 가지가 **동시에** 바뀐다:

#### 변경 1: 위쪽 인터페이스를 conventional로

```c
// M0
.features = DM_TARGET_ZONED_HM,   // "나는 zoned 타깃"
.report_zones = zns_base_report_zones,  // zone 정보 위로 노출

// M1
.features = 0,                     // conventional처럼 광고
// .report_zones 제거              // zone 없음
```

이게 절반. 이것만 하면 ext4가 임의 쓰기를 내려보내는데, 아래는 여전히 sequential-only ZNS이므로 즉시 reject된다.

#### 변경 2: `.map`에서 임의 쓰기를 순차로 변환

```
LBA X에 쓰기 요청 수신 (임의 위치)
    │
    ├── 활성 zone의 현재 wp 위치 확인
    ├── wp에 데이터를 append (sequential write → ZNS 허용)
    ├── 매핑 테이블에 기록: LBA X → (현재 zone, wp_offset)
    └── wp 갱신

LBA X 읽기 요청 수신
    │
    ├── 매핑 테이블에서 X 조회 → (zone Z, offset O)
    └── /dev/nullb0의 zone Z, offset O 위치 읽기
```

이 두 변경이 동시에 이뤄져야 의미가 있다. 변경 1만 하면 ext4가 내려보내는 임의 쓰기가 ZNS에서 reject. 변경 2만 하면 위쪽이 여전히 zoned라 ext4 자체가 mount를 거부.

**dm-zoned와 비교**: dm-zoned도 같은 목표(임의→순차 변환)를 달성하지만 conventional zone을 이용해 메타데이터를 저장한다. M1은 in-memory 매핑으로 시작하고 영속화는 stretch.

**dm-zap과 비교**: dm-zap은 일부 zone만 변환. M1은 **전체** LBA 공간을 conventional로 변환한다는 점에서 더 완전하다.

```
M1 성공 기준:
  fio --rw=randwrite --bs=4k --size=100M ... (25,600개 임의 쓰기)
  dmesg에서 blk_update_request: I/O error 증가 = 0
  → 단 한 건의 reject도 없어야 변환이 완전히 동작하는 것
```

---

### M2 — ext4 라운드트립

**목적**: "파일시스템을 올려도 실제로 데이터가 맞는가" 검증

M1은 fio가 직접 블록 디바이스에 쓰는 raw 검증이다. M2는 실제 사용 시나리오: ext4를 올리고 파일을 쓰고 umount/remount 후 데이터가 그대로인지 확인한다.

```
mkfs.ext4 /dev/mapper/myzns   ← ext4가 superblock 등을 임의 위치에 씀
mount → 파일 쓰기 → umount   ← 페이지 캐시 비우기
mount → 파일 읽기 → md5 비교 ← DM 레이어를 실제로 거쳐 읽어야 함
```

umount/remount 사이에 검증하는 이유: remount 없이 읽으면 페이지 캐시에서 읽혀서 DM 레이어를 전혀 거치지 않는다. 매핑 테이블에 버그가 있어도 통과해버린다.

**prior art와의 관계**: dm-zoned는 M2에 해당하는 검증이 이미 통과돼 있다(메인라인이므로). 본 과제는 M1에서 만든 변환 레이어가 실제 파일시스템 워크로드를 감당하는지 M2에서 처음 확인한다.

---

### M3 — GC + Zone Reset 사이클

**목적**: 디바이스 용량이 다 차도 계속 사용할 수 있도록

M1~M2는 빈 공간에 쓰기만 한다. 실제로는 공간이 가득 차면 GC가 돌아 invalid 매핑을 정리하고 zone을 reset해 재사용해야 한다.

```
빈 공간 없음 → 활성 zone 없음 → 새 쓰기 불가
         ↓
GC 필요:
1. victim zone 선정 (valid 매핑이 적은 zone)
2. victim zone의 valid 데이터를 새 zone으로 복사
3. 매핑 테이블 갱신 (복사된 데이터의 새 위치로)
4. victim zone reset → 재사용 가능
```

**dm-zap의 GC 알고리즘 7가지가 이 단계의 선택지**가 된다. 본 과제에서는 LSM-Tree compaction이 이 GC와 자연스럽게 결합된다는 것이 설계상 장점이다.

```
M3 성공 기준:
  용량 80% 채운 후 1.2배 overwrite
  → "no space left" 없이 완료
  → GC가 실제로 동작하며 zone을 재활용하는 것 확인
```

---

### M4 — 성능 평가

**목적**: prior art(dm-zoned + 일반 SSD, F2FS zoned mode 등)와 정량 비교

M3까지는 "동작하는가"였다면 M4는 "얼마나 잘 동작하는가"다. null_blk은 지연이 0이라 성능 수치가 의미 없으므로 FEMU 환경에서 수행한다.

| 비교 대상 | 의미 |
|---|---|
| raw sequential write | 하드웨어 상한 (이보다 빠를 수 없음) |
| dm-zoned + ext4 (일반 SSD 동반) | 동종 기능, 다른 구현 |
| F2FS multi-device | zone-aware FS의 best case |
| ext4 on 일반 SSD | ZNS를 쓰는 비용 기준선 |

---

## 종합 정리

```
dm-zoned:  conventional zone 있는 디바이스에서 완전한 변환 ← 완성된 prior art
dm-zap:    ZNS SSD 단독이지만 부분 변환만               ← 미완성 prior art
본 과제:   ZNS SSD 단독 + 완전한 변환                   ← 두 prior art의 교집합

M0: DM 프레임워크 이해 + ZNS 인프라 기준선 확보
M1: 핵심 — 임의→순차 변환 + 매핑 테이블 (prior art가 각각 포기한 조건 동시 달성)
M2: 실제 ext4가 올라가는지 end-to-end 검증
M3: GC로 무한 사용 가능하도록 (LSM compaction과 결합)
M4: 성능 정량화 (FEMU 환경)
```
