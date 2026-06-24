# FEMU (Flash Emulator)

> 출처: https://github.com/MoatLab/FEMU

## 개요

FEMU는 **QEMU/KVM 기반 NVMe SSD 에뮬레이터**다. 실제 SSD 하드웨어 없이 애플리케이션 → OS → NVMe 인터페이스 전체 스택에서 SSD 동작을 정확하게 에뮬레이션한다. ZNS SSD 에뮬레이션을 지원하며, 이 프로젝트의 서버 단계 실험 환경이다.

ASPLOS, OSDI, SOSP, FAST 등 최고급 학술대회에서 광범위하게 사용. 2018년 FAST 논문 기반.

## null_blk vs FEMU 비교

| 항목 | null_blk (로컬) | FEMU (서버) |
|---|---|---|
| 용도 | 코드 정확성 검증 | 성능 평가 |
| I/O 지연 | ~0 (메모리) | 실제 플래시 수준 (수 µs ~ ms) |
| NVMe 명령 경로 | 없음 | 있음 (Zone Append 포함) |
| 설정 복잡도 | 낮음 | 높음 (VM 필요) |
| fio 결과의 의미 | 없음 | 있음 |

## 5가지 에뮬레이션 모드

| 모드 | FTL 위치 | 주요 용도 |
|---|---|---|
| **BlackBox (BBSSD)** | 장치 내부 | 상용 SSD 에뮬레이션 |
| **WhiteBox (OCSSD)** | 호스트 | OpenChannel SSD 연구 |
| **ZNS (ZNSSD)** | zone 기반 | ZNS SSD 연구 ← 본 과제 |
| **NoSSD** | 없음 | SCM(Storage Class Memory), <10µs 지연 |
| **CSD** | 장치+계산 | Computational Storage 연구 |

## ZNS 에뮬레이션 구조

```
FEMU ZNS 모드
├── hw/femu/zns/         ← ZNS 에뮬레이션 구현
│   ├── zns.c            ← Zone 상태 관리, 명령 처리
│   ├── zns-ns.c         ← Namespace 관리
│   └── zns-util.c       ← 유틸리티
└── hw/femu/bbssd/
    └── ftl.c            ← 플래시 타이밍 모델
```

ZNS 모드는 zone별 wp 추적, Zone Append 명령, max_open/active_zones 제약을 실제 NVMe 명령 경로로 처리한다.

## 주요 설정 파라미터 (BlackBox 기준)

```bash
# 플래시 타이밍 (µs 단위)
secs_per_pg=8         # 페이지당 섹터
pgs_per_blk=256       # 블록당 페이지
pg_rd_lat=40000       # 읽기 지연: 40 µs
pg_wr_lat=200000      # 쓰기 지연: 200 µs
blk_er_lat=2000000    # erase 지연: 2 ms
```

## 설치 및 실행

```bash
# 빌드
git clone https://github.com/MoatLab/FEMU.git
cd FEMU && mkdir build-femu && cd build-femu
../femu-scripts/femu-copy-scripts.sh .
sudo ./pkgdep.sh      # 의존성 설치
./femu-compile.sh     # QEMU 기반 빌드

# ZNS 모드 VM 실행
./run-zns.sh
```

## 시스템 요구사항

| 항목 | 최소 | 권장 |
|---|---|---|
| CPU | x86_64, VT-x/AMD-V 지원 | 16코어+ |
| RAM | 12 GB | 32 GB+ |
| 디스크 | 20 GB 여유 | NVMe SSD 100 GB+ |
| OS | Ubuntu 18.04+ | Ubuntu 22.04 |

## 이 프로젝트에서의 사용 시점

```
로컬 (null_blk)                서버 (FEMU)
    ↓                               ↓
M1 구현 → M2 → M3 정확성 검증   M4 성능 평가
코드가 맞는가?                  얼마나 빠른가?
```

M3까지는 null_blk에서 개발·검증. M4 성능 평가 단계에서 FEMU를 사용해 실제 NVMe 명령 경로와 플래시 타이밍으로 fio 벤치마크를 수행한다.
