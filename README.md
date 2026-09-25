# hopperscan

Extracts Gizmo hopper range matrices from a robot log straight into per-station
CSVs, without the intermediate multi-gigabyte JSON dump.

The old path was `ark-logtool --dump` (6.9 GB of JSON for a day's log) piped
through a Python extractor. `hopperscan` reads the two channels it actually
needs, decodes them into the generated rbuf types, and writes the ~40 MB of CSV
you were after. It also resolves the `station_id` column, which the JSON path
left empty, by joining against the log's own XCU mapping.

## Build

```sh
./build.sh          # -> bin/hopperscan
```

`build.sh` compiles against the lab37 checkout (`$LAB37_DIR`, default
`~/Projects/lab37`), which supplies the toolchain and all 77 static libraries
the log reader needs. **Nothing in the lab37 tree is modified.** The attachment
is a single CMake cache variable, `CMAKE_PROJECT_Lab37_INCLUDE`, pointing at
this project's `inject.cmake`; `build.sh` sets it if it is missing.

That variable lives in `lab37/build/CMakeCache.txt`, so it survives normal
rebuilds but not `./make.sh --expunge` or a deleted `build/`. After either of
those, `./build.sh` just re-adds it.

## Use

Find out what the log contains:

```sh
bin/hopperscan <log-id> --list
```

```
STATION  BUS                  NODE   RANGE  SPAD   SIZE     SAMPLES    SENSOR
-        bag_conveyor_1       0x50   0      0      8x8      50         unmapped
2        food_dispenser_1     0x50   0      0      48x32    50         hopper
3        food_dispenser_2     0x50   0      0      48x32    51         hopper
4        food_dispenser_3     0x50   0      0      48x32    50         hopper
5        food_dispenser_4     0x50   0      0      48x32    50         hopper
6        food_dispenser_5     0x50   0      0      48x32    1          hopper
7        food_dispenser_6     0x50   0      0      48x32    50         hopper
-        lidder_1             0x50   0      0      8x8      50         unmapped
```

Station N sits on `food_dispenser_(N-1)`. Rows without a station are range sensors
on other hardware, which have no ingredient dispenser mapping to resolve against.


Then pull the stations you want:

```sh
bin/hopperscan <log-id> --station 3
bin/hopperscan <log-id> --station 2,3
bin/hopperscan <log-id> --station all
```

One CSV per station, named by `--output`'s template (default
`range_matrix_station_{station}.csv`). `{bus}` works too, and a template with no
placeholder writes a single combined file.

## Options

| Flag | Meaning |
|---|---|
| `-s, --station` | Station ids: one, comma separated, repeatable, or `all`. Required unless `--list`. |
| `-l, --list` | Show the range sensors in the log and exit. |
| `--node` | Node ids, hex or decimal, or `all`. Defaults to `0x50`. |
| `--bus` | Glob over bus names. |
| `--range-index` | Keep only one range sensor index per node. |
| `--include-object-range` | Also emit the BSU bowl-detect sensors (see below). |
| `--unmapped` | Emit sensors the mapping does not resolve to a station. |
| `--no-dedupe` | Emit every republished copy of a reading. |
| `--columns legacy` | Emit only the original 13 columns. |
| `-n` / `-x` | Time bounds, in seconds, matching `ark-logtool`. |
| `--force` | Overwrite existing output files. |

## Notes

**Two range sensors per bus.** Every dispenser bus carries a second range sensor
on node `0x58` — the Gizmo BSU, which detects whether a bowl is in the puck
rather than looking into the hopper. The default `--node 0x50` excludes it. An
ingredient dispenser can also map the BSU as `object_range` on the same node as
the hopper, separated only by range index; those are skipped unless you pass
`--include-object-range`, and `--list` labels both so you can tell them apart.

**Dedupe.** `/xcu/state` publishes at roughly 10 Hz while these sensors read at
about 1 Hz, so most published matrices are repeats. A reading counts as fresh
when the board's own timestamps move. On a 50 s log that is 508 published
matrices per station reduced to 50 actual reads; `--no-dedupe` keeps all of
them.

**Columns.** The first 13 match the old Python extractor's output exactly, so
existing notebooks keep working. Appended after them: `range_index`, `rows`,
`cols`, `quantization_min_mm`, `quantization_max_mm`, `measured_read_hz` — all
present in `XcuRangeSensor` but dropped by the old JSON path.
=======
cli tool for pulling down 0x50 range matrix data from individual gizmo stations

