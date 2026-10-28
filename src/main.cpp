// agentPlans — C++ port of the TSM trip-list R scripts
//   2_Create_LDT_TripTable_GA_AL_template.R  (stage ldt)
//   3_get_ELTOD_TripTable_template.R         (stage eltod)
//
// Runs the full pipeline from one control file and writes the Hydra/AgentFlow
// gzipped trip list (ELTOD_tt_List_hourly.csv.gz).
//
// Usage:
//   agentPlans <control_file.txt>
#include <cstdio>
#include <exception>
#include <string>

#include "lookups.h"
#include "settings.h"
#include "stage_eltod.h"
#include "stage_ldt.h"
#include "util/rng.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "agentPlans — TSM market/trip-list builder\n"
                     "usage: %s <control_file.txt>\n", argv[0]);
        return 2;
    }
    try {
        ap::Settings s = ap::Settings::load(argv[1]);
        s.validate();
        std::printf("[agentPlans] scenario=%s year=%d loop=%d seed=%llu\n",
                    s.scenario_dir.c_str(), s.year, s.feedback_loop,
                    (unsigned long long)s.seed);
        std::printf("[agentPlans] resolution: SDT=%dmin LDT=%s truck=%s -> output=%dmin\n",
                    s.sdt_input_resolution,
                    s.ldt_input_resolution == 0 ? "daily" : "binned",
                    s.truck_input_resolution == 0 ? "daily" : "binned",
                    s.output_resolution);

        ap::Lookups lk;
        lk.load_taz_dma(s);
        lk.load_external_auto(s);
        lk.load_airport(s);
        lk.load_cruise(s);
        lk.load_cbm_ext(s);
        lk.load_os_state_ext(s);
        lk.load_tod(s);
        std::printf("[agentPlans] lookups loaded\n");

        ap::Rng rng(s.seed);

        // Stage A: long-distance trip list (in memory).
        std::vector<ap::LdtTrip> ldt = ap::run_stage_ldt(s, lk, rng);

        // Stage B: combine SDT + LDT + trucks -> Hydra trip list + OD tables.
        ap::run_stage_eltod(s, lk, rng, ldt);

        std::printf("[agentPlans] done.\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[agentPlans] ERROR: %s\n", e.what());
        return 1;
    }
}
