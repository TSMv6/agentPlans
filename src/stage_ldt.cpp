#include "stage_ldt.h"

#include <algorithm>
#include <map>
#include <cstdio>
#include <unordered_set>

#include "util/csv.h"
#include "util/gz_io.h"

namespace ap {

namespace {

// One LDT tour with the fields stage A reads and the fields it derives.
struct Tour {
    // LDT inputs
    long long hhId = 0;
    int trPurpose = 0;
    double trPartySize = 1;
    int trMode = 1;
    int trOState = 0, trDState = 0;
    long long trOZone = 0, trDZone = 0;
    int trORegion = 0, trDRegion = 0;
    double trVOT = 0;
    double hhIncome = 0;
    // derived
    int org_DMA = 0, des_DMA = 0;
    std::string type;          // II/IE/EI/EE
    long long entry_ext = 0, exit_ext = 0, entry_airpt = 0, exit_airpt = 0;
    std::string accMode = "Auto", egrMode = "Auto", tripMode = "Auto";
    long long otaz = 0, dtaz = 0;
    std::string vot;
    int period = 0;
};

const std::unordered_set<long long> kDisneyZones = {4232, 4235, 4589, 4590, 4592, 4594,
                                                    4607, 4610, 4611, 4620, 4621, 4632};
constexpr long long OIA = 4657;       // Orlando Int'l (only airport with DME)
constexpr long long TAZ_PORT = 4381;  // Cape Canaveral port parking
constexpr int CRUISE_TRIPS = 5500;

std::vector<Tour> read_ldt_tours(const Settings& s) {
    // R step 0: rbind FL + OS tour outputs into LD_tour_out.csv, then read it.
    std::vector<std::string> files = {s.fl_ldt_tours, s.os_ldt_tours};
    std::vector<Tour> tours;
    for (const auto& f : files) {
        if (f.empty()) continue;
        GzLineReader rd(f);
        if (!rd.good()) throw std::runtime_error("cannot open LDT tours: " + f);
        std::string_view line;
        if (!rd.next_line(line)) continue;
        CsvHeader h; h.parse(line);
        int cHh = h.col("hhId"), cPurp = h.col("trPurpose"), cParty = h.col("trPartySize");
        int cMode = h.col("trMode"), cOS = h.col("trOState"), cDS = h.col("trDState");
        int cOZ = h.require("trOZone", f), cDZ = h.require("trDZone", f);
        int cOR = h.col("trORegion"), cDR = h.col("trDRegion");
        int cVot = h.col("trVOT"), cInc = h.col("hhIncome");
        std::vector<std::string_view> fld;
        while (rd.next_line(line)) {
            if (line.empty()) continue;
            split_csv(line, fld);
            Tour t;
            auto F = [&](int c) -> std::string_view { return c >= 0 && c < (int)fld.size() ? fld[c] : std::string_view(); };
            t.hhId = to_ll(F(cHh));
            t.trPurpose = (int)to_ll(F(cPurp));
            t.trPartySize = to_double(F(cParty));
            t.trMode = (int)to_ll(F(cMode));
            t.trOState = (int)to_ll(F(cOS));
            t.trDState = (int)to_ll(F(cDS));
            t.trOZone = to_ll(F(cOZ));
            t.trDZone = to_ll(F(cDZ));
            t.trORegion = (int)to_ll(F(cOR));
            t.trDRegion = (int)to_ll(F(cDR));
            t.trVOT = to_double(F(cVot));
            t.hhIncome = to_double(F(cInc));
            tours.push_back(std::move(t));
        }
    }
    std::printf("[ldt] read %zu LDT tours\n", tours.size());
    return tours;
}

// 6-class LDT VOT segment (R step 6 / step 9 non-separated form).
std::string ldt_vot(const std::string& type, double trVOT, double lo, double hi) {
    if (type == "EI") {
        if (trVOT <= lo) return "LDT_Vis_Low";
        if (trVOT <= hi) return "LDT_Vis_Med";
        return "LDT_Vis_Hig";
    }
    if (trVOT <= lo) return "LDT_Res_Low";
    if (trVOT <= hi) return "LDT_Res_Med";
    return "LDT_Res_Hig";
}

LdtTrip to_trip(const Tour& t) {
    LdtTrip x;
    x.otaz = t.otaz; x.dtaz = t.dtaz; x.period = t.period;
    x.trMode = t.trMode; x.type = t.type; x.vot = t.vot;
    x.trVOT = t.trVOT; x.trPurpose = t.trPurpose; x.hhId = t.hhId;
    x.trOState = t.trOState; x.hhIncome = t.hhIncome; x.trPartySize = t.trPartySize;
    x.org_DMA = t.org_DMA; x.des_DMA = t.des_DMA;
    return x;
}

} // namespace

std::vector<LdtTrip> run_stage_ldt(const Settings& s, const Lookups& lk, Rng& rng) {
    std::vector<Tour> tours = read_ldt_tours(s);

    // --- 1. Append DMAs, code II/IE/EI/EE, intra-DMA air -> auto ---
    for (auto& t : tours) {
        t.des_DMA = lk.dma_for(t.trDZone);
        t.org_DMA = lk.dma_for(t.trOZone);
        bool oFL = (t.trOState == 12), dFL = (t.trDState == 12);
        t.type = oFL && dFL ? "II" : oFL && !dFL ? "IE" : !oFL && dFL ? "EI" : "EE";
        if (t.trMode == 4 && t.org_DMA == t.des_DMA) t.trMode = 1;  // intra-DMA air->auto
        // 2. init assignment fields
        t.otaz = t.trOZone; t.dtaz = t.trDZone;
        t.entry_ext = t.exit_ext = t.entry_airpt = t.exit_airpt = 0;
        t.accMode = t.egrMode = t.tripMode = "Auto";
    }

    const WeightedChoices& ext0 = lk.ext_auto_by_cbm.at(0);  // CBM==0 externals
    auto ext0_disc = rng.make_disc(ext0.probs);

    // --- 0. Cape Canaveral cruise trips (template = first CRUISE_TRIPS tours) ---
    std::vector<Tour> cruise;
    {
        int n = std::min<int>(CRUISE_TRIPS, (int)tours.size());
        auto cdisc = rng.make_disc(lk.cruise.probs);
        for (int i = 0; i < n; ++i) {
            Tour t = tours[i];
            t.trOZone = lk.cruise.values[rng.draw(cdisc)];
            t.trDZone = TAZ_PORT;
            t.des_DMA = 5; t.org_DMA = 4; t.trMode = 1;
            t.otaz = t.trOZone; t.dtaz = t.trDZone;
            cruise.push_back(std::move(t));
        }
    }

    // --- a) EI / IE auto external stations ---
    std::vector<Tour> ei_auto, ie_auto, cbm_auto;
    std::map<int, size_t> unmapped_states;
    for (auto& t : tours) {
        if (t.trMode == 1 && t.type == "EI") {
            if (s.apply_originState_based_externals) {
                // Entry station by origin state (zone rows override), from
                // OutofState_to_ExternalStations_Mapping.csv. A state missing
                // from the table enters on I-95, as the R default did.
                t.entry_ext = lk.os_entry_station(t.trOState, t.trOZone);
                if (t.entry_ext == 0) {
                    ++unmapped_states[t.trOState];
                    t.entry_ext = s.ext_station_i95;
                }
            } else {
                t.entry_ext = ext0.values[rng.draw(ext0_disc)];
            }
            ei_auto.push_back(t);
        } else if (t.trMode == 1 && t.type == "IE") {
            bool crossborder = (t.org_DMA < 4 && t.des_DMA == 10);
            if (!crossborder) {
                t.exit_ext = ext0.values[rng.draw(ext0_disc)];
                ie_auto.push_back(t);
            } else {
                // IE crossborder (North FL -> GA/AL)
                if (s.use_cbm_external_lookup) {
                    auto it = lk.cbm_ext.find(t.otaz);
                    t.exit_ext = it == lk.cbm_ext.end() ? 0 : it->second;
                } else {
                    // Fallback: CBM group share sampling (approx of R else-branch).
                    auto git = lk.ext_auto_by_cbm.find(t.org_DMA);
                    if (git == lk.ext_auto_by_cbm.end()) git = lk.ext_auto_by_cbm.find(2);
                    if (git != lk.ext_auto_by_cbm.end() && !git->second.empty()) {
                        auto d = rng.make_disc(git->second.probs);
                        t.exit_ext = git->second.values[rng.draw(d)];
                    }
                }
                cbm_auto.push_back(t);
            }
        }
    }

    for (const auto& kv : unmapped_states)
        std::printf("[ldt] WARNING: origin state %d not in os_state_externals -> I-95 (%zu tours)\n",
                    kv.first, kv.second);
    {
        std::map<long long, size_t> by_station;
        for (const auto& t : ei_auto) ++by_station[t.entry_ext];
        for (const auto& kv : by_station)
            std::printf("[ldt] EI auto entry station %lld: %zu tours\n", kv.first, kv.second);
    }

    // --- 3. Airport TAZ + access/egress mode for air tours, by DMA 1..9 ---
    std::vector<Tour> ei_air, ie_air, ii_air;
    for (const auto& t : tours) {
        if (t.trMode != 4) continue;
        if (t.type == "EI") ei_air.push_back(t);
        else if (t.type == "IE") ie_air.push_back(t);
        else if (t.type == "II") ii_air.push_back(t);
    }
    const std::vector<std::string> air_mode_lab = {"Auto", "DME", "Other"};
    const std::vector<double> air_mode_share = {0.815, 0.183, 0.02};
    auto mode_disc = rng.make_disc(air_mode_share);

    auto assign_airport = [&](std::vector<Tour>& v, bool at_dest) {
        for (int d = 1; d <= 9; ++d) {
            auto it = lk.airport_by_dma.find(d);
            if (it == lk.airport_by_dma.end() || it->second.empty()) continue;
            auto adisc = rng.make_disc(it->second.probs);
            const auto& vals = it->second.values;
            for (auto& t : v) {
                bool match = at_dest ? (t.des_DMA == d && t.org_DMA != d)
                                     : (t.org_DMA == d && t.des_DMA != d);
                if (!match) continue;
                long long apt = vals[rng.draw(adisc)];
                std::string m = air_mode_lab[rng.draw(mode_disc)];
                if (at_dest) { t.exit_airpt = apt; t.egrMode = m; }
                else { t.entry_airpt = apt; t.accMode = m; }
            }
        }
        // DME only valid at OIA and only for Disney resort zones.
        for (auto& t : v) {
            if (at_dest) {
                if (t.exit_airpt != OIA && t.egrMode == "DME") t.egrMode = "Auto";
                if (t.exit_airpt == OIA && !kDisneyZones.count(t.trDZone) && t.egrMode == "DME")
                    t.egrMode = "Auto";
            } else {
                if (t.entry_airpt != OIA && t.accMode == "DME") t.accMode = "Auto";
                if (t.entry_airpt == OIA && !kDisneyZones.count(t.trOZone) && t.accMode == "DME")
                    t.accMode = "Auto";
            }
            if (t.des_DMA == t.org_DMA) t.trMode = 1;  // same-DMA -> auto
        }
    };
    assign_airport(ei_air, true);    // visitor: pick airport at destination (egress)
    assign_airport(ie_air, false);   // resident: pick airport at origin (access)
    assign_airport(ii_air, true);
    assign_airport(ii_air, false);

    // --- 5. Consolidate tours across markets into dt_tours3 ---
    std::vector<Tour> ii_auto;
    for (const auto& t : tours)
        if (t.trMode == 1 && t.type == "II") ii_auto.push_back(t);

    std::vector<Tour> out;
    out.reserve(tours.size() * 2);
    auto add = [&](std::vector<Tour>& v) { for (auto& t : v) out.push_back(t); };

    add(ii_auto);
    for (auto& t : ei_auto) { t.otaz = t.entry_ext; out.push_back(t); }
    for (auto& t : ie_auto) { t.dtaz = t.exit_ext; out.push_back(t); }
    for (auto& t : cbm_auto) { if (t.type == "IE") t.dtaz = t.exit_ext; out.push_back(t); }
    for (auto& t : ie_air) { t.dtaz = t.entry_airpt; t.tripMode = t.accMode; out.push_back(t); }
    for (auto& t : ei_air) { t.otaz = t.exit_airpt; t.tripMode = t.egrMode; out.push_back(t); }
    // II air -> two legs (access + egress).
    for (auto& t : ii_air) {
        Tour acc = t; acc.dtaz = t.entry_airpt; acc.tripMode = t.accMode; out.push_back(acc);
        Tour egr = t; egr.otaz = t.exit_airpt; egr.tripMode = t.egrMode; out.push_back(egr);
    }
    add(cruise);

    // --- 6. VOT classes (LDT thresholds) ---
    for (auto& t : out) t.vot = ldt_vot(t.type, t.trVOT, s.vot_ldt_low, s.vot_ldt_high);

    // Optional: write the consolidated tour list (parity artifact).
    // (Skipped large per-row CSV here; the gz trip list is the deliverable.)

    // --- 7. Reverse direction: append flipped trips ---
    std::vector<LdtTrip> both;
    both.reserve(out.size() * 2);
    for (const auto& t : out) {
        both.push_back(to_trip(t));         // tour_dir
        Tour r = t;                          // return_dir: swap O/D and DMA
        std::swap(r.otaz, r.dtaz);
        std::swap(r.org_DMA, r.des_DMA);
        both.push_back(to_trip(r));
    }

    // --- 8. Time of day: sample 15-min segment by market (Res_Long / Vis_Long) ---
    auto res_disc = rng.make_disc(lk.tod.res_long);
    auto vis_disc = rng.make_disc(lk.tod.vis_long);
    for (auto& t : both) {
        size_t k = (t.type == "EI") ? rng.draw(vis_disc) : rng.draw(res_disc);
        t.period = lk.tod.seg96[k];
    }

    // --- 9. External filters ---
    if (s.exclude_non_Interstate) {
        static const std::unordered_set<long long> inter = {11560, 11548, 11504, 11558,
                                                            11542, 11540, 11538, 11529,
                                                            11516, 11509};
        both.erase(std::remove_if(both.begin(), both.end(), [&](const LdtTrip& t) {
                       return !(inter.count(t.otaz) || inter.count(t.dtaz));
                   }), both.end());
    }
    if (s.remove_NorthFL_res_trips) {
        auto nfl = [](int d) { return d == 1 || d == 2 || d == 3 || d == 10; };
        both.erase(std::remove_if(both.begin(), both.end(), [&](const LdtTrip& t) {
                       return nfl(t.org_DMA) && nfl(t.des_DMA);
                   }), both.end());
    }

    std::printf("[ldt] produced %zu LDT trips (both directions)\n", both.size());
    return both;
}

} // namespace ap
