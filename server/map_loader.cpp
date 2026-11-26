#include "map_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace botched::server {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && issp(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && issp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

bool printable_ascii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
}

bool id_ok(const std::string& s) {
    if (s.empty() || s.size() > kMaxMapIdLength) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-';
    });
}

// "3" or "2-4", within 2..kMaxPlayers.
bool parse_players(const std::string& v, int& lo, int& hi) {
    auto num = [](const std::string& t, int& out) {
        if (t.size() != 1 || t[0] < '0' || t[0] > '9') return false;
        out = t[0] - '0';
        return true;
    };
    std::string a = trim(v), b;
    auto dash = a.find('-');
    if (dash != std::string::npos) { b = trim(a.substr(dash + 1)); a = trim(a.substr(0, dash)); }
    if (!num(a, lo)) return false;
    hi = lo;
    if (dash != std::string::npos && !num(b, hi)) return false;
    return lo >= 2 && hi <= kMaxPlayers && lo <= hi;
}

}  // namespace

std::optional<LoadedMap> parse_map_resource(const std::string& filename, std::string_view text,
                                            std::vector<std::string>& errors) {
    const std::size_t first_error = errors.size();
    auto bad = [&](const std::string& what) { errors.push_back(filename + ": " + what); };

    // ---- metadata header: "; key: value" comment lines ----
    static const std::set<std::string> kKeys{"id", "name", "description", "players"};
    std::map<std::string, std::string> meta;
    {
        std::istringstream in{std::string(text)};
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            std::string t = trim(line);
            if (t.empty() || t[0] != ';') continue;
            std::string body = trim(t.substr(1));
            auto colon = body.find(':');
            if (colon == std::string::npos) continue;
            std::string key = trim(body.substr(0, colon));
            if (!kKeys.count(key)) continue;  // ordinary comment
            if (meta.count(key)) { bad("line " + std::to_string(lineno) + ": duplicate metadata key '" + key + "'"); continue; }
            meta[key] = trim(body.substr(colon + 1));
        }
    }
    MapInfo info;
    info.file = filename;
    if (!meta.count("id") || meta["id"].empty()) bad("missing required metadata '; id: ...'");
    else if (!id_ok(meta["id"])) bad("id '" + meta["id"] + "' must be 1-" + std::to_string(kMaxMapIdLength) + " characters of [A-Za-z0-9._:-]");
    else info.id = meta["id"];
    if (!meta.count("name") || meta["name"].empty()) bad("missing required metadata '; name: ...'");
    else if (meta["name"].size() > kMaxMapNameLength || !printable_ascii(meta["name"]))
        bad("name must be printable ASCII, at most " + std::to_string(kMaxMapNameLength) + " characters");
    else info.name = meta["name"];
    if (meta.count("description")) {
        if (meta["description"].size() > kMaxMapDescriptionLength || !printable_ascii(meta["description"]))
            bad("description must be printable ASCII, at most " + std::to_string(kMaxMapDescriptionLength) + " characters");
        else info.description = meta["description"];
    }
    if (meta.count("players") && !parse_players(meta["players"], info.min_players, info.max_players))
        bad("players '" + meta["players"] + "' must be N or A-B within 2-" + std::to_string(kMaxPlayers));
    if (errors.size() != first_error) return std::nullopt;

    // ---- tiles: the existing format, unchanged ----
    MapLoad parsed = parse_map(info.id, text);
    if (!parsed.map) {
        for (const auto& e : parsed.errors) bad(e);
        if (parsed.errors.empty()) bad("map could not be parsed");
        return std::nullopt;
    }
    for (int n = info.min_players; n <= info.max_players; ++n) {
        ValidationOptions vo;
        vo.player_count = n;
        for (const auto& e : validate_map(*parsed.map, vo)) bad("invalid for " + std::to_string(n) + " players: " + e);
        if (errors.size() != first_error) break;  // one player count's report is enough
    }
    if (errors.size() != first_error) return std::nullopt;
    LoadedMap out;
    out.info = std::move(info);
    out.map = std::make_shared<const Map>(std::move(*parsed.map));
    return out;
}

MapDirReport load_map_dir(const std::string& dir) {
    MapDirReport rep;
    rep.dir = dir;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        rep.errors.push_back(dir + ": maps directory not found");
        return rep;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".map") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::map<std::string, std::string> id_file;  // id -> first file that defined it
    for (const auto& p : files) {
        const std::string name = p.filename().string();
        std::ifstream f(p, std::ios::binary);
        if (!f) { rep.errors.push_back(name + ": cannot be read"); continue; }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.size() > kMaxMapFileBytes) { rep.errors.push_back(name + ": file larger than " + std::to_string(kMaxMapFileBytes) + " bytes"); continue; }
        auto lm = parse_map_resource(name, text, rep.errors);
        if (!lm) continue;
        auto dup = id_file.find(lm->info.id);
        if (dup != id_file.end()) {
            rep.errors.push_back(name + ": duplicate map id '" + lm->info.id + "' (already defined by " + dup->second + ")");
            continue;
        }
        id_file[lm->info.id] = name;
        rep.maps.push_back(std::move(*lm));
    }
    return rep;
}

void print_map_report(const MapDirReport& rep, std::ostream& out) {
    out << "maps: " << rep.maps.size() << " loaded from " << rep.dir;
    for (std::size_t i = 0; i < rep.maps.size(); ++i)
        out << (i ? ", " : " (") << rep.maps[i].info.id << " <- " << rep.maps[i].info.file << (i + 1 == rep.maps.size() ? ")" : "");
    out << "\n";
    for (const auto& e : rep.errors) out << "map error: " << e << "\n";
}

namespace {
bool has_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

fs::path current_executable() {
    std::error_code ec;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    fs::path p = fs::weakly_canonical(fs::path(buf.data()), ec);
    return ec ? fs::path(buf.data()) : p;
#else
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}
}  // namespace

std::string resolve_maps_dir(const std::string& explicit_dir, const std::string& exe_path) {
    if (!explicit_dir.empty()) return explicit_dir;
    if (const char* env = std::getenv("BOTCHED_MAPS_DIR"); env && *env) return env;
    std::error_code ec;
    fs::path exe = exe_path.empty() ? current_executable() : fs::path(exe_path);
    if (!exe.empty()) {
        if (exe.is_relative()) exe = fs::absolute(exe, ec);
        const fs::path dir = exe.parent_path();
        for (const char* rel : {"resources/maps", "../share/botched/maps", "../resources/maps"}) {
            fs::path cand = (dir / rel).lexically_normal();
            if (has_dir(cand)) return cand.string();
        }
    }
#ifdef BOTCHED_DEFAULT_MAPS_DIR
    if (has_dir(BOTCHED_DEFAULT_MAPS_DIR)) return BOTCHED_DEFAULT_MAPS_DIR;
#endif
    return "resources/maps";
}

}  // namespace botched::server
