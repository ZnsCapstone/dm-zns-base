# dm-zns-base — 프로젝트 개요

졸업프로젝트 *"엣지 컴퓨팅을 위한 동적 플랫폼(Apache Kafka)과 ZNS(Zoned Namespace) SSD 호환을 위한 리눅스 커널 블록 계층 개발"* 의 학생용 base repo.

목표는 sequential-only ZNS SSD 위에서 ext4 같은 zone-unaware 파일시스템이 동작하도록 임의 쓰기를 순차 쓰기로 변환하는 LSM-Tree 기반 Device Mapper 타깃을 만드는 것. 본 repo는 그 출발점으로 zoned-aware pass-through DM 타깃, 로컬 샌드박스(zoned `null_blk`), 마일스톤 정의를 제공한다. LSM-Tree 매핑·GC·메타데이터 영속화는 *과제의 산출물*이라 포함하지 않는다.

## Quickstart

| 호스트 | 게스트 | 자세히 |
|---|---|---|
| 네이티브 Linux | 불필요 | [docs/01](01-setup-linux.md) |
| Windows | `vagrant up` (자동) | [docs/04](04-setup-windows.md) |
| Mac (Apple Silicon / Intel) | UTM + Ubuntu | [docs/02](02-setup-mac.md) |

Linux 게스트가 준비되면 동일:

```bash
sudo bash scripts/nullblk-up.sh   # /dev/nullb0 (zoned, 2GB, 64MB × 32 zones)
cd src && make && cd ..
sudo bash scripts/test-basic.sh   # ALL CHECKS PASSED 까지
```

## 문서

| 문서 | 내용 |
|---|---|
| [00-overview](00-overview.md) | 과제 배경, dm-zoned / dm-zap 비교, 본 과제의 차별점 |
| [01-setup-linux](01-setup-linux.md) | 네이티브 Linux |
| [02-setup-mac](02-setup-mac.md) | Mac (UTM) |
| [04-setup-windows](04-setup-windows.md) | Windows (Vagrant + VirtualBox) |
| [05-nullblk-zoned](05-nullblk-zoned.md) | zoned `null_blk` 사용법과 한계 |
| [06-build-and-run](06-build-and-run.md) | 빌드 / 적재 / 테스트 / 진단 |
| [07-milestones](07-milestones.md) | M0 → M4 마일스톤과 성공 기준 |
| [08-references](08-references.md) | 공식 문서, prior art, 참고 자료 |

## 구조

```
dm-zns-base/
├── src/         커널 모듈 소스 + Makefile
├── scripts/     nullblk 셋업, 자동 테스트
├── docs/        셋업·마일스톤·참고자료
├── Vagrantfile  Windows용 자동 VM
└── provision.sh
```

GPL-2.0. 각 소스 파일의 SPDX 헤더 참고.
