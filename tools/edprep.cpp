// edprep: turn the player's Eternal Darkness disc image into recompiled C,
// with no Python (the Mac .app runs it on first launch). A port of
// tools/generate.py, fix_generated.py, symbols.py and gamedata.py, which it
// must match byte for byte (tools/mac/check-edprep.sh compares them).
//
//   1. Read the disc header and check it is Eternal Darkness (GEDE01/GEDP01).
//   2. Extract main.dol (plain ISO/GCM, or a CISO).
//   3. Translate main.dol with DolRecomp into <out>.
//   4. Constant-time chunk dispatch in generated.h (fix_generated.py).
//   5. The symbol table the host's intercepts use (symbols.py).
//   6. The per-build guest constants, game_data.h (gamedata.py).
//
// usage: edprep <disc image> --out <dir> --dolrecomp <binary> --symbols <symbols.txt>
//               [--jobs N]
// Progress lines on stdout ("step N/6 ..."); errors on stderr, exit 1.

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

uint32_t be32(const uint8_t* p) {
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
}
uint32_t le32(const uint8_t* p) {
    return uint32_t{p[3]} << 24 | uint32_t{p[2]} << 16 | uint32_t{p[1]} << 8 | p[0];
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        fail("cannot read " + path.string());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void write_file(const fs::path& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(data.data(), static_cast<std::streamsize>(data.size())))
        fail("cannot write " + path.string());
}

std::string hex(uint64_t v, int width, bool upper) {
    char buf[32];
    std::snprintf(buf, sizeof buf, upper ? "%0*llX" : "%0*llx", width,
                  static_cast<unsigned long long>(v));
    return buf;
}

// --- 1, 2: the disc ----------------------------------------------------------

// Random access to a plain ISO/GCM or a CISO image (generate.py Disc).
class Disc {
public:
    explicit Disc(const fs::path& path) : f_(std::fopen(path.c_str(), "rb")) {
        if (f_ == nullptr)
            fail("cannot open " + path.string());
        std::vector<uint8_t> head(0x8000);
        head.resize(std::fread(head.data(), 1, head.size(), f_));
        ciso_ = head.size() >= 8 && std::memcmp(head.data(), "CISO", 4) == 0;
        if (ciso_) {
            block_ = le32(&head[4]);
            uint32_t n = 0;
            for (size_t i = 8; i < head.size(); i++) {
                index_.push_back(head[i] ? std::optional<uint32_t>(n) : std::nullopt);
                n += head[i] ? 1 : 0;
            }
        }
    }
    ~Disc() { std::fclose(f_); }

    std::vector<uint8_t> read(uint64_t offset, uint64_t size) {
        std::vector<uint8_t> out;
        out.reserve(size);
        if (!ciso_) {
            out.resize(size);
            std::fseek(f_, static_cast<long>(offset), SEEK_SET);
            out.resize(std::fread(out.data(), 1, size, f_));
            return out;
        }
        while (size != 0) {
            const uint64_t blk = offset / block_, inner = offset % block_;
            const uint64_t take = std::min<uint64_t>(size, block_ - inner);
            const std::optional<uint32_t> slot =
                blk < index_.size() ? index_[blk] : std::nullopt;
            const size_t at = out.size();
            out.resize(at + take);
            if (slot) {
                std::fseek(f_, static_cast<long>(0x8000 + uint64_t{*slot} * block_ + inner),
                           SEEK_SET);
                const size_t got = std::fread(out.data() + at, 1, take, f_);
                out.resize(at + got);
            }
            offset += take;
            size -= take;
        }
        return out;
    }

private:
    std::FILE* f_;
    bool ciso_ = false;
    uint32_t block_ = 0;
    std::vector<std::optional<uint32_t>> index_;
};

const std::map<std::string, std::string> kGameIds = {{"GEDE01", "USA"}, {"GEDP01", "Europe"}};

std::string extract_dol(const fs::path& disc_path, const fs::path& out_path) {
    Disc disc(disc_path);
    const std::vector<uint8_t> id_bytes = disc.read(0, 6);
    const std::string game_id(id_bytes.begin(), id_bytes.end());
    if (kGameIds.count(game_id) == 0)
        fail(disc_path.string() + " is " + game_id +
             ", not Eternal Darkness (GEDE01/GEDP01)");
    const uint32_t dol_off = be32(disc.read(0x420, 4).data());
    const std::vector<uint8_t> head = disc.read(dol_off, 0x100);
    if (head.size() < 0x100)
        fail("the disc image is cut short");
    uint64_t end = 0;
    for (int i = 0; i < 18; i++) {
        const uint32_t o = be32(&head[4 * i]), s = be32(&head[144 + 4 * i]);
        if (s != 0)
            end = std::max<uint64_t>(end, uint64_t{o} + s);
    }
    const std::vector<uint8_t> dol = disc.read(dol_off, end);
    write_file(out_path, std::string(dol.begin(), dol.end()));
    std::printf("disc: %s (%s), main.dol %llu bytes\n", game_id.c_str(),
                kGameIds.at(game_id).c_str(), static_cast<unsigned long long>(end));
    return game_id;
}

// --- 3: DolRecomp ------------------------------------------------------------

void run(const std::vector<std::string>& argv, bool quiet) {
    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (quiet)
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int err = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (err != 0)
        fail("cannot run " + argv[0] + ": " + std::strerror(err));
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail(argv[0] + " failed");
}

// --- 4: fix_generated.py -----------------------------------------------------

const char* const kFastDispatchMarker = "DolRecomp constant-time chunk dispatch";

// Replace DolRecomp's linear chunk search with indexed dispatch; 1 if applied.
int fix_generated_dispatch(const fs::path& generated_dir) {
    const fs::path header = generated_dir / "generated.h";
    if (!fs::is_regular_file(header))
        fail("generated header not found: " + header.string());
    const std::string original = read_file(header);
    if (original.find(kFastDispatchMarker) != std::string::npos)
        return 0;
    const std::string head = "static inline int dolrecomp_call(CPUState* ctx, u32 address) {\n";
    const std::string tail = "\n}\n\n";
    const std::string next = "static inline int dolrecomp_run_blocks";
    // DISPATCH_FUNCTION: the first head whose body ends at the first "\n}\n\n"
    // followed by dolrecomp_run_blocks (non-greedy).
    size_t start = original.find(head), body_end = std::string::npos;
    while (start != std::string::npos) {
        for (size_t at = original.find(tail, start + head.size()); at != std::string::npos;
             at = original.find(tail, at + 1))
            if (original.compare(at + tail.size(), next.size(), next) == 0) {
                body_end = at;
                break;
            }
        if (body_end != std::string::npos)
            break;
        start = original.find(head, start + 1);
    }
    if (start == std::string::npos)
        return 0;
    const std::string body =
        original.substr(start + head.size(), body_end - (start + head.size()));

    static const std::regex range(
        R"(if \(address >= (0x[0-9A-F]+)u && address < (0x[0-9A-F]+)u )"
        R"(&& \(\(address - 0x[0-9A-F]+u\) & 3u\) == 0u\) )"
        R"(\{ (func_[0-9A-F]+)\(ctx\); return 1; \})");
    struct Range {
        uint64_t start, end;
        std::string function;
    };
    std::vector<Range> ranges;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), range);
         it != std::sregex_iterator(); ++it)
        ranges.push_back({std::stoull((*it)[1].str(), nullptr, 16),
                          std::stoull((*it)[2].str(), nullptr, 16), (*it)[3].str()});
    if (ranges.size() < 2)
        return 0;
    const std::vector<Range> regular(ranges.begin() + 1, ranges.end());
    const int64_t stride = regular.size() > 1
                               ? static_cast<int64_t>(regular[1].start) -
                                     static_cast<int64_t>(regular[0].start)
                               : 0;
    if (stride <= 0)
        return 0;
    for (size_t i = 0; i < regular.size(); i++)
        if (regular[i].start != regular[0].start + i * static_cast<uint64_t>(stride))
            return 0;
    for (size_t i = 0; i + 1 < regular.size(); i++)
        if (regular[i].end != regular[i + 1].start)
            return 0;

    const Range& first = ranges[0];
    std::string functions;
    for (size_t i = 0; i < regular.size(); i++)
        functions += (i ? ",\n            " : "") + regular[i].function;
    const std::string rs = hex(regular[0].start, 8, true);
    std::string fixed;
    fixed += head;
    fixed += "    if (ppc_host_call(ctx, address)) return 1;\n";
    fixed += std::string("    // ") + kFastDispatchMarker + ".\n";
    fixed += "    if (address >= 0x" + hex(first.start, 8, true) + "u && address < 0x" +
             hex(first.end, 8, true) + "u && ((address - 0x" + hex(first.start, 8, true) +
             "u) & 3u) == 0u) {\n";
    fixed += "        " + first.function + "(ctx);\n";
    fixed += "        return 1;\n";
    fixed += "    }\n";
    fixed += "    if (address >= 0x" + rs + "u && address < 0x" +
             hex(regular.back().end, 8, true) + "u && ((address - 0x" + rs +
             "u) & 3u) == 0u) {\n";
    fixed += "        static const DolRecompFunction functions[] = {\n";
    fixed += "            " + functions + "\n";
    fixed += "        };\n";
    fixed += "        const u32 index = (address - 0x" + rs + "u) / 0x" +
             hex(static_cast<uint64_t>(stride), 1, true) + "u;\n";
    fixed += "        functions[index](ctx);\n";
    fixed += "        return 1;\n";
    fixed += "    }\n";
    fixed += "    return 0;\n";
    fixed += "}\n\n";
    const std::string result =
        original.substr(0, start) + fixed + original.substr(body_end + tail.size());
    if (result == original)
        return 0;
    write_file(header, result);
    return 1;
}

// --- 5: symbols.py -----------------------------------------------------------

constexpr size_t kUsaDolSize = 3156384;

struct DolSection {
    uint32_t offset, address, size;
};

std::vector<DolSection> dol_sections(const std::string& dol, int count) {
    if (dol.size() < 0x100)
        fail("main.dol is cut short");
    const auto* p = reinterpret_cast<const uint8_t*>(dol.data());
    std::vector<DolSection> out;
    for (int i = 0; i < count; i++)
        out.push_back({be32(p + 4 * i), be32(p + 72 + 4 * i), be32(p + 144 + 4 * i)});
    return out;
}

// The text sections' words by address (read_text_words).
class TextWords {
public:
    explicit TextWords(const std::string& dol) {
        const auto* p = reinterpret_cast<const uint8_t*>(dol.data());
        for (const DolSection& s : dol_sections(dol, 7))
            for (uint32_t i = 0; i < s.size; i += 4)
                if (s.offset + i + 4 <= dol.size())
                    words_[s.address + i] = be32(p + s.offset + i);
    }
    std::optional<uint32_t> get(int64_t a) const {
        if (a < 0 || a > 0xFFFFFFFFll)
            return std::nullopt;
        auto it = words_.find(static_cast<uint32_t>(a));
        return it == words_.end() ? std::nullopt : std::optional<uint32_t>(it->second);
    }

private:
    std::unordered_map<uint32_t, uint32_t> words_;
};

bool is_end(uint32_t w) {
    return w == 0x4E800020u || w == 0x4C000064u || ((w >> 26) == 18 && !(w & 1));
}

struct UsaSymbol {
    int64_t address, size;
    std::string name;
    bool operator<(const UsaSymbol& o) const {
        return std::tie(address, size, name) < std::tie(o.address, o.size, o.name);
    }
};
struct Mapped {
    std::string name;
    int64_t usa, here;
};

std::vector<Mapped> track(const std::vector<UsaSymbol>& usa, const TextWords& words) {
    auto boundary = [&](int64_t a) {
        const auto p = words.get(a - 4), w = words.get(a);
        return w && *w != 0 && (!p || *p == 0 || is_end(*p));
    };
    auto fits = [&](int64_t a, int64_t s) {
        const int64_t end = a + s;
        if (!boundary(a))
            return false;
        const auto last = words.get(end - 4);
        if (!last || !is_end(*last))
            return false;
        const auto after = words.get(end);
        return !after || *after == 0 || boundary(end);
    };
    int64_t shift = 0;
    std::vector<Mapped> out;
    for (size_t i = 0; i < usa.size(); i++) {
        const int64_t a = usa[i].address, s = usa[i].size;
        auto score = [&](int64_t d) {
            if (!fits(a + d, s))
                return 0;
            int n = 1;
            for (size_t j : {size_t{1}, size_t{2}})
                if (i + j < usa.size() && fits(usa[i + j].address + d, usa[i + j].size))
                    n++;
            return n;
        };
        std::optional<int64_t> best;
        if (score(shift) >= 2)
            best = shift;
        int64_t step = 4;
        while (!best && step < 0x2000) {
            for (int64_t d : {shift + step, shift - step})
                if (score(d) == 3) {
                    best = d;
                    break;
                }
            step += 4;
        }
        if (best) {
            shift = *best;
            out.push_back({usa[i].name, a, a + *best});
        }
    }
    return out;
}

void make_symbols(const fs::path& symbols_path, const fs::path& dol_path,
                  const fs::path& out_dir) {
    const std::string dol = read_file(dol_path);
    static const std::regex symbol_re(
        R"(^(\S+)\s*=\s*\.text:0x([0-9A-Fa-f]+);.*type:function size:0x([0-9A-Fa-f]+))");
    static const std::regex unnamed_re(R"(^(fn|func|lbl|sub|FUN)_[0-9A-Fa-f]+$)");
    std::vector<UsaSymbol> usa;
    std::istringstream lines(read_file(symbols_path));
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, symbol_re))
            usa.push_back({static_cast<int64_t>(std::stoull(m[2].str(), nullptr, 16)),
                           static_cast<int64_t>(std::stoull(m[3].str(), nullptr, 16)),
                           m[1].str()});
    }
    std::sort(usa.begin(), usa.end());

    std::vector<Mapped> mapped;
    if (dol.size() == kUsaDolSize) {
        for (const UsaSymbol& u : usa)
            mapped.push_back({u.name, u.address, u.address});
    } else {
        mapped = track(usa, TextWords(dol));
    }

    fs::create_directories(out_dir);
    std::string tsv;
    for (const Mapped& m : mapped)
        tsv += m.name + "\t" + hex(static_cast<uint64_t>(m.usa), 8, false) + "\t" +
               hex(static_cast<uint64_t>(m.here), 8, false) + "\n";
    write_file(out_dir / "symbols.tsv", tsv);

    std::set<std::string> seen;
    std::vector<std::pair<int64_t, std::string>> named;
    for (const Mapped& m : mapped) {
        if (std::regex_match(m.name, unnamed_re) || seen.count(m.name))
            continue;
        seen.insert(m.name);
        named.emplace_back(m.here, m.name);
    }
    std::sort(named.begin(), named.end());
    std::string inc = "// Generated by tools/symbols.py from the user's main.dol -- do not edit.\n";
    inc += "// " + std::to_string(named.size()) + " named functions of " +
           std::to_string(usa.size()) + " in the decomp map.\n\n";
    for (const auto& [p, n] : named)
        inc += "    { 0x" + hex(static_cast<uint64_t>(p), 8, true) + "u, \"" + n + "\" },\n";
    write_file(out_dir / "sdk_symbols.inc", inc);

    std::set<int64_t> shifts;
    for (const Mapped& m : mapped)
        shifts.insert(m.here - m.usa);
    std::printf("symbols: %zu/%zu functions placed, %zu named, %zu distinct shifts\n",
                mapped.size(), usa.size(), named.size(), shifts.size());
}

// --- 6: gamedata.py ----------------------------------------------------------

class Dol {
public:
    explicit Dol(const fs::path& path) : data_(read_file(path)) {
        entry = be32(reinterpret_cast<const uint8_t*>(data_.data()) + 0xE0);
        for (const DolSection& s : dol_sections(data_, 18))
            if (s.size != 0)
                sections_.push_back(s);
    }
    uint32_t word(int64_t a) const {
        for (const DolSection& s : sections_)
            if (s.address <= a && a < int64_t{s.address} + s.size) {
                const int64_t at = s.offset + (a - s.address);
                if (at + 4 > static_cast<int64_t>(data_.size()))
                    break;
                return be32(reinterpret_cast<const uint8_t*>(data_.data()) + at);
            }
        fail("0x" + hex(static_cast<uint64_t>(a), 8, true) + " is outside the DOL");
    }
    uint32_t entry = 0;

private:
    std::string data_;
    std::vector<DolSection> sections_;
};

struct DForm {
    uint32_t op, rt, ra;
    int32_t simm;
};
DForm dform(uint32_t w) {
    const int32_t imm = static_cast<int32_t>(w & 0xFFFF);
    return {w >> 26, (w >> 21) & 31, (w >> 16) & 31, imm & 0x8000 ? imm - 0x10000 : imm};
}

int64_t branch_target(int64_t a, uint32_t w) {
    int64_t li = w & 0x03FFFFFC;
    if (li & 0x02000000)
        li -= 0x04000000;
    return a + li;
}

std::pair<int64_t, int64_t> registers(const Dol& dol) {
    const int64_t init = branch_target(dol.entry, dol.word(dol.entry));
    std::map<uint32_t, int64_t> hi, value;
    for (int i = 0; i < 16; i++) {
        const uint32_t w = dol.word(init + 4 * i);
        const DForm d = dform(w);
        if (d.op == 15 && d.ra == 0)
            hi[d.rt] = int64_t{(d.simm & 0xFFFF)} << 16;
        else if (d.op == 24 && hi.count(d.rt))
            value[d.ra] = hi[d.rt] | (w & 0xFFFF);
    }
    if (!value.count(2) || !value.count(13))
        fail("gamedata: r2/r13 not found in __init_registers");
    return {value[2], value[13]};
}

std::tuple<int64_t, int64_t, int64_t> gx_data(const Dol& dol, int64_t gx_begin) {
    std::optional<uint32_t> base;
    uint32_t reg = 0;
    int64_t offset = 0;
    for (int i = 0; i < 24; i++) {
        const DForm d = dform(dol.word(gx_begin + 4 * i));
        if (d.op == 32 && !base && (d.ra == 2 || d.ra == 13)) {
            base = d.ra;
            reg = d.rt;
            offset = d.simm;
        } else if (d.op == 32 && base && d.ra == reg && d.simm > 0x100) {
            return {*base, offset, d.simm};
        }
    }
    fail("gamedata: GX state access not found in GXBegin");
}

int64_t flush_prim_when_zero(const Dol& dol, int64_t func, int64_t flush_prim) {
    for (int i = 0; i < 32; i++) {
        const int64_t a = func + 4 * i;
        const uint32_t w = dol.word(a);
        if (w >> 26 == 18 && (w & 1) && branch_target(a, w) == flush_prim) {
            const uint32_t bo = (dol.word(a - 4) >> 21) & 31;
            return bo == 4 ? 1 : 0;
        }
    }
    fail("gamedata: __GXSendFlushPrim call not found in GXCallDisplayList");
}

int64_t r13_arg_before_call(const Dol& dol, int64_t func, int64_t callee, int64_t r13) {
    std::optional<int64_t> last;
    for (int i = 0; i < 96; i++) {
        const int64_t a = func + 4 * i;
        const uint32_t w = dol.word(a);
        const DForm d = dform(w);
        if (d.op == 14 && d.rt == 3 && d.ra == 13)
            last = r13 + d.simm;
        if (d.op == 18 && (w & 1) && branch_target(a, w) == callee && last)
            return *last & 0xFFFFFFFF;
    }
    fail("gamedata: queue argument not found");
}

std::vector<int64_t> r13_stores(const Dol& dol, int64_t func, int64_t r13, size_t count) {
    std::vector<int64_t> found;
    for (int i = 0; i < 8; i++) {
        const DForm d = dform(dol.word(func + 4 * i));
        if (d.op == 36 && d.ra == 13) {
            found.push_back((r13 + d.simm) & 0xFFFFFFFF);
            if (found.size() == count)
                return found;
        }
    }
    fail("gamedata: r13 stores not found");
}

void make_game_data(const fs::path& dol_path, const fs::path& tsv_path,
                    const fs::path& out_path) {
    const Dol dol(dol_path);
    std::map<std::string, int64_t> sym;
    std::istringstream lines(read_file(tsv_path));
    for (std::string line; std::getline(lines, line);) {
        const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos)
            continue;
        sym.emplace(line.substr(0, t1), std::stoll(line.substr(t2 + 1), nullptr, 16));
    }
    auto at = [&](const char* name) {
        auto it = sym.find(name);
        if (it == sym.end())
            fail(std::string("gamedata: ") + name + " not placed");
        return it->second;
    };
    const auto [r2, r13] = registers(dol);
    const auto [gx_base, gx_offset, gx_dirty] = gx_data(dol, at("GXBegin"));
    const int64_t finish_queue =
        r13_arg_before_call(dol, at("GXDrawDone"), at("OSSleepThread"), r13);
    const std::vector<int64_t> dsp = r13_stores(dol, at("dspInitCallback"), r13, 2);
    const int64_t flush_zero =
        flush_prim_when_zero(dol, at("GXCallDisplayList"), at("__GXSendFlushPrim"));

    const std::vector<std::tuple<const char*, int64_t, const char*>> values = {
        {"ED_GUEST_R2", r2, "small-data base (r2), set by __init_registers"},
        {"ED_GUEST_R13", r13, "small-data base (r13), set by __init_registers"},
        {"ED_GX_DATA_BASE_REG", gx_base, "register GXBegin loads the GX state pointer through"},
        {"ED_GX_DATA_SDA_OFFSET", gx_offset, "its offset from that register (signed)"},
        {"ED_GX_DIRTY_STATE_OFFSET", gx_dirty, "dirty-state word in the GX state block"},
        {"ED_GX_FINISH_QUEUE", finish_queue,
         "FinishQueue, the OSThreadQueue GXDrawDone sleeps on"},
        {"ED_MUSYX_DSP_DONE", dsp[0], "salDspIsDone, set by dspInitCallback"},
        {"ED_MUSYX_DSP_INIT_DONE", dsp[1], "salDspInitIsDone, its second store"},
        {"ED_GX_FLUSH_PRIM_WHEN_ZERO", flush_zero,
         "1: GX sends the flush primitive when the state block's first word is 0"},
    };
    std::string out = "// Generated by tools/gamedata.py from the user's main.dol -- do not edit.\n";
    out += "#pragma once\n\n";
    for (const auto& [name, value, note] : values) {
        const std::string literal = value < 0 ? "(" + std::to_string(value) + ")"
                                              : "0x" + hex(static_cast<uint64_t>(value), 8, true) + "u";
        out += std::string("#define ") + name + " " + literal + "  // " + note + "\n";
    }
    write_file(out_path, out);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path disc, out, dolrecomp, symbols;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "edprep: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--out")
            out = value();
        else if (a == "--dolrecomp")
            dolrecomp = value();
        else if (a == "--symbols")
            symbols = value();
        else if (a == "--jobs")
            jobs = static_cast<unsigned>(std::max(1, std::atoi(value().c_str())));
        else if (disc.empty())
            disc = a;
        else {
            std::fprintf(stderr, "edprep: unexpected argument %s\n", a.c_str());
            return 2;
        }
    }
    if (disc.empty() || out.empty() || dolrecomp.empty() || symbols.empty()) {
        std::fprintf(stderr,
                     "usage: edprep <disc image> --out <dir> --dolrecomp <binary> "
                     "--symbols <symbols.txt> [--jobs N]\n");
        return 2;
    }
    try {
        out = fs::absolute(out);
        const fs::path tmp = out.parent_path() / (out.filename().string() + ".tmp");
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        std::printf("step 1/6 reading the disc\n");
        std::fflush(stdout);
        const fs::path dol = tmp / "main.dol";
        const std::string game_id = extract_dol(disc, dol);
        std::printf("step 2/6 translating main.dol\n");
        std::fflush(stdout);
        run({dolrecomp.string(), "--gamecube", dol.string(), tmp.string(), "-j",
             std::to_string(jobs)},
            true);
        fs::remove_all(out);
        fs::rename(tmp / "generated", out);
        fs::copy_file(dol, out / "main.dol", fs::copy_options::overwrite_existing);
        fs::remove_all(tmp);
        write_file(out / "game_id.txt", game_id + "\n");
        std::printf("step 3/6 dispatch fixes: %d\n", fix_generated_dispatch(out));
        std::printf("step 4/6 symbols\n");
        std::fflush(stdout);
        make_symbols(symbols, out / "main.dol", out);
        std::printf("step 5/6 game data\n");
        make_game_data(out / "main.dol", out / "symbols.tsv", out / "game_data.h");
        std::printf("step 6/6 done: %s\n", out.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "edprep: %s\n", e.what());
        return 1;
    }
    return 0;
}
