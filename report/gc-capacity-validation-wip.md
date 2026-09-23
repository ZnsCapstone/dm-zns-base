# WIP: 공간 고갈 원인 검증 및 benchmark 4 통과 계획

2026-09-24. 기준 코드: `00d0b54`.

이 문서는 **검증 설계**다. 아래 추가 계측과 재현 테스트는 아직 구현하지
않았다. 용량 문제를 해결했다거나 EXT4/F2FS가 통과했다는 의미가 아니다.
기존 안전성 WIP를 보존하여 정책 변경 전후를 비교한다.

## 목표와 고정 조건

최종 목표는 EXT4와 F2FS가 모두 기존 4번 endurance 조건을 통과하는 것이다.
65% 논리 용량이나 discard ON으로 바꾼 통과는 원인 분리 결과이지 최종 통과가 아니다.

- Ubuntu guest 5.15.0-186-generic; guest에서 모듈 빌드.
- FEMU 32GiB, 실제 zone report/zone capacity 기록. 기존 관측은 16 x 2GiB.
- DM logical 75% = 24GiB, reserve/low/high = 2/4/5.
- FS prefill 80%, 1024B, scenario_b, 8 producers, 20,000 ops/s.
- warmup 300s, measurement 3600s, Kafka retention 총 2GiB,
  segment 128MiB/60s. 기존 기본 mount 조건(EXT4 discard OFF,
  F2FS nodiscard)을 기준으로 한다. 실행 로그에서 실제 설정 확인.
- 각 실행에서 깨끗한 실험 장치로 시작하고 다른 실행과 중첩하지 않는다.
- 코드 SHA뿐 아니라 dirty diff, module/JAR SHA256, vermagic, FEMU build,
  mount options, topic config, 파티션 수, 실제 prefill 바이트도 보존한다.

`65% logical`과 `65% prefill`은 다른 변수다. 기존 EXT4 로그에서 logical
65%/75%는 각각 20/24GiB였고, 둘 다 prefill 80%였으며 prefill 쓰기는
14.80/17.79GiB였다. prefill 파일은 유지된다. retention은 Kafka 데이터만
순환시키므로 누적 쓰기가 늘어도 이 영구 live 데이터 차이는 없어지지 않는다.
Kafka 실제 점유량은 segment 삭제 지연/인덱스 등으로 retention 설정과 다르다.

## 증거와 미확정 사항

| 실행 | 결과 | 증명하지 못한 것 |
|---|---|---|
| 이전 65% EXT4 | 1시간 Valid=True | 최신 코드의 안정성, 무기한 정상 회수 |
| RwyaQmNL / 00d0b54 / EXT4 75% discard OFF | free=reserve=2, 무진전 3회, 할당 실패, journal abort | 최초 공간 손실 원인 |
| 2aefWNvi / 00d0b54 / EXT4 75% discard ON | 같은 실패 경로, 실제 discard mount 확인 | discard의 DM 내부 처리 정확성 |
| NOfUB4Xr / 이전 코드 | cached GC map 물리 주소 중복, consumer offset regression, 이후 free=0 | 중복이 실제 read map에도 존재하는지, consumer reset과의 인과 |
| 이전 F2FS 75% nodiscard | free=1, live>used, 할당 고갈/ACK stall | 최신 안전성 코드에서 동일 원인인지 |

일반 NVMe 위 동일 FS가 benchmark를 통과하는 것은 중요한 대조군이다.
하지만 물리 성능/장치 내부 OP 차이를 DM 버그 증거로 단독 사용하지 않는다.
DM 할당 오류와 FS journal abort는 통계 파싱 문제와 별개다.

## 가설과 반증 가능한 테스트

### H1: 조기 seal과 victim 승인 조건 불일치 (최우선)

`gc_work_fn`은 free>reserve+1이고 active USER_DATA의 invalid_count>0이면
active 지정을 제거한다. `gc_reclaim_one_victim`은 invalid 비율 5% 또는
used-live-WAL 순이익 조건으로 같은 zone을 거절할 수 있다.

- 재현: free=4/reserve=2, 부분 사용 USER_DATA(약 306MiB), 소량 invalid,
  나머지 closed 데이터 zone은 fully-live. GC 이전/이후 foreground tail 비교.
- 실제 production helper를 이용해 seal -> victim 판정 -> foreground allocation
  순서를 테스트한다. 단순히 독립적인 모형에 같은 논리를 다시 쓰지 않는다.
- 이전 로그의 zone 11을 `sealed active USER_DATA` 이벤트와 연결한다.
- 확인 기준: reset/reclaim 없이 foreground가 쓸 수 있는 tail이 사라지고
  신규 zone 소비가 증가함. seal이 없거나 이미 tail이 없는 상태라면 이 가설로
  해당 실패를 설명할 수 없다.
- 수정 후보: 불필요한 seal 제거 또는 실제 회수/진행 가능성을 만족할 때만 seal.
  partial zone의 전체 capacity만 gain에 대입하는 변경은 단독 해결책이 아니다.

### H2: 용도별 공간 분리 / reserve 경계 진행 불능

USER_DATA, GC_DATA, WAL, SSTABLE의 active tail은 서로 다른 자원이다.
총 빈 바이트나 reset 횟수만으로 foreground 진행을 판단할 수 없다.

- 재현: free=2, USER_DATA 소진, GC_DATA tail 충분/부족 두 경우;
  WAL tail 충분/rollover 두 경우; partial victim과 fully-live victim 분리.
- foreground와 GC가 교차할 때도 예약한 data/WAL 작업 공간이 보존되는지 확인.
- 확인 기준: GC 전후 FREE 개수, 일반 쓰기 가능 바이트, 추가 성공 write 수.
  새 GC zone 1개 소비 + victim 1개 반환을 순 free 증가로 계산하지 않는다.
- 수정 후보는 정상 쓰기 진행과 이주 완료 공간을 함께 보장해야 한다.
  reserve 축소, 무제한 차용, 무한 재시도는 해결로 인정하지 않는다.

### H3: 오래된 매핑 / GC snapshot이 live를 과대 계산

- 작은 고정 LBA 집합을 여러 번 덮어쓴다. payload는 LBA+generation+CRC로
  식별하며 기대값 oracle은 실험 장치 바깥에 둔다.
- active -> frozen -> SSTable -> compaction, cached GC victim 전환,
  zone reset/reuse를 반드시 통과시킨다. foreground overwrite와 relocation
  충돌도 포함한다. discard는 별도 케이스로 분리한다.
- 같은 일관된 시점에서 oracle, 실제 lookup 경로, fresh GC map, cached GC map을
  비교한다. 시점이 다른 snapshot의 차이를 corruption으로 판정하지 않는다.
- 성공 ACK 이후 readback은 최신 generation이어야 한다. LBA 집합을 늘리지 않는
  overwrite에서 논리 live key 수는 증가하지 않아야 한다.
- dedup 없는 서로 다른 live LBA의 물리 범위 중복, WP 밖 매핑,
  reset될 zone을 가리키는 최신 매핑은 실패다.
- 수정은 최초 잘못된 갱신/병합/재사용 순서를 특정한 후 해당 경로에 한정한다.
  consumer regression만으로 storage replay를 확정하지 않는다.

## 필요한 추가 계측 (구현 예정)

주기적 저빈도 snapshot과 최초 seal/할당 거절/relocation 실패에서 bounded
event를 수집한다. 대규모 스캔이나 출력은 spinlock 안에서 수행하지 않는다.

1. event id, monotonic 시간, cycle id, mapping epoch, 실패 단계와 errno.
2. zone별 tag, active 여부, 실제 capacity, allocation WP, dispatch WP,
   in-flight/pin 상태, invalid hint, snapshot 기준 unique live sectors.
3. FREE capacity, USER/GC/WAL/SSTABLE active tail, inactive tail,
   written-but-invalid, live data, metadata 및 header/padding 공간.
4. seal 전후 WP/invalid/free와 결정 이유; victim 사용량/live/WAL 비용,
   gain 및 workspace 판정; 이주 전후 실제 free 변화와 foreground 성공량.
5. WAL/checkpoint frontier, frozen 및 SSTable 상태. reclaim 지연 이유.
6. discard 요청/완료 범위·수량과 최신 매핑 tombstone 반영 여부.

공간 합계는 동일한 snapshot 기준으로 물리 capacity와 일치해야 한다.
invalid hint를 실제 invalid 섹터로 합산하지 않는다. mapping epoch만으로
모든 동시 상태의 원자성이 보장된다고 가정하지 않는다. 일관된 snapshot을
만들 수 없으면 그 사실/범위를 기록하고 정지된 작은 재현에서 확정한다.

## 검증 순서와 통과 기준

1. 기존 `python3 scripts/test-safety-unit.py` 회귀 테스트.
2. H1/H2 production 경로 재현 테스트 추가: **수정 전 실패 / 수정 후 성공**.
   기존 suite만 통과하는 것은 새 원인을 검증한 것이 아니다.
3. 작은 FEMU 실험에서 H3 oracle overwrite/readback + zone 재사용 검증.
   최소 여러 번의 전체 물리 용량 상당 누적 쓰기와 실제 GC/reset을 확인.
4. prefill 바이트를 고정한 hot-region overwrite로 공간 snapshot이 안정적으로
   순환하는지 확인. 낮은/높은 prefill을 한 변수만 바꿔 비교한다.
5. EXT4/F2FS 각각 짧은 benchmark 진단. 실패하면 최초 원인부터 분석하며
   같은 실패를 장시간 반복 실행하지 않는다.
6. 같은 수정 커밋으로 두 FS의 원래 4번 1시간 조건을 각각 실행하고,
   두 번 이상 독립 초기화 반복으로 재현성을 확인한다.

최종 성공에는 전체 warmup/measurement 완료, Valid=True, send/failed/unresolved
0, consumer delta/gap/duplicate/order/CRC 이상 0, mapping audit 이상 0,
I/O/journal/readonly 오류 0, 지속 ACK stall 없음이 필요하다. throughput과
tail latency, 최저 free, 시간별 공간 추이도 보고한다. `benchmark_exit=0`이나
CSV 저장 성공은 통과 증거가 아니다. 1시간 통과는 무기한/전원 장애 안전성의
증명이 아니다. 별도 crash 검증과 구분한다.

## 일반 NVMe 대조군

같은 FS, 논리 용량, 실제 prefill 바이트, mount 옵션, Kafka/JAR/consumer 설정을
맞춘다. NVMe와 FEMU의 속도 차이는 명시한다. steady occupancy, block writes,
discard, ACK 및 readback을 비교한다. 하드웨어의 숨겨진 FTL live map/OP를
측정할 수 있다고 가정하지 않는다. 대조군 통과만으로 benchmark 계측 전부가
정확하다고 단정하지 않는다.

## 실행 안전성과 산출물

- 모든 장치 쓰기/초기화 테스트는 확인된 **게스트의 폐기 가능한 실험 장치**에서만.
  자동으로 찾은 raw device나 호스트 NVMe를 대상으로 하는 명령은 제공하지 않는다.
- 실패 장치에 fsck/재포맷/재실행하기 전에 로그, 환경, broker rotation,
  JSON/raw 결과 및 필요한 메타데이터 snapshot을 보존한다.
- 기존 진단 폴더 RwyaQmNL, 2aefWNvi, NOfUB4Xr을 삭제하지 않는다.
- 결과표: 코드/모듈/JAR 식별자, FS/설정, 최초 실패 이벤트, 가설별 판정,
  local test, guest short, guest full, 반복 결과를 분리해 기록한다.
- 현재 상태: 검증 설계 작성. 새 계측/재현 테스트 미구현, 새 정책 수정 없음,
  최신 코드의 두 FS benchmark 4 최종 성공 미확인.
