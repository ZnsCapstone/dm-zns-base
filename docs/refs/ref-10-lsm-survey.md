# LSM-Tree 서베이: "LSM-based Storage Techniques: A Survey"

> 출처: https://link.springer.com/article/10.1007/s00778-019-00555-y  
> 저자: Chen Luo, Michael J. Carey (UC Irvine)  
> 발행: VLDB Journal 2019 | arXiv: 1812.07527

## 개요

LSM-Tree(Log-Structured Merge-Tree)가 현대 NoSQL 시스템(LevelDB, RocksDB, Cassandra, HBase 등)에서 스토리지 계층으로 광범위하게 채택된 이유, 구조, 개선 연구들을 체계적으로 정리한 서베이 논문이다.

## 1. LSM-Tree 기본 구조

### 핵심 아이디어

기존 B-tree는 임의 위치 덮어쓰기(in-place update)를 한다. 이는 디스크 임의 쓰기를 유발하고 ZNS SSD에서는 아예 불가능하다. LSM-Tree는 모든 쓰기를 **순차 append**로만 처리한다.

```
쓰기 요청
    ↓
MemTable (in-memory, 정렬된 버퍼)
    ↓ 가득 차면 flush
L0 (디스크, SST 파일들 — 겹침 허용)
    ↓ compaction
L1 (디스크, SST 파일들 — 겹침 없음)
    ↓ compaction
L2 ...
    ↓
Lmax
```

### 구성 요소

**MemTable**: 메모리 내 정렬 자료구조(보통 Skip List). 쓰기는 여기 먼저.

**WAL (Write-Ahead Log)**: 크래시 복구용. MemTable에 쓰기 전 먼저 디스크에 순차 기록.

**SST (Sorted String Table)**: MemTable이 flush되거나 compaction 결과로 생성되는 불변(immutable) 디스크 파일. 키 순서로 정렬.

**Bloom Filter**: 각 SST마다 존재. "이 키가 이 파일에 없다"를 O(1)로 판별해서 읽기 성능 향상.

## 2. 읽기 / 쓰기 / 삭제

### 쓰기

```
1. WAL에 순차 append
2. MemTable에 삽입
3. MemTable 가득 차면 → Immutable MemTable로 전환
4. Background thread가 L0에 SST로 flush
```

디스크 쓰기가 항상 **순차**다 → ZNS SSD와 자연스럽게 호환.

### 읽기

```
1. MemTable 검색
2. Immutable MemTable 검색
3. L0의 SST들 검색 (Bloom Filter로 필터링)
4. L1, L2, ... 순서로 검색 (최신 데이터가 위 레벨에 있음)
```

최악의 경우 모든 레벨을 탐색 → 읽기 증폭(Read Amplification) 문제.

### 삭제

"tombstone" 키를 삽입. 실제 삭제는 compaction 시점에 발생.

## 3. Compaction 전략

LSM-Tree 성능의 핵심. Write Amplification(WA)과 Space Amplification(SA), Read Amplification(RA)의 트레이드오프를 결정한다.

### Leveled Compaction (LevelDB, RocksDB 기본)

```
L0: SST들이 겹침 허용 (최대 4개)
L1: 키 범위 겹침 없음, 고정 크기 (예: 10 MB)
L2: L1의 10배 크기 (100 MB)
L3: L2의 10배 (1 GB)
...
```

L(i)가 임계치 초과 → L(i)의 SST 하나를 선택해 L(i+1)의 겹치는 SST들과 병합.

- **장점**: 읽기 성능 좋음 (각 레벨에서 최대 1개 SST만 확인), 공간 효율 좋음
- **단점**: Write Amplification 높음 (데이터가 여러 번 다시 씌어짐)

### Tiered Compaction (Cassandra, RocksDB Universal)

같은 크기의 SST N개가 쌓이면 하나로 합친다.

```
Tier 0: [SST][SST][SST][SST] → 합쳐서 Tier 1의 SST 하나
Tier 1: [SST][SST][SST][SST] → 합쳐서 Tier 2의 SST 하나
...
```

- **장점**: Write Amplification 낮음
- **단점**: 공간 효율 나쁨, 읽기 느림

### FIFO Compaction

시간 순서로 삭제. 시계열 데이터처럼 오래된 데이터를 자동 만료할 때 사용.

### Dostoevsky / Fluid LSM

Leveled와 Tiered를 섞는 하이브리드. 레벨별로 전략을 다르게 설정.

## 4. Write Amplification 문제

WA = 실제 디스크에 쓰인 데이터 / 사용자가 요청한 데이터

Leveled Compaction에서 WA는 레이어 당 배율의 곱에 비례. 10배 배율, 5레벨이면 WA ≈ 50.

이 WA가 ZNS SSD에서 특히 중요하다. SSD 수명은 총 쓰기 바이트에 반비례하므로, WA를 줄이는 것이 ZNS + LSM-Tree 시스템의 핵심 과제 중 하나.

## 5. 주요 시스템

| 시스템 | Compaction 전략 | 특징 |
|---|---|---|
| **LevelDB** (Google) | Leveled | LSM-Tree 구현의 기준. 단순하고 이해하기 쉬움 |
| **RocksDB** (Meta/Facebook) | Leveled (기본) + Universal | LevelDB 기반, 프로덕션 수준. 수많은 튜닝 옵션 |
| **Cassandra** (Apache) | STCS (Size-Tiered) | 분산 NoSQL, Tiered 기반 |
| **HBase** (Apache) | Leveled | HDFS 위에서 동작하는 분산 KV |
| **AsterixDB** | LSM-based | 복잡한 데이터 타입 지원 |

## 6. ZNS SSD와 LSM-Tree의 자연스러운 결합

LSM-Tree의 모든 쓰기 패턴이 ZNS SSD의 제약에 맞다.

| LSM-Tree 동작 | ZNS SSD 관점 |
|---|---|
| MemTable → SST flush | 순차 append → wp에 순차 쓰기 ✓ |
| SST compaction 결과 쓰기 | 새 zone에 순차 append ✓ |
| 오래된 SST 삭제 | zone reset ✓ |
| tombstone 처리 | compaction 시 GC와 결합 ✓ |

이것이 본 과제에서 LSM-Tree를 매핑 자료구조로 선택한 이유다. 매핑 테이블 자체를 LSM-Tree로 구성하면 내부 compaction이 자연스럽게 GC(invalid 매핑 제거)와 결합된다.

## 7. 본 과제와의 연관

본 과제는 RocksDB 같은 **애플리케이션 레벨** LSM-Tree가 아니라, **블록 레이어**에서 LBA → (zone, offset) 매핑을 LSM-Tree 구조로 유지한다. 개념은 같지만 동작하는 레이어가 다르다.

```
일반적인 LSM-Tree KV 스토어:
  사용자 key → 물리 위치 매핑을 LSM-Tree로 관리

본 과제:
  논리 LBA → 물리 (zone, offset) 매핑을 LSM-Tree로 관리
  (커널 블록 계층에서 동작)
```

M1의 in-memory 매핑 테이블을 LSM-Tree 구조로 설계하면, M3의 GC가 compaction으로 자연스럽게 구현된다.
