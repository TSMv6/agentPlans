#pragma once
// Control-file (key=value) parser + run settings for agentPlans.
// Mirrors the argument lists of the two R scripts plus their internal toggles,
// all hoisted into one control file so the full pipeline runs from one command.
#include <string>
#include <unordered_map>
#include <vector>

namespace ap {

struct Settings {
    // --- paths / scenario (script args) ---
    std::string catalog_dir;          // e.g. C:/TSM_NextGen_v6
    std::string scenario_dir;         // e.g. C:/TSM_NextGen_v6/Base/TSMv6_2024
    int year = 2024;                  // 4-digit; stored internally as yy = year-2000
    int feedback_loop = 1;            // SDT resident trips_<loop>.csv
    uint64_t seed = 1;                // base RNG seed (R used set.seed(1L)/(3..6L))

    // --- temporal resolution ---
    // Each demand source has a native bin; the pipeline works internally at the
    // finest grain (15-min, the resolution of the ToD share table) and collapses
    // the trip-list `period` column to `output_resolution` at write time.
    //   - SDT is binned (default 30-min periods 0..47).
    //   - LDT and trucks are DAILY (res 0): timed to 15-min via ToD shares.
    // output_resolution: minutes per output bin (15 -> period 0..95, 30 -> 0..47).
    int output_resolution = 30;       // 15 or 30
    int sdt_input_resolution = 30;    // SDT trip-list native bin (15 or 30)
    int ldt_input_resolution = 0;     // 0 = daily (timed via ToD)
    int truck_input_resolution = 0;   // 0 = daily (timed via axle ToD)

    // --- converted xlsx lookups (now CSV) ---
    std::string external_auto_shares;     // external_auto_shares.csv
    std::string airport_shares;           // airport_shares.csv
    std::string canaveral_cruise;         // canaveral_cruise.csv
    std::string tod_distributions;        // tod_distributions.csv
    std::string ga_al_destinations;       // ga_al_ldt_destinations.csv
    std::string cbm_external_lookup_csv;  // cbm_external_lookup.csv

    // --- already-CSV inputs ---
    std::string taz_dma;              // Florida_Zones_appended_STL_TSMv4.csv (TAZ,DMA)
    std::string distance_skim;        // Skim_distbased.csv
    std::string truck_odme;           // TSMv5_Truck_ODME_TT.csv
    std::string sdt_syn_hh;           // synthetic_households_<yy>.csv
    std::string fl_ldt_tours;         // FL_LD_tour_out.csv
    std::string os_ldt_tours;         // OS_LD_tour_out.csv
    std::string sdt_res_trips;        // trips_<loop>.csv  (override; else derived)
    std::string sdt_vis_trips;        // visitorTrips.csv  (override; else derived)

    // External-station target calibration (GUI-produced ldt_external_targets.csv).
    // Scales all OD trips serving each external zone to a target volume:
    // scale = target / modeled. target is either an absolute count or a growth
    // rate (e.g. "1.2%") applied linearly to a base-year count over
    // (year - external_base_year), matching the model's annual-growth convention.
    std::string ldt_external_targets; // default: scenario_dir/ldt_external_targets.csv
    std::string external_base_counts; // optional base-count override (plugin config)
    int external_base_year = 2024;
    // FL TAZs within 50 mi of the GA/AL border (one 'taz' column). A GA/AL(DMA 10)
    // <-> FL LDT trip is tagged "CrossBorderCommute" only when its FL end is in
    // this set; otherwise native long-distance commutes stay "Commute".
    std::string ldt_border_zones;     // default: scenario_dir/border_zones_50mi.csv
    bool apply_external_targets = true;

    // External-station TAZ ids used for LDT origin-state assignment. Keep these
    // aligned with the ext_zone_id values in ldt_external_targets.csv.
    long long ext_station_i10 = 11504;
    long long ext_station_i75 = 11548;
    long long ext_station_i95 = 11560;

    // --- outputs (override; else derived from scenario_dir) ---
    std::string trip_table_out;       // gz Hydra trip list (ELTOD_tt_List_hourly.csv.gz)
    std::string hourly_table_out;     // hourly OD trip table CSV (ELTOD_tt_HourClock.csv),
                                      // wide by market/VOT with STARTTIME HH:00 -- matches
                                      // the original R 3_get_ELTOD output.
    bool write_hourly_table = true;   // also emit the hourly OD trip table

    // --- toggles: stage ldt (script 2) ---
    bool apply_originState_based_externals = true;
    bool use_cbm_external_lookup = true;
    bool separate_res_markets = true;
    bool exclude_non_Interstate = false;
    bool remove_NorthFL_res_trips = false;
    bool fineCalib = false;
    bool compute_LDT_TT = true;

    // --- toggles: stage eltod (script 3) ---
    bool track_sdt_grt50M = false;
    bool track_AirTours = true;
    bool appendNewMkt = false;
    bool aggregate_purp = false;
    bool telework = false;
    bool LimitCounties = false;
    bool write_HourClock_format = true;
    bool use_hour = true;
    bool doTransims = false;

    // VOT thresholds: LDT (legacy 2015) vs SDT (post-COVID). Both retained
    // because the R scripts use different pairs.
    double vot_ldt_low = 6.51,  vot_ldt_high = 11.68;
    double vot_sdt_low = 12.81, vot_sdt_high = 22.83;

    // Truck scaling base year (yy) and axle/cost parameters.
    int truck_base_year = 23;
    double trk_axle_scale = 3.5;

    std::vector<std::string> limit_counties;  // when LimitCounties

    int yy() const { return year - 2000; }

    // 15-min internal segment (1..96) -> 0-based output period at output_resolution.
    int period_out(int seg15) const {
        if (output_resolution == 15) return seg15 - 1;   // 0..95
        return (seg15 - 1) / 2;                           // 30-min: 0..47
    }

    // Resolve a derived path under scenario_dir if the override is empty.
    std::string scen(const std::string& override_val, const std::string& fname) const;

    static Settings load(const std::string& control_file);
    void validate() const;
};

} // namespace ap
