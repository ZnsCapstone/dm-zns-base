# Keper 추가 설계 통합 실험

## 상태

초기 기준은 `exp/wal-writeback-20261021`의 `06b9d3e`이며 추가 구현은
`36dffa2`다. 사용자 요청으로 두 커밋을 `zns_dm/Kovalt03`에 fast-forward로
반영했다. 옵션 기본값은 유지하며, 최종 브랜치에서 원래 benchmark 4 조건의
재검증은 아직 수행하지 않았다.
로컬 회귀 38개, Python 문법 검사, 로컬 커널용 `W=1` 빌드가 통과했다.
연결 복구 후 guest 5.15.0-186에서 W=1 빌드와 CRC/재부착/extent 검증,
fio short 12조건을 완료했다. 수집 커널 오류 및 WAL 오류는 0이었다.
guest module SHA-256:
`455b0f073cd1b5af0304e7a31547346645bd9f7867495262b3e974e33b843c13`.
기본 옵션은 변경하지 않았으며 전원 장애 검증/최종 통합 완료를 뜻하지 않는다.
기존 `wal-writeback-review-20261021/final-v4` 결과는 이전 버전이며 이번 변경의
성능 증거로 사용하지 않는다.

## 출처와 대응

참고는 `ZnsCapstone/dm-zns-base`, `zns_dm/keper1212`, `380daed`의
`src/dm-zns-base.c`다. 함수 본문을 그대로 복사하지 않고 설계·제어 흐름을
우리 Zone Append/skiplist/WAL page/비동기 bio 소유권 구조에 맞게 구현했다.

| Keper 참고 부분 | 이번 구현 | 범위 |
|---|---|---|
| `zns_base_process_write_bio`, `zns_base_write_full_blocks` | `append_extent_sectors`, `start_foreground_write`, `extent_append_done` | 큰 정렬 쓰기를 장치 한도 내 단일 Append로 기록, 4KiB 매핑은 유지 |
| `zns_base_wal_prepare_group_locked`, `zns_base_wal_write_group_locked` | `wal_group_capacity`, `wal_batch_work_fn`, `wal_page_append_sync` | 여러 checksummed page, 마지막 page PREFLUSH+FUA |
| `zns_base_lookup_read_block`, private buffer 재사용 | `sstable_read_probe`, `sstable_probe_consume` | 한 조회 안에서 동일 512B immutable sector 재사용 |
| `zns_base_gc_pressure_mode_locked`, `zns_base_gc_work` | `gc_reclaim_threshold`, `gc_reclaim_one_victim` | background 회수율 20/10%, reserve에서는 기존 bootstrap 검사 |
| SSTable lookup 오류 반환 | `sstable_probe_done`, `sstable_read_finish` | 읽기/OOM 오류를 오래된 매핑 또는 zero-fill 성공으로 바꾸지 않음 |
| one-shot failpoint 설계 | `failpoint_hit`와 5개 hook | 아래 번호는 우리 구현용이며 Keper 번호와 의미가 전부 같지는 않음 |

공유 SSTable 블록/논리 결과 캐시, SRCU catalog 전체 교체, clean-zone cooldown,
부분 쓰기 RMW, Manifest 포맷은 이번 이식에 포함하지 않았다. 기존 GC 배치
읽기/쓰기와 snapshot 재사용은 중복 구현하지 않았다.

## 옵션과 안전 경계

기본값은 기존 크기/정책이다.

| 옵션 | 기본 | 추가 기능 비교값 |
|---|---:|---:|
| `wal_writeback` | 0 | 양쪽 모두 1 |
| `wal_batch_delay_ms` | 2 | 양쪽 모두 0 |
| `data_append_max_blocks` | 1 | 64 (최대 256KiB, 실제 장치 한도로 추가 제한) |
| `wal_group_pages` | 1 | 8 (최대 허용 16, writeback 필요) |
| `sstable_probe_cache` | 0 | 1 |
| `adaptive_gc` | 0 | 1 |

- 큰 쓰기는 장치의 max-zone-append/max-I/O와 4KiB 경계를 따른다. 읽기와
  부분 쓰기는 기존 4KiB 경로다. 물리 zone 경계는 기존 allocator가 보장한다.
- 부모 bio는 보유하고 별도 clone으로 data를 제출한다. data 완료 후 반환된
  실제 시작 주소에 4KiB offset을 더해 매핑한다. 자식 bio를 모두 chain한 뒤
  WAL에 넘긴다. 부모의 내부 reference count를 재설정하지 않는다.
- 모든 자식 context/bio를 data 제출 전에 준비한다. 할당/header/data 오류 시
  자식·예약·foreground 회계를 정리한다. 쓰기 context는 zero-initialize한다.
- 다중 WAL page는 각각 기존 포맷의 CRC를 갖는다. data FLUSH 뒤 순서대로
  기록하고 마지막 page의 PREFLUSH+FUA로 앞선 WAL page까지 영속화한다.
- 영속화 실패는 writeback 여부와 관계없이 metadata를 fail-closed한다.
- probe cache는 현재 조회의 pinned immutable sector만 재사용하며 epoch로
  조회를 재시작할 때 무효화한다. 공유 결과 캐시가 아니다.
- GC의 20/10%는 품질 기준일 뿐이다. 기존 workspace/WAL 보호 및 reset 안전
  검사를 유지한다. short에서 GC가 안 일어나면 GC 효율 개선은 평가할 수 없다.

## 장애 주입

`/sys/module/dm_zns_base/parameters/failpoint`는 root가 쓰는 one-shot 진단이다.
0은 비활성화이며 정상 경로에서 cmpxchg를 반복하지 않는다.

| 번호 | 주입 위치 |
|---:|---|
| 1 | data 완료 후 WAL staging 이전 오류 |
| 2 | data FLUSH 후 WAL 제출 이전 오류 |
| 3 | SSTable write 완료 후 publication 이전 오류 |
| 4 | checkpoint 제출 이전 오류 |
| 5 | zone reset 직전 오류 |

현재는 one-shot helper와 hook 경계/기존 오류 전파를 로컬에서 검증했다.
실제 FEMU fault injection과 power-loss 시험을 수행했다고 해석하면 안 된다.

## 로컬 검증

```bash
python3 scripts/test-safety-unit.py       # 21개; 읽기 오류의 stale/zero 반환 방지도 포함
python3 scripts/test-wal-userspace.py     # 5개
python3 scripts/test-wal-writeback.py     # 7개
python3 scripts/test-keper-extensions.py  # 5개
make -C src W=1
```

## 원격 short 비교 실행

guest 자체 빌드한 새 모듈로만 다음을 실행한다. 고정된 team4 FEMU raw 장치를
초기화하는 스크립트이므로 다른 장치/기존 실험이 있으면 실행하지 않는다.

```bash
python3 run-wal-writeback-smoke.py --run --compare-extensions \
  --module /home/zns/keper-extensions-20261021/module-src/dm-zns-base.ko \
  --out /home/zns/kafka_python_result/results/final/keper-extensions-short-20261021 \
  --seconds 20
```

`policy=0`은 기존 writeback ON, `policy=1`은 writeback ON+추가 옵션이다.
EXT4/F2FS × 순차 4KiB/랜덤 4KiB/순차 1MiB × 2정책 = 12조건이며,
각 조건마다 zone reset/mkfs/256MiB 파일 준비를 새로 한다. QD32/direct/libaio,
종료 fsync, discard OFF, 같은 VM boot의 단일 반복이다.

성능 실행 전에 작은 threshold의 CRC/FLUSH/재부착, 미영속 tail의 정상 제거 후
복구, 큰 1MiB write의 CRC와 실제 Append 수 감소를 검사한다. 이들은 커널
동시성·갑작스러운 전원 차단·장기 GC를 대신하는 검증이 아니다.

## 장기 GC 비교 검증 범위

2026-10-22 EXT4 쓰기전용 5,000 ops/s에서 패치 전후 모두 30분 측정을
완료했고 유효 판정 및 sampler 오류 0을 확인했다. 양쪽 모두 80% prefill 후
모든 파티션 retention과 추가 ACK payload 10GiB를 기다렸다. 본 측정 시작
실제 FS 점유율은 약 90%였다. 측정 경계 차분의 DM WA는 10.09→3.93,
논리 쓰기당 GC 이주 비율은 8.96→2.88이었다. 이는 단일 반복에서 관측한
묶음 변경 효과이지, 개별 옵션의 인과 효과나 NAND WA가 아니다.
측정 중 조기 ACK 증가가 0이므로 writeback 조기 완료의 효과로 단정하지 않는다.

추가 버전 EXT4 10,000 ops/s는 dmsetup status 조회의 3초 timeout으로
중단됐다. 이 조건을 성공 결과로 취급하지 않으며 timeout의 원인은 미해결이다.
Native F2FS 및 DM F2FS의 동일 장기 조건은 아직 완료하지 않았다.
상세 원본은 별도 benchmark 결과 저장소에 보존하며 이 문서는 구현 검증 범위만
기록한다. 실제 FEMU 장애 주입/전원 차단 및 독립 반복 검증은 남아 있다.
