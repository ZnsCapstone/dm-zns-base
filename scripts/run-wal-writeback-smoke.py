#!/usr/bin/env python3
"""team4 FEMU 전용 파괴적 smoke. --run 없이는 장치를 변경하지 않는다.

결과는 root disk에 저장. 실패 시 후속 초기화를 하지 않고 증거/장치를 남긴다.
독립 FEMU 반복/전원 장애 시험이 아니므로 최종 성능 비교로 사용하지 않는다.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

RAW = '/dev/nvme0n1'
NAME = 'wal-writeback-smoke'
DEV = '/dev/mapper/' + NAME
MOUNT = '/mnt/wal-writeback-smoke-20261021'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=12, choices=range(5, 61))
    args = parser.parse_args()
    if not args.run:
        print(json.dumps(dict(raw=RAW, target=NAME, destructive=True,
                              filesystems=['ext4', 'f2fs'], cells=6,
                              fresh_format_per_cell=True,
                              seconds=args.seconds), ensure_ascii=False))
        return
    if socket.gethostname() != 'zns-guest':
        raise RuntimeError('team4 guest hostname 불일치')
    args.module = args.module.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=False)
    results = []

    def save(name, value):
        (args.out / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')

    def run(cmd, timeout=120, name=None, check=True, input=None):
        with (args.out / 'commands.jsonl').open('a') as log:
            log.write(json.dumps(dict(at=time.time(), cmd=cmd)) + '\n')
        value = subprocess.run(cmd, input=input, text=True, capture_output=True, timeout=timeout)
        if name:
            (args.out / name).write_text(value.stdout)
            (args.out / (name + '.stderr')).write_text(value.stderr)
        if check and value.returncode:
            raise RuntimeError(f'{cmd}: rc={value.returncode}\n{value.stderr[-2000:]}')
        return value

    def status(label):
        text = run(['sudo', '-n', 'dmsetup', 'status', NAME], name=label + '.status').stdout
        return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', text)}

    def attach(threshold):
        # 정책/크기 옵션을 지정하지 않아 실제 모듈 기본 동작을 검증한다.
        run(['sudo', '-n', 'insmod', str(args.module),
             'metrics_enabled=1', 'wal_timing_enabled=1',
             f'flush_threshold={threshold}', 'gc_reserved_zones=8',
             'gc_low_watermark=16', 'gc_high_watermark=20'])
        run(['sudo', '-n', 'dmsetup', 'create', NAME],
            input=f'0 50331648 zns-base {RAW}\n')
        actual = status('attached')
        for key, expected in dict(wal_writeback=1, append_max_blocks=64,
                                  wal_group_pages=8, adaptive_gc=1, probe_cache=1,
                                  wal_delay_ms=0).items():
            if actual.get(key) != expected:
                raise RuntimeError('모듈 기본값 불일치: ' + key)
        device = os.stat(DEV).st_rdev
        cache = Path(f'/sys/dev/block/{os.major(device)}:{os.minor(device)}/queue/write_cache').read_text().strip()
        if cache != 'write back':
            raise RuntimeError('DM volatile cache/barrier capability 누락')
        with (args.out / 'queue-cache.jsonl').open('a') as log:
            log.write(json.dumps(dict(write_cache=cache)) + '\n')

    def detach(mounted=False):
        if mounted:
            run(['sudo', '-n', 'umount', MOUNT])
        run(['sudo', '-n', 'udevadm', 'settle', '--timeout=30'])
        run(['sudo', '-n', 'dmsetup', 'remove', '--retry', NAME], timeout=180)
        run(['sudo', '-n', 'rmmod', 'dm_zns_base'])

    def reset():
        # preflight에서 고정 장치의 모델/전체 용량/zone geometry를 확인했다.
        run(['sudo', '-n', 'blkzone', 'reset', RAW], timeout=180)

    def fio(label, filename, options):
        value = run(['sudo', '-n', 'fio', '--name=' + label, '--filename=' + filename,
                     '--direct=1', '--ioengine=libaio', '--output-format=json',
                     '--thread=1', '--group_reporting=1', *options],
                    timeout=args.seconds + 180, name=label + '.fio.json')
        payload = json.loads(value.stdout)
        if len(payload.get('jobs', [])) != 1 or payload['jobs'][0].get('error'):
            raise RuntimeError('fio 검증/실행 오류: ' + label)
        return payload['jobs'][0]

    start = float(Path('/proc/uptime').read_text().split()[0])
    try:
        run(['sudo', '-n', 'true'])
        model = run(['lsblk', '-dn', '-o', 'MODEL', RAW]).stdout
        if 'FEMU' not in model:
            raise RuntimeError('FEMU raw device 아님')
        geometry = {k: Path('/sys/block/nvme0n1/' + k).read_text().strip()
                    for k in ['size', 'queue/zoned', 'queue/chunk_sectors', 'queue/nr_zones']}
        if geometry != {'size': '67108864', 'queue/zoned': 'host-managed',
                        'queue/chunk_sectors': '1048576', 'queue/nr_zones': '64'}:
            raise RuntimeError('허용된 32GiB/512MiB zone geometry 불일치: ' + str(geometry))
        devices = run(['sudo', '-n', 'dmsetup', 'ls']).stdout.strip()
        if devices != 'No devices found':
            raise RuntimeError('기존 DM이 존재함: 자동 제거 금지')
        mounts = run(['findmnt', '-rn', '-o', 'SOURCE,TARGET']).stdout
        if '/dev/nvme' in mounts or MOUNT in mounts:
            raise RuntimeError('실험 장치에 기존 mount가 존재함')
        for cmd in [['pgrep', '-x', 'fio'], ['pgrep', '-x', 'java']]:
            if run(cmd, check=False).returncode == 0:
                raise RuntimeError('기존 부하기 존재')
        kernel = run(['uname', '-r']).stdout.strip()
        if run(['modinfo', '-F', 'vermagic', str(args.module)]).stdout.split()[0] != kernel:
            raise RuntimeError('guest kernel/module 불일치')
        save('manifest.json', dict(kernel=kernel, geometry=geometry, seconds=args.seconds,
             module_sha256=hashlib.sha256(args.module.read_bytes()).hexdigest(),
             source_sha256=hashlib.sha256(args.module.with_name('dm-zns-base-main.c').read_bytes()).hexdigest(),
             mode='production-defaults',
             scope='기능/짧은 smoke; 동일 boot, 단일 반복, crash recovery 미검증'))
        run(['sudo', '-n', 'dmesg'], name='kernel-before.log')
        if Path('/sys/module/dm_zns_base').exists():
            run(['sudo', '-n', 'rmmod', 'dm_zns_base'])
        # 작은 threshold로 실제 SSTable/checkpoint/compaction 및 remap replay 검증.
        reset()
        attach(32)
        fio('boundary-crc', DEV, ['--rw=write', '--bs=4k', '--iodepth=8', '--size=2m',
                                  '--verify=crc32c', '--do_verify=1', '--end_fsync=1'])
        run(['sudo', '-n', 'blockdev', '--flushbufs', DEV])
        boundary = status('boundary-after-flush')
        if not boundary.get('wal_early_acks') or boundary['wal_buffered'] or boundary['wal_early_errors']:
            raise RuntimeError('실제 조기 완료/영속화 경계 계측 불일치')
        detach()
        attach(131072)  # raw reset 없이 WAL/SSTable 복구로 읽는다.
        fio('replay-crc', DEV, ['--rw=write', '--bs=4k', '--iodepth=8', '--size=2m',
                               '--verify=crc32c', '--verify_only=1'])
        # 명시적 fsync 없이 남은 partial 그룹도 정상 target 제거 때 drain.
        fio('teardown-prepare', DEV, ['--rw=write', '--bs=4k', '--iodepth=8',
                                     '--offset=4m', '--size=1m', '--verify=crc32c',
                                     '--do_verify=0', '--end_fsync=0'])
        run(['sudo', '-n', 'udevadm', 'settle', '--timeout=30'])
        fio('teardown-tail', DEV, ['--rw=write', '--bs=4k', '--iodepth=1',
                                  '--offset=6m', '--size=4k', '--verify=crc32c',
                                  '--do_verify=0', '--end_fsync=0'])
        save('teardown-before.json', status('teardown-before'))
        detach()
        attach(131072)
        fio('teardown-replay', DEV, ['--rw=write', '--bs=4k', '--iodepth=8',
                                    '--offset=4m', '--size=1m', '--verify=crc32c',
                                    '--verify_only=1'])
        fio('teardown-tail-replay', DEV, ['--rw=write', '--bs=4k', '--iodepth=1',
                                         '--offset=6m', '--size=4k', '--verify=crc32c',
                                         '--verify_only=1'])
        before = status('extent-crc-before')
        fio('extent-crc', DEV, ['--rw=write', '--bs=1m', '--iodepth=8',
                               '--offset=8m', '--size=8m', '--verify=crc32c',
                               '--do_verify=1', '--end_fsync=1'])
        after = status('extent-crc-after')
        if after['wal_diag_data_count'] - before['wal_diag_data_count'] >= 2048:
            raise RuntimeError('실제 extent append가 발동하지 않음')
        detach()
        save('boundary-result.json', boundary)
        run(['sudo', '-n', 'mkdir', '-p', MOUNT])
        jobs = [('write', '4k'), ('randwrite', '4k'), ('write', '1m')]
        for fs in ['ext4', 'f2fs']:
            for rw, bs in jobs:
                label = f'{fs}-{rw}-{bs}'
                reset()
                attach(131072)
                command = ['sudo', '-n', 'mkfs.ext4', '-F', '-E', 'nodiscard', DEV] if fs == 'ext4' else [
                    'sudo', '-n', 'mkfs.f2fs', '-f', '-t', '0', DEV]
                run(command, timeout=180, name=label + '-mkfs.log')
                run(['sudo', '-n', 'mount', '-t', fs, '-o', 'nodiscard', DEV, MOUNT])
                filename = MOUNT + '/fio-smoke.bin'
                fio(label + '-prepare', filename, ['--rw=write', '--bs=1m', '--iodepth=32',
                                                '--size=256m', '--end_fsync=1'])
                before = status(label + '-before')
                job = fio(label, filename, ['--rw=' + rw, '--bs=' + bs, '--iodepth=32',
                          '--size=256m', '--time_based=1', f'--runtime={args.seconds}',
                          '--end_fsync=1', '--percentile_list=95:99'])
                after = status(label + '-after')
                if after.get('wal_early_errors') or after.get('wal_buffered'):
                    raise RuntimeError('buffered WAL 오류')
                results.append(dict(filesystem=fs, rw=rw, bs=bs,
                    write=job['write'], status_before=before, status_after=after))
                save('summary.json', results)
                detach(mounted=True)
        save('status.json', dict(state='completed', cells=len(results)))
    except BaseException as exc:
        save('status.json', dict(state='failed', cells=len(results), error=str(exc)))
        raise
    finally:
        log = run(['sudo', '-n', 'dmesg'], check=False, name='kernel-after.log').stdout
        lines = [line for line in log.splitlines() if (match := re.match(r'\[\s*([\d.]+)\]', line))
                 and float(match[1]) >= start]
        (args.out / 'kernel-window.log').write_text('\n'.join(lines) + '\n')
        errors = [line for line in lines if re.search(
            r'I/O error|foreground WAL append failed|flush chain failed|checkpoint write failed|'
            r'BUG:|Oops:|KASAN:|insufficient relocation workspace', line, re.I)]
        save('kernel-errors.json', errors)
        if errors:
            save('status.json', dict(state='failed', cells=len(results), error='kernel 오류: 원본 보존'))
            raise RuntimeError('kernel 오류: 원본 보존')


if __name__ == '__main__':
    main()
