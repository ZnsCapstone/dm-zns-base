# ZNS 실험 서버 사용 안내

졸업프로젝트 FEMU/ZNS 실험 서버 사용법입니다. 추가로 궁금한 점은 메일/카톡 주세요.

## 1. 접속

```bash
ssh <팀계정>@<서버주소>        # 초기 비밀번호는 개별 메일로 안내드리겠습니다.
```

| 팀 | 계정 | 게스트 SSH 포트 |
|---|---|---|
| 1조 | zns-team1 | 2201 |
| 2조 | zns-team2 | 2202 |
| 4조 | zns-team4 | 2204 |
| 64조 | zns-team64 | 2264 |

호스트 계정에는 sudo가 없습니다. **root가 필요한 작업(insmod, dmsetup, mkfs,
mount)은 전부 게스트 VM 안에서** 하세요 .

## 2. 비밀번호 변경 — 두 곳, 처음에 한 번씩

**① 호스트 팀 계정** (첫 로그인 시 자동으로 요구됩니다):

```bash
passwd
```

**② 게스트 zns 계정** (게스트 첫 접속 후 직접):

```bash
# 게스트 안에서
passwd        # 현재 비밀번호: zns
```

②를 하는 이유: 게스트 SSH 포트는 서버 내부(localhost)에 열려 있어서, 초기
비밀번호(`zns`)를 그대로 두면 같은 서버를 쓰는 다른 팀이 여러분의 게스트에
들어올 수 있습니다.

## 3. 기본 사용 흐름

```bash
tmux new -s zns        # tmux 사용 권장 (SSH가 끊기면 VM도 죽습니다)
./run-zns.sh           # FEMU 게스트 시작 (sudo 불필요)
```

다른 터미널에서 게스트 접속:

```bash
ssh -p <팀포트> zns@localhost
```

게스트 안에서:

```bash
check-zns                        # ZNS 장치 확인 (zoned=host-managed 여야 정상)
sudo blkzone report /dev/nvme0n1 | head   # zone 레이아웃
sudo poweroff                    # 종료 — 강제 종료(Ctrl-a x)
```

- `~/results/femu-runs.log` 에 매 실행의 FEMU 종류·커밋·장치 인자가 자동
  기록됩니다.
- tmux 세션에서 나올 때는 `Ctrl-b d` (detach — VM은 계속 돕니다),
  다시 붙을 때는 `tmux attach -t zns`.

## 4. FEMU 수정 → 재빌드 → 실행

FEMU 소스는 `~/femu/src` 에 있고 **마음대로 고쳐도 됩니다.** zns geometry/latency 등은 소스에 박혀 있어 수정하려면 재빌드가 필요합니다.

주요 위치 (전부 `~/femu/src/` 기준):

```
hw/femu/zns/zns.c        ZNS 모드 핵심 로직 (zone 상태, 명령 처리)
hw/femu/zns/zftl.c       zone 기반 FTL
hw/femu/timing-model/    지연 모델
hw/femu/femu.c           장치 프로퍼티 정의 (zns_num_ch 등)
```

**재빌드:**

```bash
./build-own-femu.sh
```

**실행:** 그냥 `./run-zns.sh` — 런처가 `~/femu` 빌드를 자동으로 우선 사용합니다.
시작 배너에 `FEMU : 팀 자체 빌드` 로 표시되는지 확인하세요.

**주의:** 새 바이너리는 **VM을 껐다 켜야** 반영됩니다. 게스트 재부팅이 아니라,
게스트 `sudo poweroff` → 호스트에서 `./run-zns.sh` 재실행입니다.
(FEMU는 게스트 안이 아니라 호스트에서 도는 프로그램이기 때문입니다.)

**수정 이력:** 소스를 고쳤으면 `~/femu/src` 에서 git commit으로 남기고,
월간보고서에 어떤 커밋에서 무엇을 왜 바꿨는지 기록하세요. 수정 전/후 수치를
비교할 때 근거가 됩니다.

## 5. ZNS 용량·지오메트리 조절

**용량** — 기본 32GB, 64GB까지는 조교 승인 없이:

```bash
ZNS_GB=64 ./run-zns.sh
```

(에뮬레이트된 SSD는 호스트 RAM에 있습니다. VM을 끄면 ZNS 내용은 사라지고,
게스트 OS 디스크만 남습니다. 모듈 재적재 수준에서는 VM이 살아 있으니 데이터가
유지됩니다.)

**zone 크기·개수** — `~/run-zns.sh` 의 `ZNS_NUM_*` 변수를 직접 수정하세요.
이 파일은 여러분 소유입니다. 현재 기본값(8채널)으로 zone 2GiB × 16개인데,
GC 실험에 zone 수가 더 필요하면 조정하고 `blkzone report` 로 확인하세요.

## 6. 문제가 생겼을 때

| 증상 | 조치 |
|---|---|
| `/dev/kvm 접근 불가` | 재접속 (그룹 적용은 새 세션부터) |
| `포트가 이미 사용 중` | `pgrep -af qemu-system` 으로 떠 있는 VM 확인, `tmux attach -t zns` |
| 게스트 SSH 호스트키 경고 | 이미지가 초기화된 경우 정상: 경고문의 `ssh-keygen -R` 명령 실행 |
| check-zns 에서 `zoned=none` | `run-zns.sh` 의 `femu_mode`/지오메트리 인자 확인, 조교 문의 |
| 게스트가 아예 안 뜸/이미지 깨짐 | 조교에게 이미지 초기화 요청 (몇 초면 됩니다. 단, 게스트 안 데이터는 사라지므로 코드는 항상 git에 push해 두세요) |
| FEMU 빌드 실패 | 오류 전문을 그대로 조교에게 |

**게스트 안의 코드는 반드시 원격 저장소에 push해 두세요.** 게스트 이미지는
초기화될 수도 있으니 자주 커밋 및 푸쉬하는 것을 권장합니다.
