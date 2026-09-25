#include "ark/core/argument_parser.hh"
#include "ark/logging/log_reader.hh"
#include "ark/logging/query.hh"
#include "ark/logging/read_cursor.hh"
#include "ark/serialization/helpers.hh"

#include "lab37/assembly_line/xcu_mappings.hh"
#include "lab37/canopen/messages.hh"

#include <fmt/core.h>

#include <fnmatch.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace
{

constexpr auto STATE_CHANNEL = "/xcu/state";
constexpr auto MAPPING_CHANNEL = "/xcu/mapping";

/// Emitted verbatim into the legacy source_path column, which named the JSON path the
/// matrices used to be dug out of.
constexpr auto SOURCE_PATH = ".xcus[].sensors.ranges[].range_matrix";

/// Gizmo ingredient dispenser sensor boards; every other node on the bus is noise here.
constexpr uint32_t DEFAULT_NODE_ID = 0x50;

const std::vector<std::string> LEGACY_COLUMNS{"source_path",
                                              "time_of_validity",
                                              "station_id",
                                              "node_id",
                                              "bus",
                                              "spad_map_id",
                                              "sensor_receive_time",
                                              "sensor_range_m",
                                              "receive_time",
                                              "time_of_last_reading",
                                              "row",
                                              "col",
                                              "range_m"};

const std::vector<std::string> EXTRA_COLUMNS{"range_index",
                                             "rows",
                                             "cols",
                                             "quantization_min_mm",
                                             "quantization_max_mm",
                                             "measured_read_hz"};

///
/// Identifies one physical range sensor. The range index is what separates a hopper
/// sensor from the bowl-detect (BSU) sensor when both sit on the same node.
///
struct SensorKey
{
    std::string bus;
    uint32_t node_id{};
    uint32_t range_index{};

    auto operator<=>(const SensorKey &) const = default;
};

struct SensorMapping
{
    uint32_t station_id{};

    /// True for the Gizmo BSU bowl-detect sensor rather than the hopper sensor.
    bool object_range{};
};

///
/// A range matrix awaiting output, held only while the mapping that resolves its
/// station is still unknown.
///
struct PendingSample
{
    SensorKey key;
    int64_t time_of_validity{};
    lab37::XcuRangeSensor sensor;
};

struct SensorSummary
{
    std::optional<uint32_t> station_id;
    bool object_range{};
    int spad_map_id{};
    uint16_t rows{};
    uint16_t cols{};
    size_t samples{};
    int64_t first_time{};
    int64_t last_time{};
};

/// Empty bus names are normalized to "can0" on the wire; do the same on both sides of
/// the mapping join so the keys actually match.
std::string normalize_bus(std::string bus)
{
    return bus.empty() ? "can0" : std::move(bus);
}

int64_t to_nanos(const std::chrono::steady_clock::time_point &time)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

///
/// Parses a comma separated list of unsigned values, accepting hex (0x50) and decimal
/// spellings. Returns nullopt for "all", meaning no filtering.
///
std::optional<std::set<uint32_t>> parse_id_list(const std::vector<std::string> &specs)
{
    std::set<uint32_t> ids;

    for (const auto &spec : specs)
    {
        size_t start = 0;

        while (start <= spec.size())
        {
            auto end = spec.find(',', start);
            auto piece = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);

            if (!piece.empty())
            {
                if (piece == "all")
                {
                    return std::nullopt;
                }

                ids.insert(static_cast<uint32_t>(std::stoul(piece, nullptr, 0)));
            }

            if (end == std::string::npos)
            {
                break;
            }

            start = end + 1;
        }
    }

    return ids;
}

std::string substitute(std::string text, const std::string &token, const std::string &value)
{
    for (auto at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
    {
        text.replace(at, token.size(), value);
    }

    return text;
}

///
/// Opens output files lazily, one per distinct expansion of the path template. A template
/// with no placeholder collapses to a single combined file.
///
class CsvWriters
{
public:
    CsvWriters(std::string path_template, bool force, bool full_columns)
        : template_(std::move(path_template)), force_(force), full_columns_(full_columns)
    {
    }

    std::ofstream &writer_for(uint32_t station_id, const std::string &bus)
    {
        auto path = substitute(template_, "{station}", std::to_string(station_id));
        path = substitute(path, "{bus}", bus);

        last_path_ = path;

        auto existing = files_.find(path);

        if (existing != files_.end())
        {
            return existing->second;
        }

        if (!force_ && std::filesystem::exists(path))
        {
            throw std::runtime_error(fmt::format("{} already exists; pass --force to overwrite it", path));
        }

        std::ofstream file(path);

        if (!file)
        {
            throw std::runtime_error(fmt::format("could not open {} for writing", path));
        }

        auto columns = LEGACY_COLUMNS;

        if (full_columns_)
        {
            columns.insert(columns.end(), EXTRA_COLUMNS.begin(), EXTRA_COLUMNS.end());
        }

        for (size_t index = 0; index < columns.size(); ++index)
        {
            file << (index == 0 ? "" : ",") << columns[index];
        }

        file << "\n";

        auto [inserted, _] = files_.emplace(path, std::move(file));
        counts_.emplace(path, 0);

        return inserted->second;
    }

    void record_rows(size_t rows)
    {
        counts_[last_path_] += rows;
    }

    [[nodiscard]] const std::map<std::string, size_t> &counts() const
    {
        return counts_;
    }

private:
    std::string template_;
    bool force_;
    bool full_columns_;
    std::string last_path_;
    std::map<std::string, std::ofstream> files_;
    std::map<std::string, size_t> counts_;
};

///
/// Collects the (bus, node, range index) -> station mapping that the log's own assembly
/// line configuration declares, so callers can select by station instead of by bus.
///
std::map<SensorKey, SensorMapping> build_mapping(const lab37::AssemblyLineXcuMapping &mapping)
{
    std::map<SensorKey, SensorMapping> resolved;

    for (const auto &dispenser : mapping.ingredient_dispensers)
    {
        resolved[SensorKey{normalize_bus(dispenser.sensors.bus),
                           dispenser.sensors.node,
                           dispenser.sensors.range_index}] = SensorMapping{dispenser.station_id, false};

        if (dispenser.object_range)
        {
            resolved[SensorKey{normalize_bus(dispenser.object_range->bus),
                               dispenser.object_range->node,
                               dispenser.object_range->range_index}] = SensorMapping{dispenser.station_id, true};
        }
    }

    return resolved;
}

}; // namespace

int main(int argc, const char **argv)
{
    ark::core::ArgumentParser parser;

    parser.set_description("Extracts Gizmo hopper range matrices from a robot log into per-station CSVs.");

    parser.add_positional("log-url").help("The log identifier or manifest path to read.").required();

    parser.add_option("station")
        .add_name("s")
        .help("Station ids to extract; comma separated, repeatable, or 'all'.")
        .requires_argument("IDS")
        .allow_multiple_uses();

    parser.add_option("list").add_name("l").help("List the range sensors present in the log and exit.");

    parser.add_option("node")
        .help(fmt::format("Node ids to keep, hex or decimal, or 'all' (default: {:#x}).", DEFAULT_NODE_ID))
        .requires_argument("IDS")
        .allow_multiple_uses();

    parser.add_option("bus").help("Keep only buses matching this glob (default: *).").requires_argument("GLOB");

    parser.add_option("range-index").help("Keep only this range sensor index on each node.").requires_argument("N");

    parser.add_option("include-object-range")
        .help("Also emit the Gizmo BSU bowl-detect sensors, which are excluded by default.");

    parser.add_option("output")
        .add_name("o")
        .help("Output path template; {station} and {bus} split the rows across files "
              "(default: range_matrix_station_{station}.csv).")
        .requires_argument("PATH");

    parser.add_option("unmapped").help("Also emit sensors that the log's mapping does not resolve to a station.");

    parser.add_option("no-dedupe").help("Emit every published copy of a reading instead of only fresh ones.");

    parser.add_option("columns")
        .help("'full' (default) appends sensor geometry and quantization; 'legacy' emits only the original 13.")
        .requires_argument("SET");

    parser.add_option("force").help("Overwrite output files that already exist.");

    parser.add_option("minimum").add_name("n").help("Only read messages after this time (in seconds).").requires_argument();

    parser.add_option("maximum").add_name("x").help("Only read messages before this time (in seconds).").requires_argument();

    auto args = parser.parse(argc, argv);

    try
    {
        const bool listing = args.count("list") > 0;

        if (!listing && args.count("station") == 0)
        {
            std::cerr << "FATAL: --station is required (use --station all, or --list to see what is available)."
                      << std::endl;
            return 1;
        }

        auto stations = listing ? std::nullopt : parse_id_list(args.arguments("station"));
        auto nodes = args.count("node") > 0 ? parse_id_list(args.arguments("node"))
                                            : std::optional<std::set<uint32_t>>{{DEFAULT_NODE_ID}};

        const std::string bus_glob = args.count("bus") > 0 ? args["bus"] : "*";
        const std::optional<uint32_t> range_index_filter =
            args.count("range-index") > 0 ? std::optional<uint32_t>{std::stoul(args["range-index"], nullptr, 0)}
                                          : std::nullopt;

        const bool include_object_range = args.count("include-object-range") > 0;
        const bool include_unmapped = args.count("unmapped") > 0;
        const bool dedupe = args.count("no-dedupe") == 0;
        const bool full_columns = args.count("columns") == 0 || args["columns"] != "legacy";

        const std::string output_template =
            args.count("output") > 0 ? args["output"] : "range_matrix_station_{station}.csv";

        //
        // Both channels live in the same log column, so asking for the two of them costs
        // no more to read than asking for the state alone.
        //

        ark::logging::Query query;
        query.message_allow_list = {STATE_CHANNEL, MAPPING_CHANNEL};

        if (args.count("minimum") > 0)
        {
            query.minimum_time = ark::logging::LoggingClock::time_point{std::chrono::duration_cast<
                std::chrono::nanoseconds>(std::chrono::duration<double>{std::stod(args["minimum"])})};
        }

        if (args.count("maximum") > 0)
        {
            query.maximum_time = ark::logging::LoggingClock::time_point{std::chrono::duration_cast<
                std::chrono::nanoseconds>(std::chrono::duration<double>{std::stod(args["maximum"])})};
        }

        ark::logging::LogReader reader{ark::core::Url{args["log-url"]}};
        auto cursor = reader.create_read_cursor(query);

        std::map<SensorKey, SensorMapping> mapping;
        bool mapping_seen = false;
        std::vector<PendingSample> pending;

        std::map<SensorKey, std::pair<int64_t, int64_t>> last_emitted;
        std::map<SensorKey, SensorSummary> summaries;

        CsvWriters writers(output_template, args.count("force") > 0, full_columns);

        size_t total_rows = 0;
        size_t total_matrices = 0;

        //
        // Applies every filter and, for a real run, writes the matrix out. Shared between the
        // buffered prelude and the streaming path so both stay in step.
        //
        auto handle = [&](const PendingSample &sample) {
            const auto &key = sample.key;
            const auto &sensor = sample.sensor;

            if (sensor.range_matrix.empty())
            {
                return;
            }

            if (fnmatch(bus_glob.c_str(), key.bus.c_str(), 0) != 0)
            {
                return;
            }

            if (nodes && !nodes->contains(key.node_id))
            {
                return;
            }

            if (range_index_filter && *range_index_filter != key.range_index)
            {
                return;
            }

            auto found = mapping.find(key);
            const bool mapped = found != mapping.end();

            if (mapped && found->second.object_range && !include_object_range)
            {
                return;
            }

            if (!mapped && !include_unmapped && !listing)
            {
                return;
            }

            if (stations && (!mapped || !stations->contains(found->second.station_id)))
            {
                return;
            }

            //
            // The state channel republishes the same sensor snapshot every tick; a reading is
            // fresh only when the board's own timestamps have moved.
            //

            const auto fingerprint =
                std::make_pair(to_nanos(sensor.receive_time), to_nanos(sensor.range_matrix.front().time_of_last_reading));

            if (dedupe)
            {
                auto &previous = last_emitted[key];

                if (previous == fingerprint)
                {
                    return;
                }

                previous = fingerprint;
            }

            ++total_matrices;

            auto &summary = summaries[key];

            if (summary.samples == 0)
            {
                summary.first_time = sample.time_of_validity;
                summary.station_id = mapped ? std::optional<uint32_t>{found->second.station_id} : std::nullopt;
                summary.object_range = mapped && found->second.object_range;
                summary.spad_map_id = static_cast<int>(sensor.spad_map_id);
                summary.rows = sensor.rows;
                summary.cols = sensor.cols;
            }

            summary.last_time = sample.time_of_validity;
            ++summary.samples;

            if (listing)
            {
                return;
            }

            auto &file = writers.writer_for(mapped ? found->second.station_id : 0, key.bus);

            //
            // The sensor context is identical for every cell, so format it once. fmt is used
            // rather than the stream operators because its default double formatting is the
            // shortest representation that round-trips; ostream would silently cut readings
            // to six significant figures.
            //

            const auto prefix = fmt::format("{},{},{},{},{},{},{},{}",
                                            SOURCE_PATH,
                                            sample.time_of_validity,
                                            mapped ? std::to_string(found->second.station_id) : std::string{},
                                            key.node_id,
                                            key.bus,
                                            static_cast<int>(sensor.spad_map_id),
                                            to_nanos(sensor.receive_time),
                                            sensor.range_m);

            const auto suffix = full_columns ? fmt::format(",{},{},{},{},{},{}",
                                                           key.range_index,
                                                           sensor.rows,
                                                           sensor.cols,
                                                           sensor.quantization_min_mm,
                                                           sensor.quantization_max_mm,
                                                           sensor.measured_read_hz)
                                             : std::string{};

            for (const auto &cell : sensor.range_matrix)
            {
                file << fmt::format("{},{},{},{},{},{}{}\n",
                                    prefix,
                                    to_nanos(cell.receive_time),
                                    to_nanos(cell.time_of_last_reading),
                                    static_cast<int>(cell.row),
                                    static_cast<int>(cell.col),
                                    cell.range_m,
                                    suffix);
            }

            writers.record_rows(sensor.range_matrix.size());
            total_rows += sensor.range_matrix.size();
        };

        while (auto object = cursor.step())
        {
            if (object.object_type->name == MAPPING_CHANNEL)
            {
                mapping = build_mapping(ark::serialization::deserialize<lab37::AssemblyLineXcuMapping>(object.data));

                if (!mapping_seen)
                {
                    mapping_seen = true;

                    for (const auto &sample : pending)
                    {
                        handle(sample);
                    }

                    pending.clear();
                    pending.shrink_to_fit();
                }

                continue;
            }

            auto states = ark::serialization::deserialize<lab37::XcuStateList>(object.data);
            const auto time_of_validity = to_nanos(states.time_of_validity);

            for (auto &xcu : states.xcus)
            {
                for (uint32_t index = 0; index < xcu.sensors.ranges.size(); ++index)
                {
                    PendingSample sample{SensorKey{normalize_bus(xcu.bus), xcu.node_id, index},
                                         time_of_validity,
                                         std::move(xcu.sensors.ranges[index])};

                    //
                    // Until the mapping arrives there is no station to select on, so hold the
                    // samples rather than silently dropping the head of the log.
                    //

                    if (mapping_seen)
                    {
                        handle(sample);
                    }
                    else
                    {
                        pending.push_back(std::move(sample));
                    }
                }
            }
        }

        if (!mapping_seen && !pending.empty())
        {
            std::cerr << fmt::format("WARNING: no {} message in this log; stations cannot be resolved.\n",
                                     MAPPING_CHANNEL);

            for (const auto &sample : pending)
            {
                handle(sample);
            }
        }

        if (listing)
        {
            fmt::print("{:<8} {:<20} {:<6} {:<6} {:<6} {:<8} {:<10} {}\n",
                       "STATION",
                       "BUS",
                       "NODE",
                       "RANGE",
                       "SPAD",
                       "SIZE",
                       "SAMPLES",
                       "SENSOR");

            for (const auto &[key, summary] : summaries)
            {
                fmt::print("{:<8} {:<20} {:<#6x} {:<6} {:<6} {:<8} {:<10} {}\n",
                           summary.station_id ? std::to_string(*summary.station_id) : "-",
                           key.bus,
                           key.node_id,
                           key.range_index,
                           summary.spad_map_id,
                           fmt::format("{}x{}", summary.rows, summary.cols),
                           summary.samples,
                           !summary.station_id      ? "unmapped"
                           : summary.object_range ? "bowl detect (BSU)"
                                                  : "hopper");
            }

            if (summaries.empty())
            {
                fmt::print("(no range sensors matched)\n");
            }

            return 0;
        }

        std::cerr << fmt::format("Wrote {} rows from {} range matrices:\n", total_rows, total_matrices);

        for (const auto &[path, count] : writers.counts())
        {
            std::cerr << fmt::format("  {}: {} rows\n", path, count);
        }

        if (writers.counts().empty())
        {
            std::cerr << "  (nothing matched the given filters)\n";
        }

        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "FATAL: " << exception.what() << std::endl;
        return 1;
    }
}
