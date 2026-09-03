# Quick User Manual

## 1. Put the project in DRAM-Bender

```text
DRAM-Bender/sources/apps/RetentionPUFCollector/
```

## 2. Build

```bash
cd DRAM-Bender/sources/apps/RetentionPUFCollector
make
```

## 3. Create a config

Easiest:

```bash
python3 generate_config.py
```

Or manually:

```bash
cp config.example.json config.json
nano config.json
```

Main fields to set:

- `run.run_id`
- `dimm.dimm_id`
- `environment.temperature_c`
- `environment.voltage_vdd_v`
- safe `timing.nominal_trcd_slots`
- safe `timing.nominal_trp_slots`
- retention times under `retention`
- `experiment.repetitions`
- memory coordinates under `regions`

## 4. Retention delays

Explicit values:

```json
"retention": {
  "include_zero_ms_control": true,
  "sweep_type": "explicit",
  "times_ms": [64, 128, 256, 512, 1000],
  "fabric_cycle_ns": 6.0
}
```

Or a range:

```json
"retention": {
  "include_zero_ms_control": true,
  "sweep_type": "range",
  "start_ms": 64,
  "maximum_ms": 1024,
  "step_ms": 64,
  "fabric_cycle_ns": 6.0
}
```

## 5. Memory region

```json
{
  "region_id": "REGION_A",
  "rank": 0,
  "bank_group": 0,
  "bank": 0,
  "row_start": 0,
  "row_end": 255
}
```

Only listed regions are tested. Row ranges are inclusive.

## 6. Test safely first

```bash
cp config.safe_test.json config.json
nano config.json
python3 run_experiment.py --config config.json --dry-run
sudo python3 run_experiment.py --config config.json
```

Confirm the 0 ms control has no unexpected errors before starting a large retention sweep.

## 7. Full run

```bash
sudo python3 run_experiment.py --config config.json
```

Run the same command again to resume interrupted work.

## 8. Main output

Every repetition contains:

- `reads.bin` — all returned bytes.
- `raw_records.bin` — returned bytes plus memory addresses.
- `raw_records.csv` — readable addressed raw data, if enabled.
- `read_index.csv` — maps `reads.bin` offsets to addresses.
- `flips.csv` — flipped bits and direction.
- `trial.json` — metadata and status.

Decode addressed binary:

```bash
python3 decode_raw_records.py path/to/raw_records.bin --limit 5
```

Convert it to CSV:

```bash
python3 decode_raw_records.py path/to/raw_records.bin --output decoded.csv
```

## What the test does

For every row, pattern, retention time, and repetition:

```text
write known pattern
-> precharge
-> disable/keep refresh off
-> wait configured retention time
-> read at safe nominal timing
-> save complete returned data
-> record bit flips and direction
-> re-enable refresh
```

Patterns are always tested in this order: `00`, `FF`, `AA`, `55`.
