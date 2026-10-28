#include "lookups.h"

#include <cstdio>
#include <stdexcept>

#include "util/csv.h"

namespace ap {

void Lookups::load_taz_dma(const Settings& s) {
    CsvTable t = load_csv(s.taz_dma);
    int cT = t.require("TAZ", s.taz_dma);
    int cD = t.require("DMA", s.taz_dma);
    for (size_t r = 0; r < t.size(); ++r) taz_dma[t.ll(r, cT)] = static_cast<int>(t.ll(r, cD));

    // GA/AL crossborder destinations -> DMA 10 (R: mutate(DMA = 10)).
    if (!s.ga_al_destinations.empty()) {
        CsvTable g = load_csv(s.ga_al_destinations);
        int gT = g.require("TAZ", s.ga_al_destinations);
        for (size_t r = 0; r < g.size(); ++r) taz_dma[g.ll(r, gT)] = 10;
    }
}

void Lookups::load_external_auto(const Settings& s) {
    CsvTable t = load_csv(s.external_auto_shares);
    int cC = t.require("CBM", s.external_auto_shares);
    int cTaz = t.require("NextGen_TAZ", s.external_auto_shares);
    int cSh = t.require("share", s.external_auto_shares);
    for (size_t r = 0; r < t.size(); ++r) {
        int cbm = static_cast<int>(t.ll(r, cC));
        auto& w = ext_auto_by_cbm[cbm];
        w.values.push_back(static_cast<int>(t.ll(r, cTaz)));
        w.probs.push_back(t.num(r, cSh));
    }
}

void Lookups::load_airport(const Settings& s) {
    CsvTable t = load_csv(s.airport_shares);
    int cD = t.require("DMA", s.airport_shares);
    int cTaz = t.require("NextGen_TAZ", s.airport_shares);
    int cSh = t.require("share", s.airport_shares);
    for (size_t r = 0; r < t.size(); ++r) {
        int dma = static_cast<int>(t.ll(r, cD));
        auto& w = airport_by_dma[dma];
        w.values.push_back(static_cast<int>(t.ll(r, cTaz)));
        w.probs.push_back(t.num(r, cSh));
    }
    // R guard row: rbindlist(err_row = list("Airport", 3, 7536, 0.01)).
    {
        auto& w = airport_by_dma[3];
        w.values.push_back(7536);
        w.probs.push_back(0.01);
    }
    // Disney DMA 9 == Central FL DMA 5 (R duplicates DMA 5 rows as DMA 9).
    airport_by_dma[9] = airport_by_dma[5];
}

void Lookups::load_cruise(const Settings& s) {
    CsvTable t = load_csv(s.canaveral_cruise);
    int cTaz = t.require("NextGen_TAZ", s.canaveral_cruise);
    int cSh = t.require("HotelRoom_Share", s.canaveral_cruise);
    for (size_t r = 0; r < t.size(); ++r) {
        cruise.values.push_back(static_cast<int>(t.ll(r, cTaz)));
        cruise.probs.push_back(t.num(r, cSh));
    }
}

void Lookups::load_cbm_ext(const Settings& s) {
    if (s.cbm_external_lookup_csv.empty()) return;
    CsvTable t = load_csv(s.cbm_external_lookup_csv);
    int cT = t.require("TAZ", s.cbm_external_lookup_csv);
    int cE = t.require("CBM_Ext", s.cbm_external_lookup_csv);
    for (size_t r = 0; r < t.size(); ++r) cbm_ext[t.ll(r, cT)] = t.ll(r, cE);
}

void Lookups::load_os_state_ext(const Settings& s) {
    if (!s.apply_originState_based_externals) return;
    if (s.os_state_externals.empty())
        throw std::runtime_error("apply_originState_based_externals = true needs os_state_externals "
                                 "(OutofState_to_ExternalStations_Mapping.csv)");
    CsvTable t = load_csv(s.os_state_externals);
    int cS = t.require("StateFIPS", s.os_state_externals);
    int cZ = t.require("ZoneID", s.os_state_externals);
    int cE = t.require("ExtStation", s.os_state_externals);
    for (size_t r = 0; r < t.size(); ++r) {
        if (t.at(r, cZ).empty()) os_state_ext[static_cast<int>(t.ll(r, cS))] = t.ll(r, cE);
        else os_zone_ext[t.ll(r, cZ)] = t.ll(r, cE);
    }
    std::printf("[lookups] OS entry stations: %zu states, %zu zone overrides\n",
                os_state_ext.size(), os_zone_ext.size());
}

void Lookups::load_tod(const Settings& s) {
    CsvTable t = load_csv(s.tod_distributions);
    int cSeg = t.require("TimeSeg_96", s.tod_distributions);
    int cRl = t.col("Res_Long"), cVl = t.col("Vis_Long");
    int c3 = t.col("3_Axles"), c4 = t.col("4_Axles"), c5 = t.col("5p_Axles");
    for (size_t r = 0; r < t.size(); ++r) {
        tod.seg96.push_back(static_cast<int>(t.ll(r, cSeg)));
        tod.res_long.push_back(t.num(r, cRl));
        tod.vis_long.push_back(t.num(r, cVl));
        tod.ax3.push_back(t.num(r, c3));
        tod.ax4.push_back(t.num(r, c4));
        tod.ax5p.push_back(t.num(r, c5));
    }
}

void Lookups::load_skim(const Settings& s) {
    if (skim_loaded || s.distance_skim.empty()) return;
    // The distance skim is OPTIONAL: it only drives distance-based segmentation
    // (air-tour access/egress, SDT >50mi). agentPlans has no OMX reader yet (a CSV
    // is expected; see the SkimConverter omx->csv reverse task), so a missing file
    // must NOT abort the run -- warn and continue with no skim (dist() returns -1,
    // so the distance thresholds simply never trigger).
    CsvTable t;
    try {
        t = load_csv(s.distance_skim);
    } catch (const std::exception& e) {
        std::printf("[lookups] WARNING: distance skim unavailable (%s); "
                    "distance-based segmentation skipped\n", e.what());
        skim_loaded = true;   // don't retry
        return;
    }
    // R reads the first three columns positionally: O, D, distance.
    if (t.header.names.size() < 3) {
        std::printf("[lookups] WARNING: %s has <3 columns; distance skim skipped\n",
                    s.distance_skim.c_str());
        skim_loaded = true;
        return;
    }
    for (size_t r = 0; r < t.size(); ++r) {
        long long o = to_ll(t.at(r, 0));
        long long d = to_ll(t.at(r, 1));
        skim[o * SKIM_MUL + d] = to_double(t.at(r, 2));
    }
    skim_loaded = true;
    std::printf("[lookups] distance skim: %zu O-D pairs\n", skim.size());
}

} // namespace ap
