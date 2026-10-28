# agentPlans — input CSV schemas

agentPlans is the C++ port of the two QGIS-plugin trip-list R scripts:

- `2_Create_LDT_TripTable_GA_AL_template.R`  → **stage `ldt`**
- `3_get_ELTOD_TripTable_template.R`         → **stage `eltod`**

The C++ port reads **CSV** (Excel `.xlsx` lookups must be exported to CSV once;
column **names are matched by header**, order-independent, extra columns ignored,
so *Save As → CSV* of each sheet keeping the original header text works).

**Any input may be gzipped.** Every reader auto-detects gzip by magic bytes
(via the vendored miniz decoder), so each file can be plain `.csv` or `.csv.gz`
interchangeably — no setting needed. (e.g. a large `truck_odme` or SDT trip list
can be supplied as `.csv.gz`.)

---

## 1. Excel files being replaced → CSV equivalents

### 1a. `External_Auto_Internal_Airport_Zone_Shares.xlsx`  (3 sheets → 3 CSVs)

This is a multi-sheet workbook; export **each sheet to its own CSV**.

**Sheet `External_Auto` → `external_auto_shares.csv`**
Used for EI/IE auto external-station assignment and crossborder (CBM) lookups.

| column      | type | meaning |
|-------------|------|---------|
| `CBM`       | int  | crossborder group: `0` = regular externals; `1,2,3` = North-FL DMA crossborder groups (Panhandle→Jacksonville) |
| `DMA`       | int  | DMA id (1–10) the share belongs to |
| `NextGen_TAZ` | int | external-station TAZ to assign |
| `share`     | num  | sampling weight (need not sum to 1) |

**Sheet `Airport` → `airport_shares.csv`**
Airport-zone assignment for air tours by DMA.

| column      | type | meaning |
|-------------|------|---------|
| `Type`      | str  | label column (e.g. `Airport`); not used in logic, kept for parity |
| `DMA`       | int  | DMA id (1–9; DMA 9 = Disney, auto-duplicated from DMA 5 in code) |
| `NextGen_TAZ` | int | airport TAZ to assign |
| `share`     | num  | sampling weight |

> The R code appends an error/guard row `("Airport", 3, 7536, 0.01)` — i.e.
> `Type=Airport, DMA=3, NextGen_TAZ=7536, share=0.01`. The C++ port re-creates
> this row internally; do **not** add it to the CSV.

**Sheet `CanaveralCruiseTrips` → `canaveral_cruise.csv`**
Hotel-room origins for the Port Canaveral cruise market.

| column           | type | meaning |
|------------------|------|---------|
| `NextGen_TAZ`    | int  | hotel-room origin TAZ |
| `HotelRoom_Share`| num  | sampling weight |

### 1b. `TimeSegment_distributions.xlsx` → `tod_distributions.csv`

One row per 15-minute time segment (96 rows). Used in **both** stages.

| column      | type | meaning |
|-------------|------|---------|
| `TimeSeg_96`| int  | time segment id, 1–96 (15-min bins over the day) |
| `Res_Long`  | num  | LDT resident time-of-day weight (stage `ldt`) |
| `Vis_Long`  | num  | LDT visitor time-of-day weight (stage `ldt`) |
| `Res_Short` | num  | SDT resident weight (currently unused — code samples 30-min periods directly; keep column for forward-compat) |
| `Vis_Short` | num  | SDT visitor weight (currently unused) |
| `3_Axles`   | num  | light-truck time-of-day weight (stage `eltod`) |
| `4_Axles`   | num  | medium-truck time-of-day weight (stage `eltod`) |
| `5p_Axles`  | num  | heavy-truck time-of-day weight (stage `eltod`) |

> CSV headers that begin with a digit (`3_Axles`, `4_Axles`, `5p_Axles`) are
> fine — they are matched as literal strings.

### 1c. `GA_AL_LDT_Destinations.xlsx` → `ga_al_ldt_destinations.csv`

Crossborder GA/AL destination TAZs; all are tagged `DMA = 10` in code.

| column | type | meaning |
|--------|------|---------|
| `TAZ`  | int  | GA/AL crossborder TAZ |

### 1d. `CrossBorder_TAZ_to_Externals_TSMv4.xlsx` → `cbm_external_lookup.csv`

Maps an internal TAZ to its crossborder external station (used when
`use_cbm_external_lookup = true`).

| column     | type | meaning |
|------------|------|---------|
| `TAZ`      | int  | internal origin TAZ |
| `CBM_Ext`  | int  | external-station TAZ for that origin |

### 1e. `OutofState_to_ExternalStations_Mapping.csv` (key `os_state_externals`)

Entry station for visitor (EI) auto tours by origin state, used when
`apply_originState_based_externals = true`. Replaces the state lists that were
hardcoded in the R script and in stage_ldt.cpp. A row with `ZoneID` set overrides
its state's row for that origin zone (none today; the table equals the R lists). A state missing
from the table enters on I-95 and is reported in the log.

| column       | type | meaning |
|--------------|------|---------|
| `StateFIPS`  | int  | origin state FIPS (`trOState`) |
| `State`      | str  | postal code (documentation only) |
| `ZoneID`     | int  | blank = whole state; set = this LDT origin zone (`trOZone`) only |
| `Interstate` | str  | I-10 / I-75 / I-95 (documentation only) |
| `ExtStation` | int  | external-station TAZ (11504 / 11548 / 11560) |
| `Note`       | str  | why the row differs from the original R lists |

---

## 2. Files that are already CSV (used as-is)

These are read with `fread()` in R and need no conversion.

### `Florida_Zones_appended_STL_TSMv4.csv`  (TAZ→DMA)
| `TAZ` (int) | `DMA` (int) | … other columns ignored |

### `Skim_distbased.csv`  (distance skim; first 3 columns only)
| col 1 = origin TAZ | col 2 = destination TAZ | col 3 = round-trip distance (miles) |
The port reads it by position and renames to `originTaz, destinationTaz, distance`.

### `<scenario>/trips_<feedback_loop>.csv`  (SDT resident trip list, from SDTModel)
Required columns: `originTaz, destinationTaz, period, valueOfTime, tripMode,
expansionFactor, destinationPurpose, originPurpose, hh_id, person_id, tour_id, trip_id`.

### `<scenario>/visitorTrips.csv`  (SDT visitor trip list)
Required columns: `originTaz, destinationTaz, period, valueOfTime, tripMode,
expansionFactor, destinationPurpose, originPurpose, tour_id, trip_id`.

### `<scenario>/FL_LD_tour_out.csv` + `OS_LD_tour_out.csv`  (LDT tours)
LDT tour output. Key columns used: `hhId, trNo, trMonth, trPurpose,
trPartySize, trNightsAway, trMode, trOState, trDState, trOZone, trDZone,
trAutoDistance, trTravelTime, trTravelCost, trExpFactor, trORegion, trDRegion,
trVOT, hhHeadAge, hhIncome`.

### Truck ODME table — `TSMv5_Truck_ODME_TT.csv`
Columns: `O, D, heavy, light, medium` (matrix form; one row per O-D with the
three vehicle-class daily volumes).

### SDT synthetic households — `synthetic_households_<yy>.csv`
Used to attach income to the resident trip list. Required: `household_id,
HHINCADJ`.

### External-station targets — `ldt_external_targets.csv` (written by the GUI)
Drives external-station calibration: OD trips serving each external zone are
scaled so the zone's modeled volume hits a target (`scale = target / modeled`).

| column | type | meaning |
|--------|------|---------|
| `interstate` | str | label (e.g. `I-75`); informational |
| `ext_zone_id` | int | external-station TAZ — **must match `ext_station_i10/i75/i95` in the control file** |
| `base_count_<yy>` | num | base-year count (matched by `base_count` prefix); used for growth-rate specs |
| `future_target_or_growth` | str | an absolute target count (e.g. `60000`) **or** a growth rate (e.g. `1.2%`) |

A growth-rate spec resolves to `base_count * (1 + rate/100 * (year - external_base_year))`
(linear annual growth). An absolute number is used as the target directly.

### Optional base-count override — `external_base_counts.csv`
If `external_base_counts` is set in the control file, it overrides the per-zone
base counts for growth-rate specs. Columns: `ext_zone_id`, `base_count_<yy>`.

---

## 3. Outputs

| output | format | notes |
|--------|--------|-------|
| `LD_tour_out_processed.csv`               | csv    | stage `ldt` consolidated tour list |
| `LD_tour_out_processed_ToD_Trips.csv`     | csv    | stage `ldt` trips w/ direction + ToD (input to stage `eltod`) |
| `ELTOD_LDT_tt.csv`                         | csv    | optional LDT-only ELToD OD table |
| `ELTOD_tt_HourClock.csv` (`hourly_table_out`) | csv | **hourly OD trip table** (wide by market/VOT, `STARTTIME` HH:00) — matches the original R `3_get_ELTOD` output; always aggregated to the clock hour. Toggle with `write_hourly_table`. |
| `ELTOD_SDT_Res_hourly.csv`                | csv    | SDT resident purpose×VOT OD table |
| **`ELTOD_tt_List_hourly.csv.gz`** (`trip_table_out`) | **gz csv** | **the Hydra/AgentFlow trip list — primary deliverable** |

The trip-list gz uses the exact Hydra schema (14 columns, in this order):

```
hh_id,person_id,tour_id,trip_id,valueOfTime,purpose,depart_time,O,D,marketVot,vehTrips,occupancy,hhIncome,market
```

`depart_time` is an HH:MM:SS clock (start of the output period bin; width =
`output_resolution`).
