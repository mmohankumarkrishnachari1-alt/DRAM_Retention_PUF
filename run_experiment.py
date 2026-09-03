#!/usr/bin/env python3
"""Configurable DRAM-Bender U200 retention-PUF raw-data runner."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

CHALLENGE_ORDER = ("00", "FF", "AA", "55")

class ConfigError(ValueError):
    pass

def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")

def sanitize(value: Any) -> str:
    text = re.sub(r"[^A-Za-z0-9._-]+", "_", str(value).strip())
    return text.strip("_") or "UNNAMED"

def path_float(value: float) -> str:
    text = f"{value:.6f}".rstrip("0").rstrip(".")
    return text.replace("-", "m").replace(".", "p")

def load_json(path: Path) -> Dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ConfigError(f"Could not read JSON {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ConfigError("Top-level config must be a JSON object")
    return data

def req_obj(parent: Dict[str, Any], key: str) -> Dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict): raise ConfigError(f"'{key}' must be an object")
    return value

def req_list(parent: Dict[str, Any], key: str) -> List[Any]:
    value = parent.get(key)
    if not isinstance(value, list): raise ConfigError(f"'{key}' must be an array")
    return value

def req_str(parent: Dict[str, Any], key: str) -> str:
    value = parent.get(key)
    if not isinstance(value, str) or not value.strip(): raise ConfigError(f"'{key}' must be non-empty text")
    return value

def req_int(parent: Dict[str, Any], key: str) -> int:
    value = parent.get(key)
    if isinstance(value, bool) or not isinstance(value, int): raise ConfigError(f"'{key}' must be an integer")
    return value

def req_num(parent: Dict[str, Any], key: str) -> float:
    value = parent.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)): raise ConfigError(f"'{key}' must be numeric")
    return float(value)

def get_int(parent: Dict[str, Any], key: str, default: int) -> int:
    value = parent.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int): raise ConfigError(f"'{key}' must be an integer")
    return value

def get_num(parent: Dict[str, Any], key: str, default: float) -> float:
    value = parent.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)): raise ConfigError(f"'{key}' must be numeric")
    return float(value)

def get_bool(parent: Dict[str, Any], key: str, default: bool) -> bool:
    value = parent.get(key, default)
    if not isinstance(value, bool): raise ConfigError(f"'{key}' must be true or false")
    return value

def retention_points(retention: Dict[str, Any]) -> List[Tuple[str, float]]:
    points: List[Tuple[str, float]] = []
    if get_bool(retention, "include_zero_ms_control", True):
        points.append(("CONTROL", 0.0))

    sweep_type = retention.get("sweep_type", "explicit")
    if sweep_type == "explicit":
        times = retention.get("times_ms", [])
        if not isinstance(times, list): raise ConfigError("retention.times_ms must be an array")
        for i, value in enumerate(times):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or float(value) <= 0:
                raise ConfigError(f"retention.times_ms[{i}] must be > 0")
            points.append(("RETENTION_SWEEP", float(value)))
    elif sweep_type == "range":
        start = req_num(retention, "start_ms")
        stop = req_num(retention, "maximum_ms")
        step = req_num(retention, "step_ms")
        if start <= 0 or stop < start or step <= 0:
            raise ConfigError("range sweep requires 0 < start_ms <= maximum_ms and step_ms > 0")
        value = start
        values: List[float] = []
        while value <= stop + 1e-12:
            values.append(value)
            value += step
        if abs(values[-1] - stop) > 1e-9:
            values.append(stop)
        points.extend(("RETENTION_SWEEP", float(v)) for v in values)
    else:
        raise ConfigError("retention.sweep_type must be 'explicit' or 'range'")
    if not points:
        raise ConfigError("No retention points configured")
    return points

def validate(config: Dict[str, Any], config_path: Path) -> Dict[str, Any]:
    run = req_obj(config, "run")
    board = req_obj(config, "board")
    dimm = req_obj(config, "dimm")
    env = req_obj(config, "environment")
    timing = req_obj(config, "timing")
    retention = req_obj(config, "retention")
    geometry = req_obj(config, "geometry")
    experiment = req_obj(config, "experiment")
    regions = req_list(config, "regions")

    req_str(run, "run_id"); req_str(dimm, "dimm_id")
    req_num(env, "temperature_c"); req_num(env, "voltage_vdd_v")
    if get_int(experiment, "repetitions", 10) <= 0: raise ConfigError("repetitions must be positive")
    if experiment.get("patterns", list(CHALLENGE_ORDER)) != list(CHALLENGE_ORDER):
        raise ConfigError('experiment.patterns must be exactly ["00", "FF", "AA", "55"]')
    if get_int(geometry, "cache_line_bytes", 64) != 64:
        raise ConfigError("geometry.cache_line_bytes must be 64 for this U200 collector")
    if get_int(geometry, "cache_lines_per_row", 128) <= 0 or get_int(geometry, "column_stride", 8) <= 0:
        raise ConfigError("invalid geometry")
    if req_int(timing, "nominal_trcd_slots") < 0 or req_int(timing, "nominal_trp_slots") < 0:
        raise ConfigError("nominal timing slots cannot be negative")
    for key, default in (("write_spacing_slots",7),("read_spacing_slots",7),("write_recovery_slots",8),
                         ("read_to_precharge_slots",8),("final_guard_slots",16)):
        if get_int(timing, key, default) < 0: raise ConfigError(f"timing.{key} cannot be negative")
    if get_num(retention, "fabric_cycle_ns", 6.0) <= 0: raise ConfigError("fabric_cycle_ns must be positive")
    _ = retention_points(retention)

    banks_per_group = get_int(dimm, "banks_per_group", 4)
    if banks_per_group <= 0: raise ConfigError("dimm.banks_per_group must be positive")
    if not regions: raise ConfigError("At least one region is required")
    seen = set(); normalized_regions = []
    for i, r in enumerate(regions):
        if not isinstance(r, dict): raise ConfigError(f"regions[{i}] must be an object")
        region_id = req_str(r, "region_id")
        if region_id in seen: raise ConfigError(f"duplicate region_id: {region_id}")
        seen.add(region_id)
        rank = req_int(r, "rank"); bg = req_int(r, "bank_group"); bank = req_int(r, "bank")
        start = req_int(r, "row_start"); end = req_int(r, "row_end")
        if min(rank,bg,bank,start) < 0 or end < start: raise ConfigError(f"invalid coordinates in {region_id}")
        flat = bg * banks_per_group + bank
        if flat > 15: raise ConfigError(f"{region_id}: flat bank {flat} is outside 0..15")
        normalized_regions.append(dict(region_id=region_id, rank=rank, bank_group=bg, bank=bank,
                                       row_start=start, row_end=end))

    collector = Path(config.get("collector_binary", "./retention_puf_collector"))
    output = Path(config.get("output_root", "./dram_retention_puf_data"))
    if not collector.is_absolute(): collector = (config_path.parent / collector).resolve()
    if not output.is_absolute(): output = (config_path.parent / output).resolve()
    result = dict(config)
    result["collector_binary"] = str(collector); result["output_root"] = str(output)
    result["regions"] = normalized_regions
    return result

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024*1024), b""): h.update(chunk)
    return h.hexdigest()

def atomic_json(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)

def append_log(path: Path, message: str) -> None:
    line = f"[{utc_now()}] {message}"
    print(line, flush=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f: f.write(line + "\n")

def run_root(config: Dict[str, Any]) -> Path:
    dimm = req_obj(config, "dimm"); run = req_obj(config, "run"); env = req_obj(config, "environment")
    return (Path(req_str(config, "output_root")) /
            f"DIMM_{sanitize(req_str(dimm,'dimm_id'))}" /
            f"RUN_{sanitize(req_str(run,'run_id'))}__TEMP_{path_float(req_num(env,'temperature_c'))}C__VDD_{path_float(req_num(env,'voltage_vdd_v'))}V")

def region_name(r: Dict[str, Any]) -> str:
    return (f"REGION_{sanitize(r['region_id'])}__R{r['rank']}__BG{r['bank_group']}__B{r['bank']}"
            f"__ROWS_{r['row_start']:06d}-{r['row_end']:06d}")

def retention_name(ms: float) -> str:
    return f"RETENTION_{path_float(ms)}ms"

def complete_point(point_dir: Path, reps: int) -> bool:
    return all((point_dir / f"REP_{i:02d}" / ".complete").exists() for i in range(reps))

def archive_incomplete(point_dir: Path, reps: int) -> None:
    if not point_dir.exists(): return
    stamp = dt.datetime.now().strftime("%Y%m%dT%H%M%S")
    partial = point_dir / "partial_attempts"
    for i in range(reps):
        rep = point_dir / f"REP_{i:02d}"
        if rep.exists() and not (rep / ".complete").exists():
            partial.mkdir(parents=True, exist_ok=True)
            dest = partial / f"REP_{i:02d}__{stamp}"
            n = 1
            while dest.exists(): dest = partial / f"REP_{i:02d}__{stamp}_{n}"; n += 1
            shutil.move(str(rep), str(dest))

def collector_cmd(config: Dict[str, Any], region: Dict[str, Any], pattern: str,
                  mode: str, retention_ms: float, point_dir: Path) -> List[str]:
    run = req_obj(config, "run"); board = req_obj(config,"board"); dimm=req_obj(config,"dimm")
    env=req_obj(config,"environment"); timing=req_obj(config,"timing"); ret=req_obj(config,"retention")
    geom=req_obj(config,"geometry"); exp=req_obj(config,"experiment")
    args = [
        req_str(config,"collector_binary"), "--output-dir", str(point_dir),
        "--dimm-id", req_str(dimm,"dimm_id"), "--run-id", req_str(run,"run_id"),
        "--region-id", str(region["region_id"]), "--challenge-id", f"CHALLENGE_{pattern}",
        "--mode", mode, "--pattern", str(int(pattern,16)), "--rank", str(region["rank"]),
        "--bank-group", str(region["bank_group"]), "--bank", str(region["bank"]),
        "--row-start", str(region["row_start"]), "--row-end", str(region["row_end"]),
        "--retention-time-ms", str(retention_ms),
        "--nominal-trcd-slots", str(req_int(timing,"nominal_trcd_slots")),
        "--nominal-trp-slots", str(req_int(timing,"nominal_trp_slots")),
        "--temperature-c", str(req_num(env,"temperature_c")), "--voltage-vdd-v", str(req_num(env,"voltage_vdd_v")),
        "--banks-per-group", str(get_int(dimm,"banks_per_group",4)),
        "--cache-lines-per-row", str(get_int(geom,"cache_lines_per_row",128)),
        "--cache-line-bytes", str(get_int(geom,"cache_line_bytes",64)),
        "--column-stride", str(get_int(geom,"column_stride",8)),
        "--repetitions", str(get_int(exp,"repetitions",10)),
        "--write-spacing-slots", str(get_int(timing,"write_spacing_slots",7)),
        "--read-spacing-slots", str(get_int(timing,"read_spacing_slots",7)),
        "--write-recovery-slots", str(get_int(timing,"write_recovery_slots",8)),
        "--read-to-precharge-slots", str(get_int(timing,"read_to_precharge_slots",8)),
        "--final-guard-slots", str(get_int(timing,"final_guard_slots",16)),
        "--fabric-cycle-ns", str(get_num(ret,"fabric_cycle_ns",6.0)),
        "--slot-ns", str(get_num(timing,"slot_ns_metadata",1.5)),
        "--write-observations-csv", str(get_bool(exp,"write_observations_csv",False)).lower(),
        "--write-raw-records-csv", str(get_bool(exp,"write_raw_records_csv",True)).lower(),
        "--temperature-source", str(env.get("temperature_source","manual_room_temperature")),
        "--voltage-source", str(env.get("voltage_source","manual")),
        "--operator", str(run.get("operator","")), "--institution", str(run.get("institution","")),
        "--board-id", str(board.get("board_id","ALVEO_U200_01")),
        "--bitstream-file", str(board.get("bitstream_file","unknown")),
    ]
    return args

def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    ns=ap.parse_args()
    config_path=Path(ns.config).resolve()
    try: config=validate(load_json(config_path), config_path)
    except ConfigError as exc:
        print(f"CONFIG ERROR: {exc}", file=sys.stderr); return 2

    root=run_root(config); root.mkdir(parents=True, exist_ok=True)
    log=root/"execution.log"
    used=dict(config); used["generated_at_utc"]=utc_now(); atomic_json(root/"config_used.json", used)
    collector=Path(req_str(config,"collector_binary"))
    manifest={"created_at_utc":utc_now(),"experiment_type":"DRAM retention PUF raw-data collection",
              "collector_binary":str(collector),"collector_sha256":sha256_file(collector) if collector.exists() else None,
              "retention_points":[{"mode":m,"retention_time_ms":v} for m,v in retention_points(req_obj(config,"retention"))],
              "patterns":list(CHALLENGE_ORDER),"regions":config["regions"]}
    atomic_json(root/"manifest.json", manifest)
    if not ns.dry_run and not collector.exists():
        print(f"ERROR: collector not found: {collector}. Run make first.", file=sys.stderr); return 2

    exp=req_obj(config,"experiment"); reps=get_int(exp,"repetitions",10)
    timeout=get_int(exp,"timeout_seconds_per_retention_point",86400)
    retries=get_int(exp,"max_retries",0)
    total=0
    for _r in config["regions"]:
        total += len(CHALLENGE_ORDER)*len(retention_points(req_obj(config,"retention")))
    append_log(log, f"Planned retention points: {total}; repetitions per point: {reps}")

    for region in config["regions"]:
        rdir=root/region_name(region)
        for pattern in CHALLENGE_ORDER:
            cdir=rdir/f"CHALLENGE_{pattern}"
            for mode, ms in retention_points(req_obj(config,"retention")):
                pdir=cdir/f"MODE_{mode}"/retention_name(ms)
                pdir.mkdir(parents=True, exist_ok=True)
                atomic_json(pdir/"point_manifest.json", {"dimm_id":req_str(req_obj(config,"dimm"),"dimm_id"),
                    "region":region,"challenge_id":f"CHALLENGE_{pattern}","pattern":pattern,"mode":mode,
                    "retention_time_ms":ms,"temperature_c":req_num(req_obj(config,"environment"),"temperature_c"),
                    "voltage_vdd_v":req_num(req_obj(config,"environment"),"voltage_vdd_v")})
                if ns.dry_run:
                    append_log(log, f"DRY RUN {region['region_id']} CHALLENGE_{pattern} {mode} {ms} ms")
                    continue
                if complete_point(pdir,reps) and not ns.force:
                    append_log(log, f"SKIP complete {region['region_id']} CHALLENGE_{pattern} {mode} {ms} ms")
                    continue
                if ns.force and pdir.exists():
                    stamp=dt.datetime.now().strftime("%Y%m%dT%H%M%S")
                    for i in range(reps):
                        rep=pdir/f"REP_{i:02d}"
                        if rep.exists():
                            archive=pdir/"forced_reruns"; archive.mkdir(exist_ok=True)
                            shutil.move(str(rep), str(archive/f"REP_{i:02d}__{stamp}"))
                else:
                    archive_incomplete(pdir,reps)
                cmd=collector_cmd(config,region,pattern,mode,ms,pdir)
                success=False
                for attempt in range(retries+1):
                    append_log(log, f"RUN {region['region_id']} CHALLENGE_{pattern} {mode} {ms} ms attempt {attempt+1}/{retries+1}")
                    with (pdir/f"collector_attempt_{attempt+1:02d}.log").open("w",encoding="utf-8") as fh:
                        try:
                            result=subprocess.run(cmd,stdout=fh,stderr=subprocess.STDOUT,timeout=timeout,check=False)
                            code=result.returncode
                        except subprocess.TimeoutExpired:
                            code=124
                    atomic_json(pdir/"point_status.json", {"updated_at_utc":utc_now(),"return_code":code,
                                "attempt":attempt+1,"complete":complete_point(pdir,reps)})
                    if code==0 and complete_point(pdir,reps): success=True; break
                    archive_incomplete(pdir,reps)
                if not success:
                    append_log(log, f"FAILED {region['region_id']} CHALLENGE_{pattern} {mode} {ms} ms; continuing")
    append_log(log, "Dry-run complete." if ns.dry_run else "Experiment runner finished.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
