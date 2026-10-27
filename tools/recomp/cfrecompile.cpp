// Command-line front end of the recompiler (src/recompiler), used by the build:
//
//   cfrecompile --dol <main.dol> --version RUUE01 --symbols <symbols.txt> [--splits <splits.txt>]
//               [--rels <dir of .rel files> [--rel-maps <dir of <module>.txt maps>]]
//               [--hooks <hooks.txt>] [--version-map <map.txt>] --out <dir>
//
// Files are only rewritten when their contents change, so incremental builds stay cheap.
#include "recompiler/recompiler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dol, symbols, splits, hooks, versionMap, outDir, rels, relMaps, version = "RUUE01";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--dol") dol = v;
        else if (k == "--symbols") symbols = v;
        else if (k == "--splits") splits = v;
        else if (k == "--hooks") hooks = v;
        else if (k == "--version-map") versionMap = v;
        else if (k == "--version") version = v;
        else if (k == "--rels") rels = v;
        else if (k == "--rel-maps") relMaps = v;
        else if (k == "--out") outDir = v;
        else {
            std::fprintf(stderr, "cfrecompile: unknown option %s\n", k.c_str());
            return 2;
        }
    }
    if (dol.empty() || outDir.empty()) {
        std::fprintf(stderr, "usage: cfrecompile --dol F [--symbols F] [--splits F] [--rels DIR [--rel-maps DIR]] "
                             "[--hooks F] [--version V] [--version-map F] --out DIR\n");
        return 2;
    }
    cfrc::Input in;
    in.version = version;
    std::string dolText;
    if (!readFile(dol, dolText) || (!symbols.empty() && std::filesystem::exists(symbols) && !readFile(symbols, in.symbols)) ||
        (!splits.empty() && !readFile(splits, in.splits)) || (!hooks.empty() && !readFile(hooks, in.hooks)) ||
        (!versionMap.empty() && !readFile(versionMap, in.versionMap))) {
        std::fprintf(stderr, "cfrecompile: could not read an input file\n");
        return 1;
    }
    in.dol.assign(dolText.begin(), dolText.end());
    if (!rels.empty()) {
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(rels))
            if (e.is_regular_file() && e.path().extension() == ".rel") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            cfrc::RelInput ri;
            ri.name = f.stem().string();
            std::string data;
            if (!readFile(f, data)) {
                std::fprintf(stderr, "cfrecompile: could not read %s\n", f.string().c_str());
                return 1;
            }
            ri.data.assign(data.begin(), data.end());
            if (!relMaps.empty()) readFile(std::filesystem::path(relMaps) / (ri.name + ".txt"), ri.map);
            in.rels.push_back(std::move(ri));
        }
    }
    cfrc::Output out;
    std::string err;
    if (!cfrc::recompile(in, out, err)) {
        std::fprintf(stderr, "cfrecompile: %s\n", err.c_str());
        return 1;
    }
    const std::filesystem::path dir(outDir);
    std::filesystem::create_directories(dir);
    for (auto& [name, text] : out.files) {
        const auto path = dir / name;
        std::string old;
        if (readFile(path, old) && old == text) continue;  // unchanged: keep the timestamp
        std::ofstream o(path, std::ios::binary);
        o.write(text.data(), std::streamsize(text.size()));
        if (!o) {
            std::fprintf(stderr, "cfrecompile: could not write %s\n", path.string().c_str());
            return 1;
        }
    }
    // Always rewritten: the build's stamp that this run happened.
    std::ofstream(dir / "summary.txt", std::ios::binary) << out.summary << '\n';
    std::printf("%s\n", out.summary.c_str());
    return 0;
}
