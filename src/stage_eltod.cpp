#include "stage_eltod.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "util/csv.h"
#include "util/gz_io.h"

namespace ap {

namespace {

constexpr long long MIN_HHID = 15000000, MIN_TRK_HHID = 13000000;

// External-station scale: factor applied to OD trips that touch an external zone.
using ScaleFn = std::function<double(long long, long long)>;

// 15-min segment (1..96) -> clock hour (1..24, 24 = midnight) as in R.
int hour_from_seg(int seg) { int h = (seg * 15) / 60; return h == 0 ? 24 : h; }

std::string strip_vot_suffix(const std::string& m) {
    for (const char* suf : {"_Med", "_Low", "_Hig"}) {
        size_t n = std::string(suf).size();
        if (m.size() >= n && m.compare(m.size() - n, n, suf) == 0)
            return m.substr(0, m.size() - n);
    }
    return m;
}

// Accumulates an OD trip table: (hour, O, D) -> segment -> summed vehTrips.
struct ODTable {
    std::map<std::tuple<int, long long, long long>, std::map<std::string, double>> cells;
    std::set<std::string> segments;
    void add(int hour, long long o, long long d, const std::string& seg, double w) {
        cells[{hour, o, d}][seg] += w;
        segments.insert(seg);
    }
    void apply_scale(const ScaleFn& factor) {
        for (auto& kv : cells) {
            long long o = std::get<1>(kv.first), d = std::get<2>(kv.first);
            double f = factor(o, d);
            if (f != 1.0) for (auto& sc : kv.second) sc.second *= f;
        }
    }
    void write_csv(const std::string& path, bool hourclock) const {
        std::ofstream out(path);
        out << (hourclock ? "STARTTIME" : "period") << ",O,D";
        for (const auto& seg : segments) out << "," << seg;
        out << "\n";
        for (const auto& kv : cells) {
            int hr = std::get<0>(kv.first);
            if (hourclock) { char b[8]; std::snprintf(b, sizeof b, "%02d:00", hr); out << b; }
            else out << hr;
            out << "," << std::get<1>(kv.first) << "," << std::get<2>(kv.first);
            for (const auto& seg : segments) {
                auto it = kv.second.find(seg);
                out << "," << (it == kv.second.end() ? 0.0 : it->second);
            }
            out << "\n";
        }
    }
};

// SDT writes resident and visitor trip modes with DIFFERENT 1-based codings
// (see SDTModel OutputWriter.cpp). RESIDENT uses the Java-UEC map
//   1=DA_FREE 2=S2GP 3=S3GP 4=DA_PAY 5=S2PAY 6=S3PAY  -> 1,4=DA 2,5=SR2 3,6=SR3
// VISITOR uses sequential cpp+1
//   1=DA_FREE 2=DA_PAY 3=S2GP 4=S2PAY 5=S3GP 6=S3PAY   -> 1,2=DA 3,4=SR2 5,6=SR3
// Applying the visitor coding to resident trips mis-assigns occupancy and inflates
// vehicle trips, so occupancy must follow the file's own coding.
double occupancy_for(int tripMode, bool resident) {
    if (resident) {
        if (tripMode == 2 || tripMode == 5) return 2.0;   // SR2
        if (tripMode == 3 || tripMode == 6) return 3.2;   // SR3
        return 1.0;                                        // DA (1,4)
    }
    if (tripMode == 3 || tripMode == 4) return 2.0;        // SR2
    if (tripMode == 5 || tripMode == 6) return 3.2;        // SR3
    return 1.0;                                            // DA (1,2)
}

std::string vot_class(double v, double lo, double hi, const char* base) {
    std::string b = base;
    if (v <= lo) return b + "_Low";
    if (v <= hi) return b + "_Med";
    return b + "_Hig";
}

// Read an SDT trip list (resident or visitor) and emit list rows + OD entries.
struct SdtCols { int oTaz, dTaz, period, vot, mode, exp, dPurp, oPurp, hh, per, tour, trip; };

// One external-station target row from ldt_external_targets.csv.
struct ExtTarget {
    double base_count = 0;   // base-year count (for growth-rate specs)
    bool has_base = false;
    std::string spec;        // absolute count, or growth rate like "1.2%"
};

bool file_exists(const std::string& p) { std::ifstream f(p); return f.good(); }

std::string trim_ws(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

// Load the GUI external-targets file: ext_zone_id -> {base_count, spec}.
// base-count column is matched by prefix "base_count" (e.g. base_count_2024).
std::unordered_map<long long, ExtTarget> load_ext_targets(const std::string& path) {
    CsvTable t = load_csv(path);
    int cZone = t.require("ext_zone_id", path);
    int cSpec = t.require("future_target_or_growth", path);
    int cBase = -1;
    for (int i = 0; i < (int)t.header.names.size(); ++i)
        if (t.header.names[i].rfind("base_count", 0) == 0) { cBase = i; break; }
    std::unordered_map<long long, ExtTarget> m;
    for (size_t r = 0; r < t.size(); ++r) {
        ExtTarget e;
        if (cBase >= 0) { e.base_count = t.num(r, cBase); e.has_base = true; }
        e.spec = trim_ws(t.at(r, cSpec));
        m[t.ll(r, cZone)] = e;
    }
    return m;
}

// Resolve a target volume: absolute count, or base*(1 + rate/100 * years) for a
// growth-rate spec (linear annual growth, the model's convention).
double resolve_target(const ExtTarget& e, int year, int base_year) {
    if (!e.spec.empty() && e.spec.back() == '%') {
        double rate = std::atof(e.spec.substr(0, e.spec.size() - 1).c_str());
        int years = year - base_year;
        return e.base_count * (1.0 + rate / 100.0 * years);
    }
    return std::atof(e.spec.c_str());
}

} // namespace

void run_stage_eltod(const Settings& s, const Lookups& lk, Rng& rng,
                     const std::vector<LdtTrip>& ldt_trips) {
    if (s.LimitCounties)
        throw std::runtime_error("LimitCounties is not supported in the C++ port "
                                 "(needs hhID_by_county.csv); set LimitCounties=false");

    std::vector<ListRow> list;
    ODTable od;             // combined ELToD OD table (by hour)
    ODTable sdt_res_od;     // SDT resident purpose*VOT OD table (by hour)

    const std::string res_file = s.scen(s.sdt_res_trips, "trips_" + std::to_string(s.feedback_loop) + ".csv");
    const std::string vis_file = s.scen(s.sdt_vis_trips, "visitorTrips.csv");

    // ---------------- SDT visitors & residents ----------------
    auto purpose_res = [](int dp, int op) -> std::string {
        int p = dp >= 0 ? dp : op;
        switch (p) {
            case 0: return "Work"; case 1: return "University"; case 2: return "School";
            case 3: return "Escort"; case 4: return "Maintenance"; case 5: return "Discretionary";
            case 6: return "AtWork"; default: return "None";
        }
    };
    auto purpose_vis = [](int dp, int op) -> std::string {
        int p = dp >= 0 ? dp : op;
        switch (p) {
            case 0: return "WorkVis"; case 1: return "Recreate"; case 2: return "Shop";
            case 3: return "Eatout"; default: return "None";
        }
    };

    // Bring an SDT period onto the internal 15-min segment grid (1..96) per the
    // declared SDT native resolution. SDT trip lists store 0-BASED slots
    // (0 = midnight bin; settings.h "periods 0..47", SDTModel OutputWriter):
    // 30-min slot p covers clock [p*30, (p+1)*30) and maps to segments
    // 2p+1 / 2p+2. Out-of-range values fall back to uniform over the day.
    // (The previous 1-based read shifted every SDT trip 30 minutes early and
    // scattered the midnight bin uniformly across the day.)
    auto recode_seg = [&](int period) -> int {
        if (s.sdt_input_resolution == 15)
            return (period >= 0 && period <= 95) ? period + 1 : rng.uniform_int(1, 96);
        if (period >= 0 && period <= 47)
            return rng.uniform_int(0, 1) ? 2 * period + 2 : 2 * period + 1;
        return rng.uniform_int(1, 96);  // corrupt/unknown: uniform over the day
    };

    auto process_sdt = [&](const std::string& file, bool resident) {
        GzLineReader rd(file);
        if (!rd.good()) throw std::runtime_error("cannot open SDT trips: " + file);
        std::string_view line; rd.next_line(line);
        CsvHeader h; h.parse(line);
        // SDT C++ writes resident trips in UPPER_SNAKE (ORIG_TAZ, MODE, TRIP_WEIGHT,
        // single PURPOSE = destination purpose) and visitor trips in the camelCase
        // schema. Accept either spelling so one reader handles both files.
        auto req2 = [&](const char* a, const char* b) -> int {
            int c = h.col(a); if (c < 0) c = h.col(b);
            if (c < 0) throw std::runtime_error(file + ": missing column '" + a + "'/'" + b + "'");
            return c;
        };
        int cO = req2("originTaz", "ORIG_TAZ"), cD = req2("destinationTaz", "DEST_TAZ");
        int cPeriod = req2("period", "DEPART_PERIOD"), cVot = req2("valueOfTime", "VALUE_OF_TIME");
        int cMode = req2("tripMode", "MODE"), cExp = req2("expansionFactor", "TRIP_WEIGHT");
        // visitor: separate origin/dest purpose; resident: single PURPOSE = dest
        // purpose (purpose_res uses dp when >=0, so originPurpose may be absent).
        int cDP = h.col("destinationPurpose"); if (cDP < 0) cDP = h.col("PURPOSE");
        if (cDP < 0) throw std::runtime_error(file + ": missing column 'destinationPurpose'/'PURPOSE'");
        int cOP = h.col("originPurpose");
        int cHh = h.col("hh_id");   if (cHh < 0)   cHh = h.col("HHID");
        int cPer = h.col("person_id"); if (cPer < 0) cPer = h.col("PERID");
        int cTour = h.col("tour_id");  if (cTour < 0) cTour = h.col("TOURID");
        int cTrip = h.col("trip_id");  if (cTrip < 0) cTrip = h.col("TRIPID");
        std::vector<std::string_view> f;
        long long vis_counter = 0;
        double veh_total = 0;       // vehicle-trips emitted from this file (for QA count)
        const double lo = s.vot_sdt_low, hi = s.vot_sdt_high;
        while (rd.next_line(line)) {
            if (line.empty()) continue;
            split_csv(line, f);
            auto G = [&](int c) -> std::string_view { return c >= 0 && c < (int)f.size() ? f[c] : std::string_view(); };
            int mode = (int)to_ll(G(cMode));
            if (mode > 6) continue;                 // auto modes only (tripMode <= 6)
            double vt = to_double(G(cVot));
            int dp = (int)to_ll(G(cDP)), op = (int)to_ll(G(cOP));
            std::string purpose = resident ? purpose_res(dp, op) : purpose_vis(dp, op);
            std::string seg = resident ? vot_class(vt, lo, hi, "SDT_Res")
                                       : vot_class(vt, lo, hi, "SDT_Vis");
            double occ = occupancy_for(mode, resident);
            double veh = to_double(G(cExp)) / occ;
            veh_total += veh;
            int period = (int)to_ll(G(cPeriod));
            int s15 = recode_seg(period);
            int hr = hour_from_seg(s15);
            long long o = to_ll(G(cO)), d = to_ll(G(cD));

            od.add(hr, o, d, seg, veh);

            ListRow r;
            r.O = o; r.D = d; r.valueOfTime = (double)(long long)vt;
            r.purpose = purpose; r.marketVot = seg; r.market = strip_vot_suffix(seg);
            r.vehTrips = veh; r.occupancy = occ; r.period = s.period_out(s15);
            if (resident) {
                r.hh_id = to_ll(G(cHh)); r.person_id = to_ll(G(cPer));
                r.tour_id = to_ll(G(cTour)); r.trip_id = to_ll(G(cTrip));
                // SDT resident purpose*VOT OD (output_SDT_Res_hourly)
                std::string pv = vot_class(vt, lo, hi, purpose.c_str());
                sdt_res_od.add(hr, o, d, pv, veh);
            } else {
                r.hh_id = MIN_HHID + (++vis_counter);
                r.person_id = 1;
                r.tour_id = to_ll(G(cTour)) - 1000000;
                r.trip_id = to_ll(G(cTrip));
            }
            list.push_back(std::move(r));
        }
        std::printf("[eltod] processed SDT %s: %.0f vehicle-trips\n",
                    resident ? "residents" : "visitors", veh_total);
    };

    process_sdt(vis_file, false);
    process_sdt(res_file, true);

    // ---------------- LDT (from stage A, in memory) ----------------
    {
        bool need_skim = s.track_AirTours || s.track_sdt_grt50M;
        if (need_skim) const_cast<Lookups&>(lk).load_skim(s);
        auto purpose_ldt = [](int p) -> std::string {
            switch (p) {
                case 1: return "PersonalBusiness"; case 2: return "VistFriendFamily";
                case 3: return "LeisureVacation"; case 4: return "CrossBorderCommute";
                case 5: return "EmployerBusiness"; default: return "None";
            }
        };
        long long ext_counter = 0;
        for (const auto& t : ldt_trips) {
            int trPurpose = t.trPurpose;
            // crossborder (DMA 10 <-> internal) employer-business -> commute
            bool cb = (t.org_DMA == 10 && t.des_DMA < 10) || (t.org_DMA < 10 && t.des_DMA == 10);
            if (cb && trPurpose == 5) trPurpose = 4;
            std::string purpose = purpose_ldt(trPurpose);

            std::string seg = t.vot;  // 6-class from stage A
            if (s.track_AirTours && t.trMode == 4) {
                seg = (t.type == "EI") ? "LDT_Vis_Air" : "LDT_Res_Air";
                double dist = lk.dist(t.otaz, t.dtaz);
                if (dist > 25.0) seg = "LDT_Air_AccEgr25M";
            }
            int s15 = t.period;                    // 15-min seg from stage A
            int hr = hour_from_seg(s15);
            od.add(hr, t.otaz, t.dtaz, seg, 1.0);  // each tour = 1 vehicle trip

            ListRow r;
            r.O = t.otaz; r.D = t.dtaz; r.valueOfTime = (double)(long long)t.trVOT;
            r.purpose = purpose; r.marketVot = seg;
            r.market = (seg == "LDT_Air_AccEgr25M" || seg == "LDT_Vis_Air" || seg == "LDT_Res_Air")
                           ? "LDT_Air" : strip_vot_suffix(seg);
            r.vehTrips = 1.0; r.occupancy = t.trPartySize; r.period = s.period_out(s15);
            r.hh_id = (t.trOState != 12) ? (MIN_HHID + (++ext_counter)) : t.hhId;
            r.person_id = 1; r.tour_id = 1; r.trip_id = 1;
            r.hhIncome = t.hhIncome; r.has_income = true;
            list.push_back(std::move(r));
        }
        std::printf("[eltod] processed %zu LDT trips\n", ldt_trips.size());
    }

    // ---------------- Trucks ----------------
    {
        CsvTable tk = load_csv(s.truck_odme);
        int cO = tk.require("O", s.truck_odme), cD = tk.require("D", s.truck_odme);
        int cH = tk.require("heavy", s.truck_odme), cL = tk.require("light", s.truck_odme);
        int cM = tk.require("medium", s.truck_odme);
        double trk_scale = 1.0 + (double)(s.yy() - s.truck_base_year) / 100.0;
        if (trk_scale < 1.0) trk_scale = 1.0;  // R only scales up

        struct TruckClass { const char* name; int col; const std::vector<double>* prob; };
        std::vector<TruckClass> classes = {
            {"heavy", cH, &lk.tod.ax5p}, {"medium", cM, &lk.tod.ax4}, {"light", cL, &lk.tod.ax3}};
        // truck VOT distribution params (R): heavy, medium, light
        const std::map<std::string, std::array<double, 3>> tparam = {
            // {sd, cost_coef, time_coef}
            {"heavy", {0.650, 2.88, 0.9}}, {"medium", {0.692, 5.42, 0.9}}, {"light", {0.896, 7.5, 0.7}}};

        long long trk_counter = 0;
        for (auto& c : classes) {
            auto disc = rng.make_disc(*c.prob);
            // Build the VOT pool for this class once (R seq(0,1,len=n) -> qnorm).
            // n here is the number of nonzero O-D cells for the class.
            std::vector<size_t> idx;
            for (size_t r = 0; r < tk.size(); ++r) if (tk.num(r, c.col) > 0) idx.push_back(r);
            size_t n = idx.size();
            const auto& tp = tparam.at(c.name);
            std::vector<double> vot_pool;
            vot_pool.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                double x = (n <= 1) ? 0.5 : (double)i / (double)(n - 1);
                double y = qnorm(x, tp[2], tp[0]);  // mean=time_coef, sd
                if (y > 0 && std::isfinite(y))
                    vot_pool.push_back(y * 60.0 * s.trk_axle_scale / tp[1]);
            }
            std::uniform_int_distribution<size_t> votpick(0, vot_pool.empty() ? 0 : vot_pool.size() - 1);

            for (size_t r : idx) {
                double veh = tk.num(r, c.col) * trk_scale;
                long long o = tk.ll(r, cO), d = tk.ll(r, cD);
                int s15 = lk.tod.seg96[rng.draw(disc)];
                int hr = hour_from_seg(s15);
                od.add(hr, o, d, c.name, veh);

                ListRow row;
                row.O = o; row.D = d; row.purpose = c.name;
                row.marketVot = c.name; row.market = "Truck";
                row.vehTrips = veh; row.occupancy = veh;  // R: occupancy := vehTrips for trucks
                row.period = s.period_out(s15);
                row.valueOfTime = vot_pool.empty() ? 0.0
                                 : (double)(long long)vot_pool[votpick(rng.engine())];
                row.hh_id = MIN_TRK_HHID + (++trk_counter);
                row.person_id = 1; row.tour_id = 1; row.trip_id = 1;
                list.push_back(std::move(row));
            }
        }
        std::printf("[eltod] processed trucks (scale=%.4f)\n", trk_scale);
    }

    // ---------------- External-station target calibration ----------------
    // Scale all OD trips serving each external zone so the zone's modeled volume
    // hits the target (count or growth-rate) from ldt_external_targets.csv.
    // Replaces the legacy hardcoded I95/I75/I10 scale constants.
    const std::string ext_file = s.scen(s.ldt_external_targets, "ldt_external_targets.csv");
    if (s.apply_external_targets && file_exists(ext_file)) {
        auto targets = load_ext_targets(ext_file);
        // Optional base-count override (a plugin-config base-year count table).
        if (!s.external_base_counts.empty() && file_exists(s.external_base_counts)) {
            CsvTable b = load_csv(s.external_base_counts);
            int bz = b.require("ext_zone_id", s.external_base_counts);
            int bc = -1;
            for (int i = 0; i < (int)b.header.names.size(); ++i)
                if (b.header.names[i].rfind("base_count", 0) == 0) { bc = i; break; }
            if (bc >= 0)
                for (size_t r = 0; r < b.size(); ++r) {
                    auto it = targets.find(b.ll(r, bz));
                    if (it != targets.end()) { it->second.base_count = b.num(r, bc); it->second.has_base = true; }
                }
        }
        // Modeled volume per external zone, from the unscaled trip list.
        std::unordered_map<long long, double> modeled;
        for (const auto& r : list) {
            if (targets.count(r.O)) modeled[r.O] += r.vehTrips;
            if (r.D != r.O && targets.count(r.D)) modeled[r.D] += r.vehTrips;
        }
        // Per-zone scale = target / modeled.
        std::unordered_map<long long, double> scale;
        for (const auto& kv : targets) {
            double tgt = resolve_target(kv.second, s.year, s.external_base_year);
            double mod = modeled.count(kv.first) ? modeled[kv.first] : 0.0;
            double sc = mod > 0 ? tgt / mod : 1.0;
            scale[kv.first] = sc;
            std::printf("[eltod] external %lld: target=%.0f modeled=%.0f scale=%.4f%s\n",
                        kv.first, tgt, mod, sc, mod > 0 ? "" : "  (no modeled trips!)");
        }
        ScaleFn factor = [scale](long long o, long long d) {
            double f = 1.0;
            auto io = scale.find(o); if (io != scale.end()) f *= io->second;
            if (d != o) { auto id = scale.find(d); if (id != scale.end()) f *= id->second; }
            return f;
        };
        for (auto& r : list) { double f = factor(r.O, r.D); if (f != 1.0) r.vehTrips *= f; }
        od.apply_scale(factor);
    } else {
        std::printf("[eltod] external target scaling skipped (%s)\n",
                    s.apply_external_targets ? ("not found: " + ext_file).c_str() : "disabled");
    }

    // ---------------- Merge SDT household income onto resident rows ----------------
    if (!s.sdt_syn_hh.empty()) {
        CsvTable hh = load_csv(s.sdt_syn_hh);
        int cId = hh.require("household_id", s.sdt_syn_hh);
        int cInc = hh.require("HHINCADJ", s.sdt_syn_hh);
        std::unordered_map<long long, double> inc;
        for (size_t r = 0; r < hh.size(); ++r) inc[hh.ll(r, cId)] = hh.num(r, cInc);
        for (auto& r : list) {
            // residents are the rows whose market begins with "SDT_Res"
            if (r.market.rfind("SDT_Res", 0) == 0) {
                auto it = inc.find(r.hh_id);
                if (it != inc.end()) { r.hhIncome = it->second; r.has_income = true; }
            }
        }
    }

    // ---------------- Outputs ----------------
    // Hourly OD trip table (wide by market/VOT, STARTTIME HH:00) — matches the
    // original R 3_get_ELTOD output. Always aggregated to the clock hour,
    // independent of the trip-list output_resolution.
    if (s.write_hourly_table) {
        const std::string out_od = s.scen(s.hourly_table_out, "ELTOD_tt_HourClock.csv");
        od.write_csv(out_od, s.write_HourClock_format);
        std::printf("[eltod] wrote hourly OD trip table -> %s\n", out_od.c_str());
    }

    const std::string out_sdt = s.scen("", "ELTOD_SDT_Res_hourly.csv");
    sdt_res_od.write_csv(out_sdt, false);
    std::printf("[eltod] wrote SDT resident OD -> %s\n", out_sdt.c_str());

    // Hydra-schema gzipped trip list.
    const std::string out_list = s.scen(s.trip_table_out, "ELTOD_tt_List_hourly.csv.gz");
    GzWriter gz(out_list);
    if (!gz.good()) throw std::runtime_error("cannot open output: " + out_list);
    // `depart_time` is an HH:MM:SS clock (start of the period bin) — Hydra's
    // tsm_trip_reader treats this as its preferred clock departure field.
    gz.line("hh_id,person_id,tour_id,trip_id,valueOfTime,purpose,depart_time,O,D,"
            "marketVot,vehTrips,occupancy,hhIncome,market");
    std::string buf;
    buf.reserve(160);
    char num[64];
    for (const auto& r : list) {
        buf.clear();
        buf += std::to_string(r.hh_id); buf += ',';
        buf += std::to_string(r.person_id); buf += ',';
        buf += std::to_string(r.tour_id); buf += ',';
        buf += std::to_string(r.trip_id); buf += ',';
        std::snprintf(num, sizeof num, "%g", r.valueOfTime); buf += num; buf += ',';
        buf += r.purpose; buf += ',';
        {   // period bin index -> HH:MM:SS clock at the start of the bin
            int mins = r.period * s.output_resolution;
            std::snprintf(num, sizeof num, "%02d:%02d:%02d", mins / 60, mins % 60, 0);
            buf += num;
        }
        buf += ',';
        buf += std::to_string(r.O); buf += ',';
        buf += std::to_string(r.D); buf += ',';
        buf += r.marketVot; buf += ',';
        std::snprintf(num, sizeof num, "%g", r.vehTrips); buf += num; buf += ',';
        std::snprintf(num, sizeof num, "%g", r.occupancy); buf += num; buf += ',';
        if (r.has_income) { std::snprintf(num, sizeof num, "%g", r.hhIncome); buf += num; }
        buf += ',';
        buf += r.market;
        gz.line(buf);
    }
    gz.finish();
    std::printf("[eltod] wrote trip list (%zu rows) -> %s\n", list.size(), out_list.c_str());
}

} // namespace ap
