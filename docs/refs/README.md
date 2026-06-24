# 레퍼런스 요약 모음

`docs/08-references.md`의 각 링크를 탐색해 요약한 파일들.

| 파일 | 원본 | 분류 |
|---|---|---|
| [ref-01-device-mapper-guide.md](ref-01-device-mapper-guide.md) | docs.kernel.org — DM admin guide | Device Mapper |
| [ref-02-dm-zoned.md](ref-02-dm-zoned.md) | docs.kernel.org — dm-zoned | Device Mapper |
| [ref-03-linux-zbd-api.md](ref-03-linux-zbd-api.md) | zonedstorage.io — Linux ZBD API | ZNS / Zoned |
| [ref-04-null-blk.md](ref-04-null-blk.md) | docs.kernel.org — null_blk | 실험 환경 |
| [ref-05-zonefs.md](ref-05-zonefs.md) | docs.kernel.org — zonefs | ZNS / Zoned |
| [ref-06-dm-target-tutorial.md](ref-06-dm-target-tutorial.md) | medium.com — DM 타깃 작성 튜토리얼 | Device Mapper |
| [ref-07-dm-concept.md](ref-07-dm-concept.md) | linuxvox.com — DM 개념 정리 | Device Mapper |
| [ref-08-dm-zap.md](ref-08-dm-zap.md) | github.com/westerndigitalcorporation/dm-zap | Prior Art |
| [ref-09-femu.md](ref-09-femu.md) | github.com/MoatLab/FEMU | 실험 환경 |
| [ref-10-lsm-survey.md](ref-10-lsm-survey.md) | VLDB Journal 2019 — LSM-Tree 서베이 | LSM-Tree |

## 빠른 탐색

**DM 타깃 개발을 이해하려면**: ref-07 → ref-01 → ref-06 순서  
**ZNS/zone 동작을 이해하려면**: ref-03 → ref-04 → ref-05 순서  
**Prior art 비교**: ref-02 (dm-zoned) → ref-08 (dm-zap)  
**M1 설계 참고**: ref-10 (LSM-Tree) → ref-08 (dm-zap GC 패턴)  
**서버 실험 환경**: ref-09 (FEMU)
