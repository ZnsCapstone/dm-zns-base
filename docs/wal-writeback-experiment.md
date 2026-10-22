# 요청 기반 WAL 영속화 실험

이 문서의 초기 구현은 `06b9d3e` 기준이다. 이후 추가 옵션과 검증 대기 상태는
`keper-extensions-experiment.md`를 참고한다.

## 출처와 구현 범위

참고 저장소: `ZnsCapstone/dm-zns-base`, `zns_dm/keper1212`, commit
`380daed` (2026-10-12). 참고 파일은 `src/dm-zns-base.c`다.

- `zns_base_io` / `requires_durable_commit`: FUA/PREFLUSH 요청과 일반 쓰기 구분.
- `zns_base_wal_stage_foreground`의 `commit->io = durable ? io : NULL`:
  일반 쓰기의 원본 bio 수명과 메모리 WAL 레코드 수명 분리.
- `zns_base_wal_schedule_flush`: timer가 아닌 그룹 포화/명시적 경계에서 기록.
- `zns_base_wal_write_group_locked`: data FLUSH 후 WAL 그룹 FUA 기록.

위는 설계와 제어 흐름을 참고한 부분이며 **함수 본문을 그대로 복사하지 않았다.**
우리의 `zns_io_ctx`, skiplist, Zone Append, WAL page 형식 및 기존 GC를 이용해
`wal_try_buffer_write`, `wal_complete_buffered`, WAL worker의 경계를 독립 구현했다.
초기 커밋에서는 Kepler의 매핑 slot/다중 page 그룹/Manifest 구현을 이식하지 않았다.

## 활성화

기본값 `wal_writeback=0`은 기존 durable 완료 정책이다.
실험 모듈 적재 시에만 `wal_writeback=1 wal_batch_delay_ms=0`을 지정한다.
일반 WRITE는 data append 완료 뒤 매핑/메모리 WAL에 반영하고 조기 완료한다.
pending 버퍼는 `wal_batch_max_records`(기본 127개) 이하이며,
기록 중 그룹까지 포함한 volatile PUT은 최대 두 그룹(254개)으로 제한한다.

영속화 경계는 그룹 포화, FLUSH/FUA/PREFLUSH, memtable 임계값,
GC 진입, WAL 회전 및 정상 target 제거다. 그룹 기록은 data/header FLUSH 후
WAL FUA 순서다. FUA만으로 이전 data가 영속화됐다고 가정하지 않는다.

## 보수적인 차이와 안전 경계

- 일반 PUT만 조기 완료한다. DISCARD는 기존 durable 경로다.
- GC 활성, checkpoint/회전, durable waiter가 있는 WAL worker, 기존 pending durable waiter,
  pending FLUSH 또는 여유 zone 부족에서는 기존 완료 방식으로 돌아간다.
- buffered 전용 그룹을 기록하는 동안 다음 그룹을 모을 수 있지만, 원본 bio를
  보유한 그룹과 조기 공개가 교차해 매핑 순서가 뒤집히는 것은 금지한다.
- volatile WAL이 있으면 GC snapshot/reset을 시작하지 않는다.
- `ti->flush_supported`로 DM 자체의 volatile cache를 상위 커널에 알린다.
  하위 장치가 write-through여도 FLUSH/FUA가 제거되어서는 안 된다.
- 메모리 WAL이 pending/worker에 있으면 memtable checkpoint frontier를 만들지 않는다.
- 조기 공개된 매핑을 WAL 완료 때 다시 PUT하지 않는다(새 overwrite 역전 방지).
- checkpoint는 PREFLUSH+FUA, compaction은 새 출력 FLUSH 뒤 old zone 회수한다.
- 이미 ACK한 일반 쓰기의 WAL 오류는 타깃을 fail-closed하고 후속 barrier를
  실패시킨다. volatile 기록이 있을 때 WAL ENOSPC를 GC로 우회하지 않는다.

이는 실제 write-back 캐시다. barrier 없는 일반 쓰기는 전원 장애 시 유실될 수
있다. 기본 정책과 다른 영속성 계약이므로 일반 쓰기의 IOPS 증가를 동일한
per-write durability 성능 향상으로 해석하면 안 된다.

## 로컬 검증

```bash
python3 scripts/test-safety-unit.py
python3 scripts/test-wal-userspace.py
python3 scripts/test-wal-writeback.py
make -C src W=1
```

새 테스트는 실제 조기 완료/완료 정리 C helper를 ASan/UBSan으로 실행하고
기본 정책, FUA/PREFLUSH 제외, GC/회전/공간 guard, 그룹 포화, 중복 ACK 방지,
overwrite 역전 방지와 지연 오류를 검사한다. 나머지는 코드 경계 구조 회귀다.
LSan은 관리형 실행기의 ptrace 제약으로 비활성화하고 해제 횟수를 별도 검증한다.

사용자 공간 helper/구조 테스트는 커널 동시성·전원 장애·장기 GC의 증명이 아니다.
FEMU smoke는 별도 결과 디렉터리에 commit/source/module SHA와 원본을 보존하며,
짧은 실행 결과를 최종 성능 또는 crash consistency 보장으로 제시하지 않는다.

## FEMU smoke 실행

`scripts/run-wal-writeback-smoke.py`는 `--run` 없이는 계획만 출력한다.
실행하면 고정된 `/dev/nvme0n1`을 **초기화**한다. `zns-guest`, FEMU 모델,
32GiB, 512MiB zone 64개, 기존 DM/mount/부하기 없음과 모듈 vermagic를 검사한다.
호스트/다른 팀/루트 디스크에 사용하는 스크립트가 아니다.

```bash
python3 run-wal-writeback-smoke.py --run \
  --module /별도/게스트빌드/dm-zns-base.ko \
  --out /home/zns/kafka_python_result/results/final/새로운-결과폴더 \
  --seconds 12
```

- 작은 threshold에서 CRC write/read, FLUSH, target 재부착 후 CRC 읽기.
- 명시적 fsync 없이 4KiB tail을 남긴 뒤 정상 제거·재부착 후 CRC 읽기.
- 동일 모듈의 OFF/ON × EXT4/F2FS × 순차 4KiB/1MiB, 총 8개 short 조건.
- 파일 256MiB, direct/libaio, QD32, 각 12초, 끝에서 fsync, discard OFF.
- 각 정책/FS 전환 시 raw zone reset 및 새 mkfs. 동일 VM boot이고 독립 반복은 아님.
- 실제 write-cache 광고, 조기 ACK/commit/error 카운터, 커널 로그와 SHA 보존.
- udev settle 및 일반 `dmsetup remove --retry`를 사용하며 강제 제거는 하지 않는다.

짧은 기능 smoke에는 별도 warmup/반복/호스트 간섭 통제가 없으므로 처리량 비율은
개선 가능성을 보는 참고치다. GC 압력·전원 차단·동시성 스트레스 검증은 남아 있다.
