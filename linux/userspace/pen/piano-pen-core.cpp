// SPDX-License-Identifier: Apache-2.0
// Decoder adapted from ianchb/xiaomi-sheng-thp34046210932d654a4c0df0121ecc31c008f8148c.
// All Piano board calibration comes from the caller's actual external ini.
#include "piano-pen-core.h"
#include "piano-pen-decoder.hpp"
extern "C" {
#include "piano-pen-frame.h"
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Ini = std::map<std::string, std::string>;
using piano_pen::StylusProfile;
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}
int integer(std::string text) {
    text = trim(text);
    int value;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("invalid ini integer: " + text);
    return value;
}
Ini read_ini(const char *path) {
    if (!path) throw std::runtime_error("an actual ini path is required");
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open external ini");
    Ini result;
    std::string section, logical, line;
    size_t bytes = 0;
    while (std::getline(file, line)) {
        bytes += line.size();
        if (bytes > 1024 * 1024) throw std::runtime_error("ini exceeds1MiB");
        if (const auto comment = line.find('#'); comment != std::string::npos)
            line.resize(comment);
        line = trim(line);
        if (line.empty()) continue;
        const bool continuation = line.back() == '\\';
        if (continuation) line.pop_back();
        logical += line;
        if (continuation || (logical.find('{') != std::string::npos &&
                             logical.find('}') == std::string::npos)) continue;
        const auto equals = logical.find('=');
        if (equals == std::string::npos) section = trim(logical);
        else {
            const std::string key = section + "." + trim(logical.substr(0, equals));
            if (!result.emplace(key, trim(logical.substr(equals + 1))).second)
                throw std::runtime_error("duplicate ini key: " + key);
        }
        logical.clear();
    }
    if (!file.eof() || !logical.empty()) throw std::runtime_error("incomplete ini");
    return result;
}
const std::string &required(const Ini &ini, const std::string &key) {
    const auto found = ini.find(key);
    if (found == ini.end()) throw std::runtime_error("missing actual ini key: " + key);
    return found->second;
}
int value(const Ini &ini, const std::string &key) { return integer(required(ini, key)); }
std::vector<int> array(const Ini &ini, const std::string &key, size_t count) {
    std::string text = required(ini, key);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        throw std::runtime_error("invalid ini array: " + key);
    text = text.substr(1, text.size() - 2);
    std::vector<int> out;
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find(',', start);
        out.push_back(integer(text.substr(start, end == std::string::npos ? end : end - start)));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (out.size() != count) throw std::runtime_error("ini array size differs: " + key);
    return out;
}
struct Calibration { StylusProfile profile; piano_pen_info info; };
Calibration calibration(const Ini &ini, int vendor, piano_pen_orientation orientation) {
    if (orientation != PIANO_PEN_PORTRAIT || vendor != 3)
        throw std::runtime_error("this interface requires explicit portrait P81c/vendor3");
    if (value(ini, "hw.hal_row_num") != 40 || value(ini, "hw.hal_col_num") != 60 ||
        value(ini, "hw.hal_node_num") != 2400 || value(ini, "hw.display_x_resolution") != 2136 ||
        value(ini, "hw.display_y_resolution") != 3200 ||
        value(ini, "project_infor.super_resolution") != 100 ||
        value(ini, "hw.x_flip") || value(ini, "hw.y_flip") || value(ini, "hw.xy_flip") ||
        value(ini, "stylus.stylus_num_col1") != 12 || value(ini, "stylus.stylus_num_row1") != 40 ||
        value(ini, "stylus.stylus_num_col2") != 60 || value(ini, "stylus.stylus_num_row2") != 8 ||
        value(ini, "stylus.stylus_en") != 1)
        throw std::runtime_error("ini does not describe the supported Piano portrait geometry");
    // P81c id8 chooses vendor3. This is not the stylus_3 configuration slot.
    std::string prefix;
    for (const auto &candidate : {"stylus", "stylus_2", "stylus_3"}) {
        const auto found = ini.find("stylus." + std::string(candidate) + "_vendor_id");
        if (found != ini.end() && integer(found->second) == vendor) {
            if (!prefix.empty()) throw std::runtime_error("ambiguous pen vendor calibration");
            prefix = candidate;
        }
    }
    if (prefix.empty()) throw std::runtime_error("pen vendor calibration is absent");
    const auto profile_value = [&](const std::string &suffix) {
        const std::string key = "stylus." + prefix + "_" + suffix;
        return value(ini, ini.contains(key) ? key : "stylus.stylus_" + suffix);
    };
    Calibration out{};
    auto &p = out.profile;
    p.coordinate_resolution = value(ini, "project_infor.super_resolution");
    const auto mx = array(ini, "mapping.default_mapping_x", 2400);
    const auto my = array(ini, "mapping.default_mapping_y", 2400);
    for (size_t row = 0; row < 40; ++row) {
        p.mapping_40[row] = mx[row * 60];
        for (size_t col = 0; col < 60; ++col)
            if (mx[row * 60 + col] != p.mapping_40[row])
                throw std::runtime_error("actual mapping_x is not separable into40 rows");
    }
    for (size_t col = 0; col < 60; ++col) {
        p.mapping_60[col] = my[col];
        for (size_t row = 0; row < 40; ++row)
            if (my[row * 60 + col] != p.mapping_60[col])
                throw std::runtime_error("actual mapping_y is not separable into60 columns");
    }
    const auto valid_mapping = [](const auto &map, int extent) {
        return map.front() >= 0 && map.back() < extent &&
            std::adjacent_find(map.begin(), map.end(), std::greater_equal<int>()) == map.end();
    };
    if (!valid_mapping(p.mapping_40, 2136) || !valid_mapping(p.mapping_60, 3200))
        throw std::runtime_error("actual mapping is outside Piano bounds or not increasing");
    // ALG886c0: average pitch=(last-first)/(nodes-1), then multiplied by resolution.
    p.tip_pitch_40 = (p.mapping_40.back() - p.mapping_40.front()) / 39 * p.coordinate_resolution;
    p.tip_pitch_60 = (p.mapping_60.back() - p.mapping_60.front()) / 59 * p.coordinate_resolution;
    const auto diffs = array(ini, "stylus." + prefix + "_coor_diff", 6);
    const auto angles = array(ini, "stylus.stylus_angle", 6);
    std::copy(diffs.begin(), diffs.end(), p.coordinate_differences.begin());
    std::copy(angles.begin(), angles.end(), p.tilt_angles.begin());
    p.calibration_enabled = value(ini, "stylus.stylus_tilt_calibration_en") != 0;
    p.calibration_threshold = profile_value("tilt_calibration_thd");
    p.calibration_rate = profile_value("tilt_calibration_rate");
    p.tip_slope_40 = profile_value("tip_slope_rx");
    p.tip_slope_60 = profile_value("tip_slope_tx");
    p.edge_40_left = value(ini, "stylus.stylus_rx_l_edge_param");
    p.edge_40_right = value(ini, "stylus.stylus_rx_r_edge_param");
    p.edge_60_left = value(ini, "stylus.stylus_tx_l_edge_param");
    p.edge_60_right = value(ini, "stylus.stylus_tx_r_edge_param");
    p.edge_limit_40_left = value(ini, "stylus.stylus_limit_rx_start");
    p.edge_limit_40_right = value(ini, "stylus.stylus_limit_rx_end");
    p.edge_limit_60_left = value(ini, "stylus.stylus_limit_tx_start");
    p.edge_limit_60_right = value(ini, "stylus.stylus_limit_tx_end");
    p.up_threshold = value(ini, "stylus.stylus_up_threshold");
    p.ring_threshold = value(ini, "stylus.stylus_ring_threshold");
    p.kalman_q_min = value(ini, "stylus.stylus_kalman_q_div_min");
    p.kalman_q_max = value(ini, "stylus.stylus_kalman_q_div_max");
    p.kalman_lambda_min = value(ini, "stylus.stylus_kalman_lamda_div_min");
    p.kalman_lambda_max = value(ini, "stylus.stylus_kalman_lamda_div_max");
    p.first_lock_dis = value(ini, "stylus.stylus_first_jitter_lock_dis");
    p.stable_lock_dis = value(ini, "stylus.stylus_stable_jitter_lock_dis");
    p.move_lock_dis = value(ini, "stylus.stylus_move_jitter_lock_dis");
    if (diffs.front() != 0 || angles.front() != 0 ||
        std::adjacent_find(diffs.begin(), diffs.end(), std::greater_equal<int>()) != diffs.end() ||
        std::adjacent_find(angles.begin(), angles.end(), std::greater_equal<int>()) != angles.end() ||
        p.calibration_threshold < 0 || p.calibration_rate <= 0 ||
        p.tip_slope_40 <= 0 || p.tip_slope_40 >= 16 || p.tip_slope_60 <= 0 || p.tip_slope_60 >= 16 ||
        p.up_threshold <= 0 || p.ring_threshold <= 0 || p.kalman_q_min <= 0 ||
        p.kalman_q_max <= 0 || p.kalman_lambda_min <= 0 || p.kalman_lambda_max <= 0 ||
        p.tip_pitch_40 <= 0 || p.tip_pitch_60 <= 0)
        throw std::runtime_error("actual ini calibration is unsupported");
    out.info = {orientation, vendor, p.coordinate_resolution, 213599, 319999, 16383, -60, 60,
        p.tip_pitch_40, p.tip_pitch_60, p.calibration_threshold, p.calibration_rate,
        p.mapping_40.front(), p.mapping_40.back(), p.mapping_60.front(), p.mapping_60.back()};
    return out;
}
void error_text(char *buffer, size_t bytes, const char *text) {
    if (buffer && bytes) std::snprintf(buffer, bytes, "%s", text);
}
uint16_t le16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
int frame_rate(uint8_t mode) {
    constexpr int rates[] = {120, 180, 240, 360, 60, 480, 144};
    return mode < std::size(rates) ? rates[mode] : 0;
}
} // namespace

struct piano_pen_core {
    struct Pressure { uint64_t time; int value; };
    Calibration config;
    piano_pen::StylusDecoder decoder;
    piano_pen_frame frame{};
    std::deque<Pressure> pressure;
    Pressure last_pressure{};
    uint64_t max_age, last_report_time = 0, last_frame_time = 0;
    bool have_pressure = false, have_report_time = false, have_frame_time = false;
    piano_pen_core(Calibration cfg, uint64_t age)
        : config(cfg), decoder(cfg.profile), max_age(age) {
        const piano_pen_geometry geometry{12, 40, 60, 8, 2400};
        const int error = piano_pen_frame_init(&frame, &geometry);
        if (error) throw std::runtime_error("cannot allocate pen frame buffers");
    }
    ~piano_pen_core() { piano_pen_frame_destroy(&frame); }
};

extern "C" struct piano_pen_core *piano_pen_core_create(const char *ini, int vendor,
    piano_pen_orientation orientation, uint64_t age, char *error, size_t size) {
    try { return new piano_pen_core(calibration(read_ini(ini), vendor, orientation), age); }
    catch (const std::exception &e) { error_text(error, size, e.what()); return nullptr; }
}
extern "C" void piano_pen_core_destroy(piano_pen_core *core) { delete core; }
extern "C" void piano_pen_core_reset(piano_pen_core *core) {
    if (!core) return;
    core->decoder.reset(); core->pressure.clear(); core->have_pressure = false;
    core->have_report_time = core->have_frame_time = false;
    core->frame.packet_sum = core->frame.part_mask = 0;
    core->frame.frame[0x165] = 0;
}
extern "C" int piano_pen_core_info(const piano_pen_core *core, piano_pen_info *info) {
    if (!core || !info) return -EINVAL;
    *info = core->config.info; return 0;
}
extern "C" int piano_pen_core_report5(piano_pen_core *core, const uint8_t *report,
    size_t bytes, uint64_t time, char *error, size_t size) {
    if (!core || !report || bytes != 15 || report[0] != 5) {
        error_text(error, size, "expected an actual15-byte BLE report5"); return -EINVAL;
    }
    const int pressure = le16(report + 1);
    if (pressure > core->config.info.pressure_max ||
        (core->have_report_time && time < core->last_report_time)) {
        error_text(error, size, "pressure value or report timestamp is outside the real input range");
        return -ERANGE;
    }
    try {
        core->pressure.push_back({time, pressure});
        if (core->pressure.size() > 10) core->pressure.pop_front();
    } catch (const std::exception &e) { error_text(error, size, e.what()); return -ENOMEM; }
    core->last_report_time = time; core->have_report_time = true;
    return 0;
}
extern "C" int piano_pen_core_process29(piano_pen_core *core, const uint8_t *payload,
    size_t bytes, uint64_t time, piano_pen_output *output, char *error, size_t size) {
    if (!core || !payload || !output) return -EINVAL;
    *output = {};
    if (core->have_frame_time && time < core->last_frame_time) {
        error_text(error, size, "frame timestamps must preserve actual monotonic ordering"); return -EINVAL;
    }
    if (bytes < 100 || payload[48] != 60 || payload[49] != 40) {
        error_text(error, size, "actual payload must have Piano60cols/40rows"); return -EINVAL;
    }
    // Rate validation is stateless and must precede quarter accumulation.
    const int rate = frame_rate(payload[0x3d]);
    if (!rate) { error_text(error, size, "unknown actual frame-rate mode"); return -EOPNOTSUPP; }
    const char *why = nullptr;
    const int decoded = piano_pen_frame_decode(&core->frame, payload, bytes, &why);
    if (decoded) { error_text(error, size, why ? why : "raw29 decode failed"); return -decoded; }
    piano_pen::RawStylusFrame raw;
    for (size_t i = 0; i < 480; ++i) {
        raw.tip_x[i] = core->frame.matrices[0][i]; raw.tip_y[i] = core->frame.matrices[1][i];
        raw.ring_x[i] = core->frame.matrices[2][i]; raw.ring_y[i] = core->frame.matrices[3][i];
    }
    raw.frame_interval = rate;
    raw.special_state = payload[80] != 0;
    piano_pen::StylusFrameResult result;
    try { result = core->decoder.process(raw); }
    catch (const std::exception &e) {
        // A rejected solver frame cannot make a later hand group complete.
        core->frame.packet_sum = core->frame.part_mask = 0;
        core->frame.frame[0x165] = 0;
        core->decoder.reset();
        error_text(error, size, e.what());
        return -ERANGE;
    }
    core->last_frame_time = time; core->have_frame_time = true;
    output->timestamp_ns = time; output->valid = PIANO_PEN_RAW_VALID | PIANO_PEN_RAW_DISTANCE_VALID;
    output->frame_no = le16(payload + 66); output->raw_pressure = le16(payload + 68);
    output->raw_distance = payload[80] != 0;
    output->frequency_request = core->frame.frequency_request;
    if (core->frame.frame[0x165]) output->valid |= PIANO_PEN_MUTUAL_COMPLETE;
    if (result.active) {
        output->valid |= PIANO_PEN_COORDINATES_VALID;
        output->x = std::clamp(result.coordinates.tip_x, 0, core->config.info.x_max);
        output->y = std::clamp(result.coordinates.tip_y, 0, core->config.info.y_max);
        if (result.ring.valid) {
            output->valid |= PIANO_PEN_TILT_VALID;
            output->tilt_x = std::clamp(result.coordinates.tilt_x, -60, 60);
            output->tilt_y = std::clamp(result.coordinates.tilt_y, -60, 60);
        }
    }
    // Consume one actual report per raw frame, as the OEM/peer bounded queue.
    if (!core->pressure.empty() && core->pressure.front().time <= time) {
        core->last_pressure = core->pressure.front(); core->pressure.pop_front();
        core->have_pressure = true;
    }
    if (core->have_pressure && core->last_pressure.time <= time) {
        output->pressure_age_ns = time - core->last_pressure.time;
        if (output->pressure_age_ns <= core->max_age) {
            output->pressure = core->last_pressure.value;
            output->valid |= PIANO_PEN_PRESSURE_VALID;
        }
    }
    return 0;
}
