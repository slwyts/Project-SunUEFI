// SPDX-License-Identifier: Apache-2.0
#include "piano-pen-core.h"
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
uint64_t number(const std::string &text) {
    uint64_t value;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("invalid unsigned timestamp or freshness limit");
    return value;
}
std::vector<uint8_t> file(const std::filesystem::path &path, size_t maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open captured input: " + path.string());
    const auto size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > maximum)
        throw std::runtime_error("captured input exceeds its actual packet bound");
    std::vector<uint8_t> out(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(out.data()), size))
        throw std::runtime_error("captured input read failed");
    return out;
}
void field(const char *name, int value, bool known) {
    std::cout << ",\"" << name << "\":";
    if (known) std::cout << value; else std::cout << "null";
}
void usage() {
    std::cout << "piano-pen-offline --ini ACTUAL.ini --config\n"
        "piano-pen-offline --ini ACTUAL.ini --pressure-max-age-ns N --events ACTUAL.tsv\n"
        "Events: boottime_ns report5|raw29 CAPTURED_FILE; or boottime_ns reset -\n"
        "Use CLOCK_BOOTTIME for BLE, matching NTP ktime_get_boottime; do not mix CLOCK_MONOTONIC across sleep.\n"
        "raw29 starts at original payload without257-byte transport. report5 is actual15-byte BLE data.\n"
        "Select the actual Touch LCDid/factory ini, not the display label. Output is portrait diagnostics only; no FIFO/device/uinput.\n"
        "Unknown values are null. Freshness is caller policy, not a claimed OEM timeout.\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::string ini, events;
        uint64_t age = 0;
        bool config_only = false, have_age = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { usage(); return 0; }
            if (option == "--config") { config_only = true; continue; }
            if (i + 1 == argc) throw std::runtime_error("option value is missing");
            if (option == "--ini") ini = argv[++i];
            else if (option == "--events") events = argv[++i];
            else if (option == "--pressure-max-age-ns") {
                age = number(argv[++i]); have_age = true;
            } else throw std::runtime_error("unknown option: " + option);
        }
        if (ini.empty() || (config_only ? !events.empty() : events.empty() || !have_age)) {
            usage(); return 2;
        }
        char error[256]{};
        std::unique_ptr<piano_pen_core, decltype(&piano_pen_core_destroy)> core(
            piano_pen_core_create(ini.c_str(), 3, PIANO_PEN_PORTRAIT, age, error, sizeof(error)),
            piano_pen_core_destroy);
        if (!core) throw std::runtime_error(error);
        piano_pen_info info{};
        piano_pen_core_info(core.get(), &info);
        std::cout << "{\"kind\":\"configuration\",\"orientation\":\"portrait\",\"vendor_id\":"
            << info.vendor_id << ",\"resolution\":" << info.resolution
            << ",\"x_max\":" << info.x_max << ",\"y_max\":" << info.y_max
            << ",\"pressure_max\":" << info.pressure_max
            << ",\"tilt_min\":" << info.tilt_min << ",\"tilt_max\":" << info.tilt_max
            << ",\"pitch_x\":" << info.pitch_x << ",\"pitch_y\":" << info.pitch_y
            << ",\"calibration_threshold\":" << info.calibration_threshold
            << ",\"calibration_rate\":" << info.calibration_rate
            << ",\"mapping_x_first\":" << info.mapping_x_first
            << ",\"mapping_x_last\":" << info.mapping_x_last
            << ",\"mapping_y_first\":" << info.mapping_y_first
            << ",\"mapping_y_last\":" << info.mapping_y_last << "}\n";
        if (config_only) return 0;
        std::ifstream manifest(events);
        if (!manifest) throw std::runtime_error("cannot open actual event sequence");
        const auto folder = std::filesystem::path(events).parent_path();
        std::string line;
        uint64_t previous = 0;
        bool have_previous = false;
        size_t count = 0;
        while (std::getline(manifest, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line.size() > 8192) throw std::runtime_error("event manifest line is too long");
            std::istringstream fields(line);
            std::string stamp, kind, path;
            if (!(fields >> stamp >> kind)) throw std::runtime_error("invalid event manifest");
            std::getline(fields >> std::ws, path);
            const uint64_t timestamp = number(stamp);
            if (have_previous && timestamp < previous)
                throw std::runtime_error("event sequence is not monotonic");
            previous = timestamp; have_previous = true; ++count;
            int result;
            if (kind == "reset" && path == "-") {
                piano_pen_core_reset(core.get());
                std::cout << "{\"kind\":\"reset\",\"timestamp_ns\":" << timestamp << "}\n";
                continue;
            }
            if (path.empty()) throw std::runtime_error("captured file path is missing");
            const auto bytes = file(folder / path, kind == "report5" ? 15 : 7934);
            if (kind == "report5") {
                result = piano_pen_core_report5(core.get(), bytes.data(), bytes.size(), timestamp,
                                                error, sizeof(error));
                if (result) throw std::runtime_error(error);
                std::cout << "{\"kind\":\"report5\",\"timestamp_ns\":" << timestamp
                    << ",\"pressure\":" << (bytes[1] | int(bytes[2]) << 8) << "}\n";
            } else if (kind == "raw29") {
                piano_pen_output output{};
                result = piano_pen_core_process29(core.get(), bytes.data(), bytes.size(), timestamp,
                                                  &output, error, sizeof(error));
                if (result) throw std::runtime_error(error);
                std::cout << "{\"kind\":\"raw29\",\"timestamp_ns\":" << timestamp
                    << ",\"frame_no\":" << output.frame_no << ",\"valid\":" << output.valid;
                field("x", output.x, output.valid & PIANO_PEN_COORDINATES_VALID);
                field("y", output.y, output.valid & PIANO_PEN_COORDINATES_VALID);
                field("tilt_x", output.tilt_x, output.valid & PIANO_PEN_TILT_VALID);
                field("tilt_y", output.tilt_y, output.valid & PIANO_PEN_TILT_VALID);
                field("pressure", output.pressure, output.valid & PIANO_PEN_PRESSURE_VALID);
                field("raw_distance", output.raw_distance, output.valid & PIANO_PEN_RAW_DISTANCE_VALID);
                std::cout << ",\"raw_pressure\":" << output.raw_pressure
                    << ",\"pressure_age_ns\":";
                if (output.valid & PIANO_PEN_PRESSURE_VALID) std::cout << output.pressure_age_ns;
                else std::cout << "null";
                std::cout << ",\"mutual_complete\":" << ((output.valid & PIANO_PEN_MUTUAL_COMPLETE) ? "true" : "false")
                    << ",\"frequency_request\":" << output.frequency_request << "}\n";
            } else throw std::runtime_error("unknown event type");
        }
        if (!manifest.eof()) throw std::runtime_error("event manifest read failed");
        std::cerr << "Processed " << count << " actual supplied events; no input injection.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
