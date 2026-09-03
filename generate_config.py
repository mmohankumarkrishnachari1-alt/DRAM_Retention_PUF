#!/usr/bin/env python3
"""Interactive/non-interactive config generator and dataset estimator for retention PUF runs."""
from __future__ import annotations
import argparse, json, math, sys
from pathlib import Path
from typing import Any, Dict, List

PATTERNS=["00","FF","AA","55"]

def ask(prompt, default=""):
    suffix=f" [{default}]" if default != "" else ""
    value=input(f"{prompt}{suffix}: ").strip()
    return value if value else str(default)

def ask_int(prompt, default): return int(ask(prompt, default))
def ask_float(prompt, default): return float(ask(prompt, default))
def ask_bool(prompt, default):
    d="y" if default else "n"
    return ask(f"{prompt} (y/n)", d).lower() in ("y","yes","1","true")

def split_regions(selections: List[Dict[str,Any]], rows_per_region: int) -> List[Dict[str,Any]]:
    if rows_per_region <= 0: raise ValueError("rows_per_region must be positive")
    out=[]
    for sel in selections:
        start=int(sel["row_start"]); end=int(sel["row_end"])
        if start < 0 or end < start: raise ValueError(f"Invalid selection {sel}")
        part=0; cur=start
        while cur <= end:
            last=min(end, cur+rows_per_region-1)
            out.append({"region_id":f"{sel['selection_id']}_{part:03d}","rank":int(sel["rank"]),
                        "bank_group":int(sel["bank_group"]),"bank":int(sel["bank"]),
                        "row_start":cur,"row_end":last})
            cur=last+1; part+=1
    return out

def retention_values(r: Dict[str,Any]) -> List[float]:
    vals=[]
    if r.get("include_zero_ms_control",True): vals.append(0.0)
    if r.get("sweep_type","explicit") == "explicit":
        vals += [float(x) for x in r.get("times_ms",[])]
    else:
        start=float(r["start_ms"]); stop=float(r["maximum_ms"]); step=float(r["step_ms"])
        x=start
        while x <= stop+1e-12: vals.append(x); x+=step
        if abs(vals[-1]-stop)>1e-9: vals.append(stop)
    return vals

def estimate(config: Dict[str,Any], assumed_flip_rate: float=0.001) -> Dict[str,Any]:
    rows=sum(r["row_end"]-r["row_start"]+1 for r in config["regions"])
    cl=int(config["geometry"]["cache_lines_per_row"])
    reps=int(config["experiment"]["repetitions"])
    points=len(retention_values(config["retention"]))
    challenges=len(PATTERNS)
    repetition_folders=len(config["regions"])*challenges*points*reps
    cache_records=rows*cl*challenges*points*reps
    raw_bytes=cache_records*64
    addressed_bytes=cache_records*128 + repetition_folders*2048
    index_bytes=cache_records*220
    raw_csv_bytes=cache_records*420 if config["experiment"].get("write_raw_records_csv",True) else 0
    bits=cache_records*64*8
    observation_bytes=int(bits*235) if config["experiment"].get("write_observations_csv",False) else 0
    flips_bytes=int(bits*max(0,min(1,assumed_flip_rate))*245)
    metadata_bytes=repetition_folders*3500
    total=raw_bytes+addressed_bytes+index_bytes+raw_csv_bytes+observation_bytes+flips_bytes+metadata_bytes
    wait_seconds=sum(retention_values(config["retention"]))/1000.0 * rows * challenges * reps
    return {
      "regions":len(config["regions"]),"total_rows_across_regions":rows,
      "retention_points_per_challenge":points,"patterns":challenges,"repetitions":reps,
      "repetition_folders":repetition_folders,"cache_line_records":cache_records,
      "reads_bin_bytes":raw_bytes,"raw_records_bin_estimated_bytes":addressed_bytes,
      "read_index_csv_estimated_bytes":index_bytes,"raw_records_csv_estimated_bytes":raw_csv_bytes,
      "observations_csv_estimated_bytes":observation_bytes,"flips_csv_estimated_bytes":flips_bytes,
      "metadata_estimated_bytes":metadata_bytes,"estimated_total_bytes":total,
      "estimated_total_gib":total/(1024**3),
      "minimum_programmed_retention_wait_seconds":wait_seconds,
      "notes":["reads.bin payload size is deterministic from configured geometry.",
               "CSV, metadata, and flips.csv sizes are estimates.",
               "Runtime estimate only counts programmed retention waits; FPGA/PCIe/write/read overhead is additional."]}

def build_from_schema(s: Dict[str,Any]) -> Dict[str,Any]:
    rg=s["region_generation"]
    regions=split_regions(rg["selections"], int(rg["rows_per_region"]))
    return {
      "collector_binary":"./retention_puf_collector","output_root":"./dram_retention_puf_data",
      "run":{"run_id":s["run_id"],"operator":s.get("operator",""),"institution":s.get("institution","")},
      "board":{"board_id":"ALVEO_U200_01","bitstream_file":"unknown","bitstream_sha256":"unknown"},
      "dimm":{"dimm_id":s["dimm_id"],"manufacturer":s.get("manufacturer","unknown"),
              "part_number":s.get("part_number","unknown"),"serial_number":s.get("serial_number","unknown"),
              "module_type":s.get("module_type","unknown"),"rank_count":s.get("rank_count","unknown"),
              "device_width":s.get("device_width","unknown"),"banks_per_group":int(s.get("banks_per_group",4))},
      "environment":{"temperature_c":float(s["temperature_c"]),"temperature_source":"manual_room_temperature",
                     "voltage_vdd_v":float(s["voltage_vdd_v"]),"voltage_source":"manual"},
      "timing":{"nominal_trcd_slots":int(s["nominal_trcd_slots"]),"nominal_trp_slots":int(s["nominal_trp_slots"]),
                "slot_ns_metadata":1.5,"write_spacing_slots":7,"read_spacing_slots":7,
                "write_recovery_slots":8,"read_to_precharge_slots":8,"final_guard_slots":16},
      "retention":s["retention"],
      "geometry":{"cache_lines_per_row":int(s.get("cache_lines_per_row",128)),"cache_line_bytes":64,"column_stride":8},
      "experiment":{"patterns":PATTERNS,"repetitions":int(s.get("repetitions",10)),
                    "write_observations_csv":bool(s.get("write_observations_csv",False)),
                    "write_raw_records_csv":bool(s.get("write_raw_records_csv",True)),
                    "timeout_seconds_per_retention_point":int(s.get("timeout_seconds_per_retention_point",86400)),
                    "max_retries":int(s.get("max_retries",0))},
      "regions":regions}

def interactive_schema() -> Dict[str,Any]:
    print("DRAM-Bender Retention-PUF configuration generator\n")
    run_id=ask("Run ID","retention_puf_room_temp_001")
    dimm_id=ask("DIMM ID","DIMM_001")
    temp=ask_float("Temperature C (manual metadata)",22.0)
    voltage=ask_float("VDD voltage V (manual metadata)",1.2)
    reps=ask_int("Repetitions per pattern/retention point/region",10)
    trcd=ask_int("Nominal safe tRCD NOP slots",9)
    trp=ask_int("Nominal safe tRP NOP slots",9)
    cl=ask_int("Cache lines per row",128)
    zero=ask_bool("Include 0 ms control",True)
    sweep=ask("Retention sweep type: explicit or range","explicit").lower()
    if sweep=="range":
        ret={"include_zero_ms_control":zero,"sweep_type":"range","start_ms":ask_float("Start retention ms",64),
             "maximum_ms":ask_float("Maximum retention ms",1000),"step_ms":ask_float("Step ms",64),"fabric_cycle_ns":6.0}
    else:
        values=[float(x.strip()) for x in ask("Retention times ms, comma separated","64,128,256,512,1000").split(",") if x.strip()]
        ret={"include_zero_ms_control":zero,"sweep_type":"explicit","times_ms":values,"fabric_cycle_ns":6.0}
    rows_per=ask_int("Rows per generated region",256)
    selections=[]
    n=ask_int("How many memory selections",1)
    for i in range(n):
        print(f"\nMemory selection {i+1}")
        selections.append({"selection_id":ask("Selection label",f"SEL_{i:02d}"),"rank":ask_int("Rank",0),
                           "bank_group":ask_int("Bank group",0),"bank":ask_int("Bank",0),
                           "row_start":ask_int("First row (inclusive)",0),"row_end":ask_int("Last row (inclusive)",255)})
    return {"run_id":run_id,"operator":ask("Operator (optional)",""),"institution":ask("Institution (optional)",""),
            "dimm_id":dimm_id,"manufacturer":"unknown","part_number":"unknown","module_type":"unknown",
            "rank_count":"unknown","device_width":"unknown","banks_per_group":4,
            "temperature_c":temp,"voltage_vdd_v":voltage,"nominal_trcd_slots":trcd,"nominal_trp_slots":trp,
            "retention":ret,"repetitions":reps,"cache_lines_per_row":cl,
            "write_observations_csv":ask_bool("Write huge per-bit observations.csv",False),
            "write_raw_records_csv":ask_bool("Write readable raw_records.csv",True),
            "region_generation":{"rows_per_region":rows_per,"selections":selections},
            "storage":{"assumed_flip_rate":ask_float("Assumed flip rate for size estimate",0.001),
                       "maximum_estimated_total_gib":ask_float("Maximum allowed estimated dataset GiB (0 = no limit)",0)}}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--schema",type=Path)
    ap.add_argument("--output",type=Path,default=Path("config.json"))
    ap.add_argument("--estimate-output",type=Path,default=Path("dataset_estimate.json"))
    ap.add_argument("--allow-over-limit",action="store_true")
    ns=ap.parse_args()
    try:
        schema=json.loads(ns.schema.read_text()) if ns.schema else interactive_schema()
        config=build_from_schema(schema)
        assumed=float(schema.get("storage",{}).get("assumed_flip_rate",0.001))
        est=estimate(config,assumed)
        limit=float(schema.get("storage",{}).get("maximum_estimated_total_gib",0) or 0)
        print(json.dumps(est,indent=2))
        if limit>0 and est["estimated_total_gib"]>limit and not ns.allow_over_limit:
            print(f"ERROR: estimated {est['estimated_total_gib']:.2f} GiB exceeds limit {limit:.2f} GiB",file=sys.stderr)
            return 2
        ns.output.write_text(json.dumps(config,indent=2)+"\n")
        ns.estimate_output.write_text(json.dumps(est,indent=2)+"\n")
        print(f"Wrote {ns.output} and {ns.estimate_output}")
        return 0
    except Exception as exc:
        print(f"ERROR: {exc}",file=sys.stderr); return 2

if __name__=="__main__": raise SystemExit(main())
