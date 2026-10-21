#!/usr/bin/env python3
"""Destructive FEMU smoke test for zns_dm/keper-kovalt-integration.

Without --run, print the plan and do not modify the device. Results must live
on the guest root disk. This is a single-boot functional smoke test, not a
repeatable performance result or a power-loss test.
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


RAW = "/dev/nvme0n1"
NAME = "zns-integration-smoke"
DEV = "/dev/mapper/" + NAME
MOUNT = "/mnt/zns-integration-smoke"
TABLE_SECTORS = 50331648  # 24 GiB
EXPECTED_GEOMETRY = {
    "size": "67108864",
    "queue/zoned": "host-managed",
    "queue/chunk_sectors": "1048576",
    "queue/nr_zones": "64",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=12,
                        choices=range(5, 61))
    args = parser.parse_args()

    plan = {
        "raw": RAW,
        "target": NAME,
        "destructive": True,
        "durability_modes": [0, 1],
        "filesystems": ["ext4", "f2fs"],
        "block_sizes": ["4k", "1m"],
        "cells": 8,
        "seconds": args.seconds,
    }
    if not args.run:
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return

    if socket.gethostname() != "zns-guest":
        raise RuntimeError("team4 guest hostname mismatch")
    args.module = args.module.resolve(strict=True)
    args.out = args.out.resolve()
    if not str(args.out).startswith("/home/zns/"):
        raise RuntimeError("--out must be on the guest root disk under /home/zns")
    args.out.mkdir(parents=True, exist_ok=False)
    results = []

    def save(name, value):
        (args.out / name).write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n")

    def run(cmd, timeout=120, name=None, check=True, input_text=None):
        with (args.out / "commands.jsonl").open("a") as log:
            log.write(json.dumps({"at": time.time(), "cmd": cmd}) + "\n")
        value = subprocess.run(cmd, input=input_text, text=True,
                               capture_output=True, timeout=timeout)
        if name:
            (args.out / name).write_text(value.stdout)
            (args.out / (name + ".stderr")).write_text(value.stderr)
        if check and value.returncode:
            raise RuntimeError(
                f"{cmd}: rc={value.returncode}\n{value.stderr[-2000:]}")
        return value

    def status(label):
        text = run(["sudo", "-n", "dmsetup", "status", NAME],
                   name=label + ".status").stdout
        return {key: int(value)
                for key, value in re.findall(r"(\w+)=(\d+)", text)}

    def validate_status(values):
        for key in ["wal_error", "data_write_error", "gc_error",
                    "gc_last_error"]:
            if values.get(key, 0):
                raise RuntimeError(f"{key}={values[key]}")

    def attach(strict, threshold):
        run([
            "sudo", "-n", "insmod", str(args.module),
            f"strict_wal_durability={strict}",
            "wal_batch_max_records=126",
            "metrics_enabled=1",
            "gc_diag_budget=2000",
            f"flush_threshold={threshold}",
            "compaction_k=4",
            "gc_reserved_zones=8",
            "gc_low_watermark=16",
            "gc_high_watermark=20",
        ])
        run(["sudo", "-n", "dmsetup", "create", NAME],
            input_text=f"0 {TABLE_SECTORS} zns-base {RAW}\n")
        actual = status("attached")
        expected = {
            "strict_wal_durability": strict,
            "wal_batch_max_records": 126,
            "metrics_enabled": 1,
            "gc_reserved_zones": 8,
            "gc_low_watermark": 16,
            "gc_high_watermark": 20,
            "memtable_capacity_entries": threshold,
            "sstable_compaction_threshold": 4,
        }
        for key, value in expected.items():
            if actual.get(key) != value:
                raise RuntimeError(
                    f"module setting mismatch: {key}={actual.get(key)}, expected={value}")
        validate_status(actual)

    def detach(mounted=False):
        if mounted:
            run(["sudo", "-n", "umount", MOUNT])
        run(["sudo", "-n", "udevadm", "settle", "--timeout=30"])
        run(["sudo", "-n", "dmsetup", "remove", "--retry", NAME],
            timeout=180)
        run(["sudo", "-n", "rmmod", "dm_zns_base"])

    def reset():
        run(["sudo", "-n", "blkzone", "reset", RAW], timeout=180)

    def fio(label, filename, options):
        value = run([
            "sudo", "-n", "fio", "--name=" + label,
            "--filename=" + filename, "--direct=1", "--ioengine=libaio",
            "--output-format=json", "--thread=1", "--group_reporting=1",
            *options,
        ], timeout=args.seconds + 180, name=label + ".fio.json")
        payload = json.loads(value.stdout)
        if len(payload.get("jobs", [])) != 1 or payload["jobs"][0].get("error"):
            raise RuntimeError("fio validation/execution failure: " + label)
        return payload["jobs"][0]

    start = float(Path("/proc/uptime").read_text().split()[0])
    try:
        run(["sudo", "-n", "true"])
        model = run(["lsblk", "-dn", "-o", "MODEL", RAW]).stdout
        if "FEMU" not in model:
            raise RuntimeError("raw device is not FEMU")
        geometry = {
            key: Path("/sys/block/nvme0n1", key).read_text().strip()
            for key in EXPECTED_GEOMETRY
        }
        if geometry != EXPECTED_GEOMETRY:
            raise RuntimeError("unexpected 32 GiB/512 MiB geometry: " + str(geometry))
        devices = run(["sudo", "-n", "dmsetup", "ls"]).stdout.strip()
        if devices != "No devices found":
            raise RuntimeError("existing DM device found; refusing automatic removal")
        mounts = run(["findmnt", "-rn", "-o", "SOURCE,TARGET"]).stdout
        if "/dev/nvme" in mounts or MOUNT in mounts:
            raise RuntimeError("experiment device is already mounted")
        for command in [["pgrep", "-x", "fio"], ["pgrep", "-x", "java"]]:
            if run(command, check=False).returncode == 0:
                raise RuntimeError("existing workload process found")

        kernel = run(["uname", "-r"]).stdout.strip()
        vermagic = run(["modinfo", "-F", "vermagic", str(args.module)]).stdout.split()[0]
        if vermagic != kernel:
            raise RuntimeError("guest kernel/module vermagic mismatch")
        source = args.module.with_name("dm-zns-base.c")
        source.resolve(strict=True)
        commit_file = args.module.parent.parent / "SOURCE-COMMIT"
        save("manifest.json", {
            **plan,
            "kernel": kernel,
            "geometry": geometry,
            "module_sha256": hashlib.sha256(args.module.read_bytes()).hexdigest(),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "source_commit": commit_file.read_text().strip()
            if commit_file.exists() else None,
            "scope": "functional short smoke; same boot; single repeat; no power-loss test",
        })
        run(["sudo", "-n", "dmesg"], name="kernel-before.log")
        if Path("/sys/module/dm_zns_base").exists():
            run(["sudo", "-n", "rmmod", "dm_zns_base"])

        # Writeback-mode data must survive an explicit flush and target reload.
        reset()
        attach(0, 32)
        fio("boundary-crc", DEV, [
            "--rw=write", "--bs=4k", "--iodepth=8", "--size=2m",
            "--verify=crc32c", "--do_verify=1", "--end_fsync=1",
        ])
        run(["sudo", "-n", "blockdev", "--flushbufs", DEV])
        boundary = status("boundary-after-flush")
        validate_status(boundary)
        if boundary.get("wal_staged_records"):
            raise RuntimeError("WAL records remain staged after explicit flush")
        detach()
        attach(0, 131072)
        fio("replay-crc", DEV, [
            "--rw=write", "--bs=4k", "--iodepth=8", "--size=2m",
            "--verify=crc32c", "--verify_only=1",
        ])
        validate_status(status("replay-after"))
        detach()
        save("boundary-result.json", boundary)

        run(["sudo", "-n", "mkdir", "-p", MOUNT])
        for strict in [0, 1]:
            for filesystem in ["ext4", "f2fs"]:
                reset()
                attach(strict, 131072)
                if filesystem == "ext4":
                    mkfs = ["sudo", "-n", "mkfs.ext4", "-F", "-E",
                            "nodiscard", DEV]
                else:
                    mkfs = ["sudo", "-n", "mkfs.f2fs", "-f", "-t", "0", DEV]
                run(mkfs, timeout=180,
                    name=f"strict{strict}-{filesystem}-mkfs.log")
                run(["sudo", "-n", "mount", "-t", filesystem,
                     "-o", "nodiscard", DEV, MOUNT])
                filename = MOUNT + "/fio-smoke.bin"
                fio(f"strict{strict}-{filesystem}-prepare", filename, [
                    "--rw=write", "--bs=1m", "--iodepth=32",
                    "--size=256m", "--end_fsync=1",
                ])
                for block_size in ["4k", "1m"]:
                    label = f"strict{strict}-{filesystem}-{block_size}"
                    before = status(label + "-before")
                    job = fio(label, filename, [
                        "--rw=write", "--bs=" + block_size,
                        "--iodepth=32", "--size=256m", "--time_based=1",
                        f"--runtime={args.seconds}", "--end_fsync=1",
                        "--percentile_list=95:99",
                    ])
                    after = status(label + "-after")
                    validate_status(after)
                    results.append({
                        "strict_wal_durability": strict,
                        "filesystem": filesystem,
                        "block_size": block_size,
                        "write": job["write"],
                        "status_before": before,
                        "status_after": after,
                    })
                    save("summary.json", results)
                detach(mounted=True)
        save("status.json", {"state": "completed", "cells": len(results)})
    except BaseException as exc:
        save("status.json", {
            "state": "failed", "cells": len(results), "error": str(exc),
            "cleanup": "not attempted; evidence and device state preserved",
        })
        raise
    finally:
        log = run(["sudo", "-n", "dmesg"], check=False,
                  name="kernel-after.log").stdout
        lines = [
            line for line in log.splitlines()
            if (match := re.match(r"\[\s*([\d.]+)\]", line))
            and float(match[1]) >= start
        ]
        (args.out / "kernel-window.log").write_text("\n".join(lines) + "\n")
        errors = [
            line for line in lines
            if re.search(r"I/O error|WAL.*failed|BUG:|Oops:|KASAN:|"
                         r"insufficient relocation workspace", line, re.I)
        ]
        save("kernel-errors.json", errors)


if __name__ == "__main__":
    main()
