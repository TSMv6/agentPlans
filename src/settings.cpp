#include "settings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ap {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool to_bool(const std::string& v) {
    std::string s;
    for (char c : v) s += static_cast<char>(std::tolower(c));
    return s == "1" || s == "true" || s == "yes" || s == "t";
}
std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ';')) {
        std::string t = trim(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}
} // namespace

std::string Settings::scen(const std::string& override_val, const std::string& fname) const {
    if (!override_val.empty()) return override_val;
    return scenario_dir + "/" + fname;
}

Settings Settings::load(const std::string& control_file) {
    std::ifstream in(control_file);
    if (!in) throw std::runtime_error("cannot open control file: " + control_file);

    std::unordered_map<std::string, std::string> kv;
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    auto S = [&](const char* k, std::string& dst) { auto it = kv.find(k); if (it != kv.end()) dst = it->second; };
    auto B = [&](const char* k, bool& dst) { auto it = kv.find(k); if (it != kv.end()) dst = to_bool(it->second); };
    auto I = [&](const char* k, int& dst) { auto it = kv.find(k); if (it != kv.end()) dst = std::atoi(it->second.c_str()); };
    auto D = [&](const char* k, double& dst) { auto it = kv.find(k); if (it != kv.end()) dst = std::atof(it->second.c_str()); };
    auto U = [&](const char* k, uint64_t& dst) { auto it = kv.find(k); if (it != kv.end()) dst = std::strtoull(it->second.c_str(), nullptr, 10); };
    auto LL = [&](const char* k, long long& dst) { auto it = kv.find(k); if (it != kv.end()) dst = std::strtoll(it->second.c_str(), nullptr, 10); };

    Settings s;
    S("catalog_dir", s.catalog_dir);
    S("scenario_dir", s.scenario_dir);
    I("year", s.year);
    I("feedback_loop", s.feedback_loop);
    U("seed", s.seed);

    // Resolution settings accept an integer in minutes or the word "daily" (=0).
    auto RES = [&](const char* k, int& dst) {
        auto it = kv.find(k);
        if (it == kv.end()) return;
        std::string v;
        for (char c : it->second) v += static_cast<char>(std::tolower(c));
        dst = (v == "daily") ? 0 : std::atoi(it->second.c_str());
    };
    RES("output_resolution", s.output_resolution);
    RES("sdt_input_resolution", s.sdt_input_resolution);
    RES("ldt_input_resolution", s.ldt_input_resolution);
    RES("truck_input_resolution", s.truck_input_resolution);

    S("external_auto_shares", s.external_auto_shares);
    S("airport_shares", s.airport_shares);
    S("canaveral_cruise", s.canaveral_cruise);
    S("tod_distributions", s.tod_distributions);
    S("ga_al_destinations", s.ga_al_destinations);
    S("cbm_external_lookup", s.cbm_external_lookup_csv);

    S("taz_dma", s.taz_dma);
    S("distance_skim", s.distance_skim);
    S("truck_odme", s.truck_odme);
    S("sdt_syn_hh", s.sdt_syn_hh);
    S("fl_ldt_tours", s.fl_ldt_tours);
    S("os_ldt_tours", s.os_ldt_tours);
    S("sdt_res_trips", s.sdt_res_trips);
    S("sdt_vis_trips", s.sdt_vis_trips);
    S("trip_table_out", s.trip_table_out);
    S("hourly_table_out", s.hourly_table_out);
    B("write_hourly_table", s.write_hourly_table);

    S("ldt_external_targets", s.ldt_external_targets);
    S("ldt_border_zones", s.ldt_border_zones);
    S("external_base_counts", s.external_base_counts);
    I("external_base_year", s.external_base_year);
    B("apply_external_targets", s.apply_external_targets);
    LL("ext_station_i10", s.ext_station_i10);
    LL("ext_station_i75", s.ext_station_i75);
    LL("ext_station_i95", s.ext_station_i95);

    B("apply_originState_based_externals", s.apply_originState_based_externals);
    B("use_cbm_external_lookup", s.use_cbm_external_lookup);
    B("separate_res_markets", s.separate_res_markets);
    B("exclude_non_Interstate", s.exclude_non_Interstate);
    B("remove_NorthFL_res_trips", s.remove_NorthFL_res_trips);
    B("fineCalib", s.fineCalib);
    B("compute_LDT_TT", s.compute_LDT_TT);

    B("track_sdt_grt50M", s.track_sdt_grt50M);
    B("track_AirTours", s.track_AirTours);
    B("appendNewMkt", s.appendNewMkt);
    B("aggregate_purp", s.aggregate_purp);
    B("telework", s.telework);
    B("LimitCounties", s.LimitCounties);
    B("write_HourClock_format", s.write_HourClock_format);
    B("use_hour", s.use_hour);
    B("doTransims", s.doTransims);

    D("vot_ldt_low", s.vot_ldt_low);
    D("vot_ldt_high", s.vot_ldt_high);
    D("vot_sdt_low", s.vot_sdt_low);
    D("vot_sdt_high", s.vot_sdt_high);
    I("truck_base_year", s.truck_base_year);
    D("trk_axle_scale", s.trk_axle_scale);

    auto it = kv.find("limit_counties");
    if (it != kv.end()) s.limit_counties = split_list(it->second);

    return s;
}

void Settings::validate() const {
    if (scenario_dir.empty()) throw std::runtime_error("control: scenario_dir is required");
    if (catalog_dir.empty()) throw std::runtime_error("control: catalog_dir is required");
    if (output_resolution != 15 && output_resolution != 30)
        throw std::runtime_error("control: output_resolution must be 15 or 30");
    if (sdt_input_resolution != 15 && sdt_input_resolution != 30)
        throw std::runtime_error("control: sdt_input_resolution must be 15 or 30");
}

} // namespace ap
