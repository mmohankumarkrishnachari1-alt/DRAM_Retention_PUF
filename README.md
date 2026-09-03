# DRAM-Bender U200 Retention-PUF Raw Data Collector

Configurable retention-PUF data collector for DDR4 DIMMs on an Alveo U200 running DRAM-Bender.

This repository is intentionally structured like the companion latency-PUF project. The main difference is the experimental mechanism:

- **Latency PUF:** reduce tRCD/tRP and observe timing-induced errors.
- **Retention PUF:** write known data, disable auto-refresh, wait a configured retention interval, then read at safe nominal tRCD/tRP values and observe retention-induced errors.

The collector does **not** classify final PUF cells. It preserves complete raw read data, exact logical DRAM locations, mismatch information, and flip direction so later analysis can choose selection thresholds without rerunning the hardware experiment.

## Experiment sequence

For each explicitly configured region, the fixed challenge order is:

1. `CHALLENGE_00`
2. `CHALLENGE_FF`
3. `CHALLENGE_AA`
4. `CHALLENGE_55`

For each challenge, the runner optionally performs a 0 ms control and then every configured retention delay. Each point is repeated 10 times by default.

For each tested row and repetition the native collector performs:

1. Disable DRAM-Bender auto-refresh for the row experiment.
2. PRE / ACT using safe nominal timing.
3. Write the selected pattern to every configured cache line in the row.
4. PRE the row and satisfy nominal tRP.
5. Wait the configured retention interval using DRAM-Bender `SMC_SLEEP` instructions.
6. ACT and READ the row using safe nominal tRCD.
7. Save every returned byte and every mismatch.
8. Re-enable auto-refresh before moving to the next row.

The configured `retention_time_ms` is the **additional refresh-off dwell after safe precharge**. The nominal tRP spacing immediately before that dwell is not included in the requested retention time.

## Important implementation note

DRAM-Bender documents `SMC_SLEEP()` in fabric cycles (6 ns by default), so the program converts milliseconds to fabric cycles. Long waits are automatically split into multiple `SMC_SLEEP` instructions when one 32-bit sleep count is not large enough.

`fabric_cycle_ns` is configurable and defaults to `6.0`. Confirm this against the actual DRAM-Bender build used by the receiving laboratory.

## Repository files

- `retention_puf_collector.cpp` — native DRAM-Bender collector.
- `run_experiment.py` — expands the config and runs every region/pattern/retention point.
- `generate_config.py` — interactive/schema-based config generator and dataset estimator.
- `decode_raw_records.py` — decodes the addressed binary output.
- `config.example.json` — normal example experiment.
- `config.safe_test.json` — one-row, one-repetition first test.
- `experiment_schema.example.json` — compact schema for automatic config generation.
- `Makefile` — builds the native collector inside DRAM-Bender.
- `CHANGELOG.md` — release notes.

## Compatibility

This project targets the current DRAM-Bender C++ API and the Alveo U200 DDR4 prototype. It uses the published `SMC_PRE`, `SMC_ACT`, `SMC_WRITE`, `SMC_READ`, `SMC_NOP`, `SMC_SLEEP`, `SoftMCPlatform::execute`, `receiveData`, `reset_fpga`, and `set_aref` interfaces.

Successful operation still depends on the U200 bitstream matching the installed DIMM's organization and on the local DRAM-Bender/XDMA setup already working.

## Installation

Copy this repository into:

```text
DRAM-Bender/sources/apps/RetentionPUFCollector/
```

Expected layout:

```text
DRAM-Bender/
├── boost-lib/
└── sources/
    ├── api/
    └── apps/
        └── RetentionPUFCollector/
            ├── Makefile
            ├── retention_puf_collector.cpp
            ├── run_experiment.py
            ├── generate_config.py
            ├── decode_raw_records.py
            └── config.example.json
```

Build:

```bash
make
```

## Recommended first test

Start with the included one-row configuration:

```bash
cp config.safe_test.json config.json
nano config.json
```

At minimum, change the DIMM label, correct memory coordinates, temperature, voltage, and nominal timings for the actual setup.

Preview without running hardware:

```bash
python3 run_experiment.py --config config.json --dry-run
```

Then run:

```bash
sudo python3 run_experiment.py --config config.json
```

The safe test uses one row, one repetition, a 0 ms control, and a 64 ms retention point.

## Automatic configuration generator

The easiest way to create a full experiment config is:

```bash
python3 generate_config.py
```

The generator asks for:

- run ID and DIMM ID;
- temperature and VDD metadata;
- nominal tRCD and tRP;
- repetitions;
- retention-delay sweep;
- cache lines per row;
- rank, bank group, bank, and row ranges;
- rows per generated region;
- whether large CSV outputs should be enabled;
- an assumed flip rate for storage estimation;
- an optional maximum storage limit.

It writes:

```text
config.json
dataset_estimate.json
```

The estimate includes raw binary size, addressed binary size, CSV estimates, number of runs, and the minimum time spent purely in programmed retention waits.

### Generate from a reusable schema

```bash
cp experiment_schema.example.json experiment_schema.json
nano experiment_schema.json
python3 generate_config.py \
  --schema experiment_schema.json \
  --output config.json \
  --estimate-output dataset_estimate.json
```

## Configuration guide

### Run metadata

```json
"run": {
  "run_id": "retention_puf_room_temp_001",
  "operator": "",
  "institution": ""
}
```

- `run_id`: required unique experiment label.
- `operator`: optional.
- `institution`: optional.

### DIMM metadata

```json
"dimm": {
  "dimm_id": "DIMM_001",
  "manufacturer": "unknown",
  "part_number": "unknown",
  "serial_number": "unknown",
  "module_type": "unknown",
  "rank_count": "unknown",
  "device_width": "unknown",
  "banks_per_group": 4
}
```

`dimm_id` should uniquely identify the physical module. The remaining fields are metadata and can be filled when known.

Logical bank mapping uses:

```text
flat_bank = bank_group * banks_per_group + bank
```

Verify this against the actual U200 bitstream before a large experiment.

### Environment metadata

```json
"environment": {
  "temperature_c": 22.0,
  "temperature_source": "manual_room_temperature",
  "voltage_vdd_v": 1.2,
  "voltage_source": "manual"
}
```

These values are recorded with the experiment but do not control laboratory equipment.

Retention behavior is strongly temperature dependent, so record the best available temperature measurement for every run and use a new `run_id` when temperature changes.

### Safe DRAM timings

```json
"timing": {
  "nominal_trcd_slots": 9,
  "nominal_trp_slots": 9,
  "slot_ns_metadata": 1.5,
  "write_spacing_slots": 7,
  "read_spacing_slots": 7,
  "write_recovery_slots": 8,
  "read_to_precharge_slots": 8,
  "final_guard_slots": 16
}
```

- `nominal_trcd_slots`: safe ACT-to-READ/WRITE spacing used for this retention experiment.
- `nominal_trp_slots`: safe PRE-to-ACT spacing.
- `slot_ns_metadata`: approximate command-slot duration for metadata only.
- `write_spacing_slots`: spacing between consecutive writes.
- `read_spacing_slots`: spacing between consecutive reads.
- `write_recovery_slots`: guard time after the last write before PRE.
- `read_to_precharge_slots`: guard time after the last read before PRE.
- `final_guard_slots`: final delay before the row program ends.

Unlike the latency-PUF project, these timings are **not swept**. They should remain at experimentally verified safe values so observed errors come from retention, not timing violation.

### Retention sweep

Explicit list example:

```json
"retention": {
  "include_zero_ms_control": true,
  "sweep_type": "explicit",
  "times_ms": [64, 128, 256, 512, 1000],
  "fabric_cycle_ns": 6.0
}
```

- `include_zero_ms_control`: run a no-extra-retention control before the retention sweep.
- `sweep_type`: `explicit` or `range`.
- `times_ms`: exact positive retention delays when using `explicit`.
- `fabric_cycle_ns`: duration of one `SMC_SLEEP` fabric cycle; default 6 ns.

Range example:

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

This creates 64, 128, 192, ... 1024 ms plus the optional 0 ms control.

The example retention values are only starting points. The useful range depends strongly on the DIMM and temperature and should be chosen by the researcher.

### Memory geometry

```json
"geometry": {
  "cache_lines_per_row": 128,
  "cache_line_bytes": 64,
  "column_stride": 8
}
```

- `cache_lines_per_row`: number of cache-line operations covering the configured row.
- `cache_line_bytes`: must be `64` in this collector.
- `column_stride`: DRAM-Bender column-address increment.

Confirm these values with the active bitstream/DIMM organization.

### Experiment settings

```json
"experiment": {
  "patterns": ["00", "FF", "AA", "55"],
  "repetitions": 10,
  "write_observations_csv": false,
  "write_raw_records_csv": true,
  "timeout_seconds_per_retention_point": 86400,
  "max_retries": 0
}
```

- `patterns`: fixed challenge order.
- `repetitions`: repetitions per region, challenge, and retention point.
- `write_observations_csv`: when true, writes one row for every bit; this can be enormous.
- `write_raw_records_csv`: readable cache-line-level raw output.
- `timeout_seconds_per_retention_point`: host-side timeout for one complete region/challenge/delay point. Long retention tests may require a large value.
- `max_retries`: automatic retries after a failed point.

### Regions

Only explicitly listed regions are tested:

```json
"regions": [
  {
    "region_id": "REGION_A",
    "rank": 0,
    "bank_group": 0,
    "bank": 0,
    "row_start": 0,
    "row_end": 255
  }
]
```

Row ranges are inclusive.

## Running

Validate JSON:

```bash
python3 -m json.tool config.json > /dev/null
```

Preview:

```bash
python3 run_experiment.py --config config.json --dry-run
```

Run:

```bash
sudo python3 run_experiment.py --config config.json
```

Resume after interruption by running the same command again. Completed repetition folders are skipped automatically.

Force a rerun:

```bash
sudo python3 run_experiment.py --config config.json --force
```

## Output structure

```text
dram_retention_puf_data/
└── DIMM_DIMM_001/
    └── RUN_retention_puf_room_temp_001__TEMP_22C__VDD_1p2V/
        └── REGION_REGION_A__R0__BG0__B0__ROWS_000000-000255/
            └── CHALLENGE_00/
                ├── MODE_CONTROL/
                │   └── RETENTION_0ms/
                └── MODE_RETENTION_SWEEP/
                    ├── RETENTION_64ms/
                    ├── RETENTION_128ms/
                    └── ...
```

Each repetition folder contains:

- `reads.bin` — returned DRAM bytes only.
- `raw_records.bin` — self-describing addressed binary; each 64-byte response carries rank/bank/row/column metadata.
- `raw_records.csv` — optional readable addressed cache-line data.
- `read_index.csv` — maps offsets in `reads.bin` to DRAM coordinates.
- `flips.csv` — mismatching individual bits and `0_to_1` / `1_to_0` direction.
- `observations.csv` — optional every-bit CSV.
- `trial.json` — complete experiment metadata/status.
- `.complete` — resume marker.

## `raw_records.bin` format

The file begins with:

1. 8-byte magic `RTPUF001`.
2. Little-endian 32-bit JSON-header length.
3. JSON metadata header.
4. Fixed 128-byte records.

Every record contains:

- record index;
- repetition;
- rank;
- bank group;
- bank;
- flat bank;
- row;
- cache-line index;
- column;
- offset in `reads.bin`;
- payload length;
- challenge pattern byte;
- scheduled retention sleep cycles;
- returned 64-byte DRAM payload.

Decode it with:

```bash
python3 decode_raw_records.py path/to/raw_records.bin --limit 5
```

Convert to CSV:

```bash
python3 decode_raw_records.py path/to/raw_records.bin --output decoded.csv
```

## Data-size warning

`observations.csv` is usually the dominant output because it writes one line per bit. For large experiments, keep:

```json
"write_observations_csv": false
```

The binary files still preserve the complete read data, and `flips.csv` preserves observed errors.

Run `generate_config.py` before a large test and inspect `dataset_estimate.json`.

## Experimental caveats

- The collector tests logical DRAM-Bender coordinates. Physical chip-level adjacency/address mapping is not inferred.
- Retention results are highly temperature dependent.
- Reading a DRAM row restores its contents; this is why the default implementation treats each row as an independent write/wait/read experiment and re-enables refresh between rows.
- Large row ranges and long retention times can take many hours or days.
- Always validate the 0 ms control first. If the control shows errors, do not interpret later errors as retention failures until the baseline setup is fixed.

## Upstream

DRAM-Bender: https://github.com/CMU-SAFARI/DRAM-Bender

Companion latency-PUF repository: https://github.com/theprod45/DRAM_Bender_Latency_PUF
