# Changelog

## 0.1.0
- Initial DRAM-Bender U200 retention-PUF raw-data collector.
- Configurable retention-time sweep with optional 0 ms control.
- Fixed challenge order: 00, FF, AA, 55.
- Row-by-row write -> refresh-off dwell -> nominal-timing read sequence.
- Complete `reads.bin`, self-describing `raw_records.bin`, location index, optional raw-record CSV, and bit-flip direction logging.
- Manual temperature and voltage metadata.
- Explicit configurable memory regions and automatic region generator.
- Dataset-size and minimum programmed-wait estimates.
- Resume, partial-result preservation, dry-run, and force-rerun support.
