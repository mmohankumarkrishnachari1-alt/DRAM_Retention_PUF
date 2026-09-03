#!/usr/bin/env python3
"""Decode self-describing raw_records.bin files produced by retention_puf_collector."""
from __future__ import annotations
import argparse, csv, json, struct, sys
from pathlib import Path

MAGIC=b"RTPUF001"
FMT="<QIiiiiIIIQIB3xQ64s"
RECORD_SIZE=struct.calcsize(FMT)
FIELDS=("record_index","repetition","rank","bank_group","bank","flat_bank","row","cache_line",
        "column","reads_bin_offset","payload_length","pattern_byte","retention_sleep_cycles","payload")

def read_header(f):
    magic=f.read(8)
    if magic!=MAGIC: raise ValueError(f"Bad magic {magic!r}; expected {MAGIC!r}")
    raw=f.read(4)
    if len(raw)!=4: raise ValueError("Truncated header length")
    n=struct.unpack("<I",raw)[0]
    payload=f.read(n)
    if len(payload)!=n: raise ValueError("Truncated JSON header")
    return json.loads(payload.decode("utf-8"))

def records(f):
    while True:
        blob=f.read(RECORD_SIZE)
        if not blob: return
        if len(blob)!=RECORD_SIZE: raise ValueError("Truncated raw record")
        values=struct.unpack(FMT,blob)
        d=dict(zip(FIELDS,values))
        d["pattern_hex"]=f"{d.pop('pattern_byte'):02X}"
        d["data_hex"]=d.pop("payload").hex()
        yield d

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("--output", type=Path, help="Write all records as CSV")
    ap.add_argument("--limit", type=int, default=5, help="Records printed when --output is omitted")
    ns=ap.parse_args()
    with ns.input.open("rb") as f:
        header=read_header(f)
        print(json.dumps(header,indent=2,sort_keys=True))
        it=records(f)
        if ns.output:
            names=[x for x in FIELDS if x not in ("pattern_byte","payload")]+["pattern_hex","data_hex"]
            with ns.output.open("w",newline="",encoding="utf-8") as out:
                w=csv.DictWriter(out,fieldnames=names); w.writeheader()
                count=0
                for row in it: w.writerow(row); count+=1
            print(f"Wrote {count} records to {ns.output}")
        else:
            for i,row in enumerate(it):
                if i>=ns.limit: break
                print(json.dumps(row,sort_keys=True))
    return 0

if __name__=="__main__": raise SystemExit(main())
