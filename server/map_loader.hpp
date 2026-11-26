// File-based map resources: parsing the metadata header, loading a directory, locating the default
// resource directory. Layering: this is I/O and lives with the server; the rules library
// (parse_map / validate_map) stays free of files and clocks.
//
// Resource format (extends the existing tile format, no new language): a ".map" text file whose
// tiles are whitespace-separated rows exactly as parse_map accepts them, plus metadata in comments:
//
//   ; id: training-01            required, unique, [A-Za-z0-9._:-], 1-32 characters
//   ; name: Training Floor       required, printable ASCII, <= 48 characters
//   ; description: ...           optional, printable ASCII, <= 120 characters
//   ; players: 2-4               optional ("N" or "A-B", within 2-4), default 2-4
#pragma once

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "botched/map.hpp"

namespace botched::server {

inline constexpr std::size_t kMaxMapFileBytes = 64 * 1024;
inline constexpr std::size_t kMaxMapIdLength = 32;
inline constexpr std::size_t kMaxMapNameLength = 48;
inline constexpr std::size_t kMaxMapDescriptionLength = 120;

struct MapInfo {
    std::string id;
    std::string name;
    std::string description;
    int min_players = 2;
    int max_players = kMaxPlayers;
    std::string file;  // source file name (diagnostics only; empty for programmatic maps)
};

struct LoadedMap {
    MapInfo info;
    std::shared_ptr<const Map> map;  // immutable once loaded: matches keep it alive on their own
};

// Parses and fully validates one resource (metadata, tiles, section 8 checks for every supported
// player count). Returns nullopt and appends "<filename>: ..." diagnostics to `errors` on failure.
std::optional<LoadedMap> parse_map_resource(const std::string& filename, std::string_view text,
                                            std::vector<std::string>& errors);

struct MapDirReport {
    std::string dir;                  // directory that was scanned
    std::vector<LoadedMap> maps;      // valid maps, ascending file name
    std::vector<std::string> errors;  // one entry per problem, always starting with the file name
};

// Loads every "*.map" file directly inside `dir` (sorted by name). Invalid files and duplicate ids
// are reported and skipped; the remaining maps are still returned.
MapDirReport load_map_dir(const std::string& dir);

// One-paragraph startup report for host programs: what was loaded from where, then every problem.
void print_map_report(const MapDirReport& report, std::ostream& out);

// Where the maps live when nothing is configured. First existing directory wins:
//   1. `explicit_dir` (e.g. --maps-dir), used as given (not checked here: a bad path is reported by load_map_dir)
//   2. $BOTCHED_MAPS_DIR
//   3. <executable dir>/resources/maps, <executable dir>/../share/botched/maps (installed layout),
//      <executable dir>/../resources/maps (build tree inside the source tree)
//   4. the compile-time default (BOTCHED_DEFAULT_MAPS_DIR: the source tree's resources/maps)
//   5. ./resources/maps
// `exe_path` defaults to the running executable when empty.
std::string resolve_maps_dir(const std::string& explicit_dir = "", const std::string& exe_path = "");

}  // namespace botched::server
