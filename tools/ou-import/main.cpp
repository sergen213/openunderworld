// ou-import: read-only inventory of a Tomb Raider: Underworld installation.
#include "importer/Importer.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>

using namespace ou::importer;

namespace {

int usage() {
    std::cerr << "ou-import " << toolVersion() << "\n"
              << "usage:\n"
              << "  ou-import scan <gameDir> [-o manifest.json] [--deep] [--no-hash] [--names list.txt]... [-j N]\n"
              << "  ou-import diff <a.json> <b.json>\n"
              << "  ou-import extract <gameDir> <name|0xHASH> -o <file> [--spec 0xMASK] [--raw]\n";
    return 2;
}

json load(const fs::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return json::parse(f);
}

void writeFile(const fs::path& out, const fs::path& gameDir, const std::string& bytes) {
    if (isInside(out, gameDir)) throw std::runtime_error("refusing to write inside the game directory: " + out.string());
    if (out.has_parent_path()) fs::create_directories(out.parent_path());
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f.write(bytes.data(), std::streamsize(bytes.size()))) throw std::runtime_error("write failed: " + out.string());
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) return usage();
    try {
        const std::string cmd = a[0];
        std::vector<std::string> pos;
        fs::path out;
        ScanOptions so;
        std::optional<uint32_t> spec;
        bool raw = false;
        for (size_t i = 1; i < a.size(); ++i) {
            auto next = [&]() -> std::string {
                if (i + 1 >= a.size()) throw std::runtime_error(a[i] + " needs a value");
                return a[++i];
            };
            if (a[i] == "-o") out = next();
            else if (a[i] == "--deep") so.deep = true;
            else if (a[i] == "--no-hash") so.hashFiles = false;
            else if (a[i] == "--names") so.nameLists.emplace_back(next());
            else if (a[i] == "-j") so.threads = unsigned(std::stoul(next()));
            else if (a[i] == "--spec") spec = uint32_t(std::stoul(next(), nullptr, 16));
            else if (a[i] == "--raw") raw = true;
            else if (a[i].starts_with("-")) return usage();
            else pos.push_back(a[i]);
        }
        if (cmd == "scan" && pos.size() == 1) {
            const fs::path game = pos[0];
            if (!out.empty() && isInside(out, game)) throw std::runtime_error("refusing to write inside the game directory: " + out.string());
            auto m = scan(game, so);
            if (out.empty()) out = defaultManifestPath(m["exe"].value("sha256", std::string("unknown-exe")));
            writeFile(out, game, m.dump(1, ' ', false, json::error_handler_t::replace) + "\n");
            const auto& n = m["names"];
            std::cerr << "manifest: " << out.string() << "\n"
                      << "exe: " << m["exe"].value("build", "?") << "\n"
                      << "names: " << n["resolved_total"] << "/" << n["unique_hashes"] << " unique hashes ("
                      << n["coverage_install_derived_pct"].get<double>() << "% install-derived, "
                      << n["coverage_total_pct"].get<double>() << "% total)\n"
                      << "kinds: " << m["kinds_total"].dump() << "\n"
                      << "runtime: " << m["runtime_seconds"].dump() << "\n";
            for (auto& arc : m["archives"])
                if (arc.value("record_errors", 0) || arc.contains("error") || !arc.value("missing_parts", json::array()).empty())
                    std::cerr << "PROBLEMS in " << arc["name"].get<std::string>() << " (see manifest)\n";
            return 0;
        }
        if (cmd == "diff" && pos.size() == 2) {
            auto d = diffManifests(load(pos[0]), load(pos[1]));
            std::cout << d.dump(1) << "\n";
            return d["identical"].get<bool>() ? 0 : 1;
        }
        if (cmd == "extract" && pos.size() == 2 && !out.empty()) {
            auto x = extract(pos[0], pos[1], spec, raw);
            writeFile(out, pos[0], std::string(x.bytes.begin(), x.bytes.end()));
            std::fprintf(stderr, "%s: hash 0x%08x spec 0x%08x (%zu variant(s)) -> %zu bytes\n", x.archive.c_str(), x.hash,
                         x.specMask, x.variants, x.bytes.size());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "ou-import: " << e.what() << "\n";
        return 1;
    }
}
