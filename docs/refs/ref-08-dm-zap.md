# dm-zap (Western Digital)

> 출처: https://github.com/westerndigitalcorporation/dm-zap

## 개요

`dm-zap`은 Western Digital이 개발한 리눅스 커널 DM 타깃이다. **Sequential Write Required(SWR) zone을 conventional zone처럼 보이게** 변환한다. ZNS SSD 친화적으로 설계됐으며, dm-zoned의 한계(conventional zone 의존)를 극복하려는 시도다.

현재 상태: 연구 프로토타입. 메인라인 미포함, 활발한 유지보수 없음(Stars 29, 마지막 브랜치: 5.15_dm-zap).

## 이 프로젝트와의 비교

| 항목 | dm-zap | 본 과제 (dm-zns-base) |
|---|---|---|
| conventional zone 의존 | 없음 (ZNS-only) | 없음 (ZNS-only) |
| 위쪽 노출 방식 | C개 conventional + N-C개 sequential | 전체 conventional |
| 매핑 자료구조 | zone header/footer + mapping entry | LSM-Tree |
| GC 알고리즘 | 7가지 선택 가능 | 과제 산출물 |
| 상태 | 연구 프로토타입 | 졸업프로젝트 |

## 디스크 레이아웃

```
Zone 0~1:     메타데이터 영역
              - Zone 0: 슈퍼블록 + 매핑 데이터
              - Zone 1: 백업 메타데이터

Zone 2~R-1:   랜덤 쓰기 가능 zone (dm-zap이 관리)
              - 외부에는 conventional zone으로 보임
              - 실제로는 sequential zone

Zone R~N-1:   순차 쓰기 zone (직접 passthrough)
              - 외부에도 sequential zone으로 노출
```

## 메타데이터 구조

### Zone Header (512 bytes, zone 시작)

```
MAGIC: {'z','a','p','0'}
VERSION, SEQUENCE_NR, TYPE
```

### Zone Footer (512 bytes, zone 끝)

매핑 데이터 시작 위치를 기록.

### Mapping Entry (8 bytes)

```
START   : 논리 주소 시작
LENGTH  : 길이
ADDRESS : 물리 주소 (zone, offset)
```

## 핵심 동작

### 쓰기 처리

임의 LBA 쓰기 → 활성 랜덤 zone의 wp에 append → mapping entry 기록 → 나중에 GC에서 정리.

### 읽기 처리

mapping entry lookup → 해당 zone의 물리 offset으로 변환 → 읽기 수행.

### GC (Garbage Collection)

dm-zap은 7가지 GC 알고리즘을 제공한다:

| 알고리즘 | 설명 |
|---|---|
| Greedy | valid 블록이 가장 적은 zone 선택 (단순, 높은 write amp) |
| Cost-Benefit | valid 비율과 age를 함께 고려 |
| Fast Cost-Benefit | Cost-Benefit의 빠른 근사 버전 |
| Approximative Cost-Benefit | 더 빠른 근사 |
| Constant Greedy | 상수 시간 Greedy |
| Constant Cost-Benefit | 상수 시간 Cost-Benefit |
| FeGC / FaGC+ | 학술 논문 기반 변형 |

### GC 트리거 파라미터

```
op_rate        : 과프로비저닝 비율 (%)
reclaim_limit  : GC 시작 임계값 (여유 zone %)
victim_selection_method : GC 알고리즘 선택
```

## 코드 구조 (참고용)

```
dm-zap/
├── dm-zap.c         ← 메인 DM 타깃 코드 (.ctr, .dtr, .map)
├── dm-zap-target.c  ← target_type 등록
├── dm-zap-gc.c      ← GC 알고리즘들
├── dm-zap-meta.c    ← 메타데이터 읽기/쓰기
└── dm-zap-reclaim.c ← reclaim 워커
```

## 이 프로젝트 구현 시 참고 포인트

1. **`.map`에서 BIO 분류**: 쓰기인지 읽기인지, 어느 zone으로 보낼지 결정하는 패턴 참고
2. **GC 기본 패턴**: victim zone 선정 → valid 블록 이동 → reset 사이클 (M3 구현 시)
3. **메타데이터 없이 시작**: dm-zap은 persistent 메타데이터를 처음부터 가지지만, 본 과제는 in-memory부터 시작 (crash 복구는 stretch)
4. **위쪽 노출 방식**: dm-zap은 일부 zone만 conventional로 변환하지만, 본 과제는 전체를 conventional로 노출

## 빌드 방법 (참고)

```bash
git clone https://github.com/westerndigitalcorporation/dm-zap
cd dm-zap
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```
