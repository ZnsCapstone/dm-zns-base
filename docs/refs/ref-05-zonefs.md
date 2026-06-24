# zonefs

> 출처: https://docs.kernel.org/filesystems/zonefs.html

## 개요

zonefs는 **zoned block device의 각 zone을 하나의 파일로 노출**하는 단순한 파일시스템이다. 순차 쓰기 제약을 사용자에게 숨기지 않는다는 점에서, 본 과제(임의 쓰기를 흡수하는 DM 타깃)와 설계 철학이 정반대다.

> "zonefs는 POSIX 준수 파일시스템이라기보다 원시 블록 디바이스 접근 인터페이스에 더 가깝다."

## 마운트 후 디렉터리 구조

```
/mnt/zbd/
├── cnv/          ← conventional zone 파일들 (있는 경우)
│   ├── 0         → zone 0 (임의 읽/쓰기 가능)
│   └── 1         → zone 1
└── seq/          ← sequential zone 파일들
    ├── 0         → zone 2 (순차 append만)
    ├── 1         → zone 3
    └── ...
```

파일 이름은 같은 타입 내에서 0부터 순서대로 번호를 매긴다. 물리 zone 순서와 일치한다.

## Zone 타입별 동작

### Conventional zone 파일 (`cnv/`)

- 임의 읽기/쓰기 가능
- 메모리 매핑(`mmap`) 가능
- 일반 파일과 동일하게 동작

### Sequential zone 파일 (`seq/`)

- **append-only**: 파일 끝에서만 쓰기 가능
- **direct I/O만 허용**: 버퍼링된 쓰기 금지 (dirty 페이지 캐시가 임의 위치에 flush될 수 있으므로)
- 읽기는 모든 방식(buffered, direct) 가능
- `fallocate`로 크기 예약 가능

```bash
# sequential zone 파일에 쓰기 (direct I/O + O_APPEND)
dd if=/dev/zero of=/mnt/zbd/seq/0 bs=4096 count=1 oflag=direct,append
```

## 공통 제약

- 파일 생성/삭제/이름 변경 불가 (zone 개수 = 파일 개수, 고정)
- zone 용량 초과 쓰기 → `EFBIG` 오류
- zone 크기보다 큰 파일 없음

## 메타데이터

- 슈퍼블록: 섹터 0에 고정 (불변)
- 런타임 메타데이터: 파일 크기 = zone의 write pointer 위치와 동기화
- I/O 오류 후 파일 크기와 wp 불일치 → zonefs가 자동으로 파일 크기 수정

## 마운트 옵션 (오류 처리 정책)

| 옵션 | 동작 |
|---|---|
| `errors=remount-ro` | 오류 발생 시 읽기 전용으로 재마운트 |
| `errors=zone-ro` | 오류 zone 파일만 읽기 전용 |
| `errors=zone-offline` | 오류 zone 파일을 오프라인 |
| `errors=repair` | 오류 zone wp를 파일 크기와 동기화해서 복구 |

## 이 프로젝트와의 관계

zonefs는 ZNS SSD를 **직접 노출**한다. 파일 API를 통하지만 zone 제약이 그대로 보인다.

본 과제는 반대 방향: zone 제약을 **완전히 숨기고** 위쪽에 conventional 블록 디바이스처럼 보이게 한다. 따라서 ext4 같은 zone-unaware 파일시스템이 그 위에서 아무 수정 없이 동작한다.

**RocksDB 연관:** zonefs의 주요 사용 사례가 RocksDB다. RocksDB는 SST 파일을 append-only로 쓰기 때문에 zonefs의 sequential zone append 제약과 자연스럽게 맞는다. LSM-Tree 기반 스토리지가 ZNS와 잘 맞는 이유의 실제 예시.
