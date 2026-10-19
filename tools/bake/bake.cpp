// Light map baker for Eternal Darkness: ray-traces a room captured with
// DOL_BAKE_CAPTURE (the renderer's bake_capture.cpp) and writes sharper light
// maps, Dolphin-named, for a texture pack folder.
//
// The disc's light maps are a few texels per wall and blocky (CMPR). This
// keeps each one's light (the average over every original texel stays the
// same) and adds what the geometry says should be inside it at SCALE times
// the resolution: the sun's hard shadows (the captured lights' far one,
// through the room's own geometry) and ambient occlusion in corners. A wrong
// guess can't relight the room; it can only move detail within a texel.
//
//   bake <capture dir> <out dir> [scale 4] [frames...]
//   writes <out dir>/<name>.pam (RGBA); tools/bake/bake.sh makes PNGs.
// Environment: BAKE_ONLY=<substring of a map name>, BAKE_AO_RAYS (48),
// BAKE_AO_RADIUS (120), BAKE_SUN_RAYS (16), BAKE_SUN_SOFT (0.012 rad),
// BAKE_AMBIENT (0.35), BAKE_MAX_GAIN (3).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 a) {
  const float l = std::sqrt(dot(a, a));
  return l > 0 ? a * (1.f / l) : a;
}
V3 vmin(V3 a, V3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
V3 vmax(V3 a, V3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

float env(const char* name, float fallback) {
  const char* e = std::getenv(name);
  return e != nullptr ? static_cast<float>(std::atof(e)) : fallback;
}

struct Vert {
  V3 p, n;
  float u = 0, v = 0;
};
struct Tri {
  Vert v[3];
  int map = -1;  // light map index, -1 none
};

// --- BVH (median split) ----------------------------------------------------
struct Node {
  V3 lo, hi;
  int left = -1, right = -1, first = 0, count = 0;
};
struct Bvh {
  std::vector<Node> nodes;
  std::vector<std::array<V3, 3>> tris;
  std::vector<int> order;

  void build(const std::vector<std::array<V3, 3>>& t) {
    tris = t;
    order.resize(tris.size());
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = static_cast<int>(i);
    nodes.clear();
    nodes.reserve(tris.size() * 2);
    split(0, static_cast<int>(order.size()));
  }
  int split(int first, int count) {
    Node node;
    node.lo = {1e30f, 1e30f, 1e30f};
    node.hi = {-1e30f, -1e30f, -1e30f};
    V3 clo = node.lo, chi = node.hi;
    for (int i = first; i < first + count; ++i) {
      const auto& t = tris[order[i]];
      for (const auto& p : t) {
        node.lo = vmin(node.lo, p);
        node.hi = vmax(node.hi, p);
      }
      const V3 c = (t[0] + t[1] + t[2]) * (1.f / 3.f);
      clo = vmin(clo, c);
      chi = vmax(chi, c);
    }
    const int index = static_cast<int>(nodes.size());
    nodes.push_back(node);
    if (count <= 4) {
      nodes[index].first = first;
      nodes[index].count = count;
      return index;
    }
    const V3 ext = chi - clo;
    const int axis = ext.x > ext.y && ext.x > ext.z ? 0 : ext.y > ext.z ? 1 : 2;
    const auto key = [&](int i) {
      const auto& t = tris[i];
      const V3 c = t[0] + t[1] + t[2];
      return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
    };
    const int mid = first + count / 2;
    std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
                     [&](int a, int b) { return key(a) < key(b); });
    const int l = split(first, mid - first);
    const int r = split(mid, first + count - mid);
    nodes[index].left = l;
    nodes[index].right = r;
    return index;
  }
  static bool box(const Node& n, V3 o, V3 inv, float tmax) {
    float t0 = 0, t1 = tmax;
    const float lo[3]{n.lo.x, n.lo.y, n.lo.z}, hi[3]{n.hi.x, n.hi.y, n.hi.z}, oo[3]{o.x, o.y, o.z},
        ii[3]{inv.x, inv.y, inv.z};
    for (int a = 0; a < 3; ++a) {
      float tn = (lo[a] - oo[a]) * ii[a], tf = (hi[a] - oo[a]) * ii[a];
      if (tn > tf)
        std::swap(tn, tf);
      t0 = std::max(t0, tn);
      t1 = std::min(t1, tf);
      if (t0 > t1)
        return false;
    }
    return true;
  }
  // Nearest hit distance below tmax, or tmax.
  float hit(V3 o, V3 d, float tmax) const {
    const V3 inv{1.f / d.x, 1.f / d.y, 1.f / d.z};
    int stack[96];
    int sp = 0;
    stack[sp++] = 0;
    float best = tmax;
    while (sp > 0) {
      const Node& n = nodes[stack[--sp]];
      if (!box(n, o, inv, best))
        continue;
      if (n.count > 0) {
        for (int i = n.first; i < n.first + n.count; ++i) {
          const auto& t = tris[order[i]];
          const V3 e1 = t[1] - t[0], e2 = t[2] - t[0];
          const V3 pv = cross(d, e2);
          const float det = dot(e1, pv);
          if (std::fabs(det) < 1e-9f)
            continue;
          const float id = 1.f / det;
          const V3 tv = o - t[0];
          const float u = dot(tv, pv) * id;
          if (u < 0 || u > 1)
            continue;
          const V3 qv = cross(tv, e1);
          const float v = dot(d, qv) * id;
          if (v < 0 || u + v > 1)
            continue;
          const float tt = dot(e2, qv) * id;
          if (tt > 1e-3f && tt < best)
            best = tt;
        }
      } else if (sp < 94) {
        stack[sp++] = n.left;
        stack[sp++] = n.right;
      }
    }
    return best;
  }
};

struct Map {
  std::string name;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  std::vector<int> tris;
};

struct Light {
  int index;
  float color[4], cosatt[3], distatt[3], pos[3], dir[3];
};
} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "bake <capture dir> <out dir> [scale] [frame files...]\n");
    return 1;
  }
  const std::string in = argv[1], out = argv[2];
  const int scale = argc > 3 ? std::atoi(argv[3]) : 4;
  std::vector<std::string> frames;
  for (int i = 4; i < argc; ++i)
    frames.push_back(argv[i]);
  if (frames.empty())
    for (const auto& e : std::filesystem::directory_iterator(in))
      if (e.path().filename().string().rfind("frame-", 0) == 0)
        frames.push_back(e.path().string());
  std::sort(frames.begin(), frames.end());
  if (frames.empty()) {
    std::fprintf(stderr, "no frame-*.txt in %s\n", in.c_str());
    return 1;
  }
  // One frame for now: everything in its view space.
  const std::string frameFile = frames.front();
  FILE* f = std::fopen(frameFile.c_str(), "r");
  if (f == nullptr)
    return 1;
  std::vector<Tri> tris;
  std::vector<std::array<float, 12>> drawMtx;
  std::vector<int> triDraw;
  std::vector<Light> lights;
  std::map<std::string, int> mapIndex;
  std::vector<Map> maps;
  char line[512];
  int curMap = -1, curDraw = -1, pending = 0;
  Tri cur;
  int vi = 0;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == 'D') {
      char name[128];
      std::array<float, 12> m{};
      int n = 0;
      if (std::sscanf(line, "D %127s %f %f %f %f %f %f %f %f %f %f %f %f %d", name, &m[0], &m[1], &m[2], &m[3], &m[4],
                      &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11], &n) != 14)
        continue;
      drawMtx.push_back(m);
      curDraw = static_cast<int>(drawMtx.size()) - 1;
      pending = n * 3;
      vi = 0;
      curMap = -1;
      if (std::strcmp(name, "-") != 0) {
        auto it = mapIndex.find(name);
        if (it == mapIndex.end()) {
          Map mp;
          mp.name = name;
          std::sscanf(name, "tex1_%dx%d_", &mp.w, &mp.h);
          FILE* r = std::fopen((in + "/lm/" + name + ".rgba").c_str(), "rb");
          if (r != nullptr) {
            mp.rgba.resize(static_cast<size_t>(mp.w) * mp.h * 4);
            if (std::fread(mp.rgba.data(), 1, mp.rgba.size(), r) != mp.rgba.size())
              mp.rgba.clear();
            std::fclose(r);
          }
          it = mapIndex.emplace(name, static_cast<int>(maps.size())).first;
          maps.push_back(std::move(mp));
        }
        curMap = it->second;
      }
    } else if (line[0] == 'V' && pending > 0) {
      Vert v;
      std::sscanf(line, "V %f %f %f %f %f %f %f %f", &v.p.x, &v.p.y, &v.p.z, &v.n.x, &v.n.y, &v.n.z, &v.u, &v.v);
      cur.v[vi++] = v;
      --pending;
      if (vi == 3) {
        cur.map = curMap;
        tris.push_back(cur);
        triDraw.push_back(curDraw);
        vi = 0;
      }
    } else if (line[0] == 'L') {
      Light l{};
      if (std::sscanf(line, "L %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &l.index, &l.color[0],
                      &l.color[1], &l.color[2], &l.color[3], &l.cosatt[0], &l.cosatt[1], &l.cosatt[2], &l.distatt[0],
                      &l.distatt[1], &l.distatt[2], &l.pos[0], &l.pos[1], &l.pos[2], &l.dir[0], &l.dir[1],
                      &l.dir[2]) == 17)
        lights.push_back(l);
    }
  }
  std::fclose(f);

  // The room: the draws on the matrix most triangles use (characters move).
  std::map<std::array<float, 12>, size_t> mtxCount;
  for (size_t i = 0; i < tris.size(); ++i)
    ++mtxCount[drawMtx[triDraw[i]]];
  std::array<float, 12> room{};
  size_t best = 0;
  for (const auto& [m, n] : mtxCount)
    if (n > best) {
      best = n;
      room = m;
    }
  std::vector<std::array<V3, 3>> occluders;
  for (size_t i = 0; i < tris.size(); ++i) {
    if (drawMtx[triDraw[i]] != room)
      continue;
    occluders.push_back({tris[i].v[0].p, tris[i].v[1].p, tris[i].v[2].p});
    if (tris[i].map >= 0)
      maps[tris[i].map].tris.push_back(static_cast<int>(i));
  }
  Bvh bvh;
  bvh.build(occluders);
  V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const auto& t : occluders)
    for (const auto& p : t) {
      lo = vmin(lo, p);
      hi = vmax(hi, p);
    }
  const float extent = std::sqrt(dot(hi - lo, hi - lo));

  // The sun: the light farthest away (a directional light sits far off).
  V3 sun{0, 1, 0};
  float far = 0;
  for (const auto& l : lights) {
    const V3 p{l.pos[0], l.pos[1], l.pos[2]};
    const float d = std::sqrt(dot(p, p));
    if (d > far) {
      far = d;
      sun = normalize(p);
    }
  }
  const bool haveSun = far > extent * 20.f;
  std::fprintf(stderr, "%zu triangles (%zu room), %zu maps, %zu lights, room size %.0f, sun %s (%.2f %.2f %.2f)\n",
               tris.size(), occluders.size(), maps.size(), lights.size(), extent, haveSun ? "yes" : "no", sun.x, sun.y,
               sun.z);

  const int aoRays = static_cast<int>(env("BAKE_AO_RAYS", 48));
  const float aoRadius = env("BAKE_AO_RADIUS", 120.f);
  const int sunRays = static_cast<int>(env("BAKE_SUN_RAYS", 16));
  const float sunSoft = env("BAKE_SUN_SOFT", 0.012f);
  const float ambient = env("BAKE_AMBIENT", 0.35f);
  const float maxGain = env("BAKE_MAX_GAIN", 3.f);
  const char* only = std::getenv("BAKE_ONLY");
  std::filesystem::create_directories(out);

  for (auto& mp : maps) {
    if (mp.rgba.empty() || mp.tris.empty() || (only != nullptr && mp.name.find(only) == std::string::npos))
      continue;
    const int W = mp.w * scale, H = mp.h * scale;
    std::vector<V3> pos(static_cast<size_t>(W) * H), nrm(pos.size());
    std::vector<uint8_t> covered(pos.size(), 0);
    // Lay the triangles out in light map space (clamped, as it is sampled).
    for (const int ti : mp.tris) {
      const Tri& t = tris[ti];
      float x[3], y[3];
      for (int k = 0; k < 3; ++k) {
        x[k] = t.v[k].u * W - 0.5f;
        y[k] = t.v[k].v * H - 0.5f;
      }
      const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
      if (std::fabs(area) < 1e-8f)
        continue;
      const int x0 = std::max(0, static_cast<int>(std::floor(std::min({x[0], x[1], x[2]}))) - 1);
      const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({x[0], x[1], x[2]}))) + 1);
      const int y0 = std::max(0, static_cast<int>(std::floor(std::min({y[0], y[1], y[2]}))) - 1);
      const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]}))) + 1);
      const V3 fn = normalize(cross(t.v[1].p - t.v[0].p, t.v[2].p - t.v[0].p));
      for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
          const float w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
          const float w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
          const float w2 = 1.f - w0 - w1;
          const float tol = -0.02f;
          if (w0 < tol || w1 < tol || w2 < tol)
            continue;
          const size_t i = static_cast<size_t>(py) * W + px;
          pos[i] = t.v[0].p * w0 + t.v[1].p * w1 + t.v[2].p * w2;
          V3 n = t.v[0].n * w0 + t.v[1].n * w1 + t.v[2].n * w2;
          n = dot(n, n) > 0.f ? normalize(n) : fn;
          nrm[i] = n;
          covered[i] = 1;
        }
    }
    // What the geometry says, per texel: ambient occlusion times (ambient +
    // the sun where it reaches).
    std::vector<float> detail(pos.size(), 0.f);
    std::atomic<int> nextRow{0};
    const auto work = [&] {
      std::mt19937 rng(1234u + static_cast<unsigned>(nextRow.load()));
      std::uniform_real_distribution<float> uni(0.f, 1.f);
      for (int py; (py = nextRow.fetch_add(1)) < H;)
        for (int px = 0; px < W; ++px) {
          const size_t i = static_cast<size_t>(py) * W + px;
          if (!covered[i])
            continue;
          const V3 n = nrm[i];
          const V3 o = pos[i] + n * (extent * 2e-4f);
          const V3 t1 = normalize(std::fabs(n.x) > 0.5f ? cross(n, V3{0, 1, 0}) : cross(n, V3{1, 0, 0}));
          const V3 t2 = cross(n, t1);
          float occ = 0.f;
          for (int r = 0; r < aoRays; ++r) {
            const float a = uni(rng), b = uni(rng);
            const float s = std::sqrt(a), phi = 6.2831853f * b;
            const V3 d = normalize(t1 * (s * std::cos(phi)) + t2 * (s * std::sin(phi)) + n * std::sqrt(1.f - a));
            const float t = bvh.hit(o, d, aoRadius);
            if (t < aoRadius)
              occ += 1.f - t / aoRadius;
          }
          const float ao = 1.f - occ / static_cast<float>(aoRays);
          float lit = 0.f;
          if (haveSun) {
            const float ndl = dot(n, sun);
            if (ndl > 0.f) {
              const V3 s1 = normalize(std::fabs(sun.x) > 0.5f ? cross(sun, V3{0, 1, 0}) : cross(sun, V3{1, 0, 0}));
              const V3 s2 = cross(sun, s1);
              int open = 0;
              for (int r = 0; r < sunRays; ++r) {
                const float a = uni(rng) * 6.2831853f, rr = sunSoft * std::sqrt(uni(rng));
                const V3 d = normalize(sun + s1 * (rr * std::cos(a)) + s2 * (rr * std::sin(a)));
                if (bvh.hit(o, d, extent * 4.f) >= extent * 4.f)
                  ++open;
              }
              lit = ndl * static_cast<float>(open) / static_cast<float>(sunRays);
            }
          }
          detail[i] = ao * (ambient + lit);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < std::max(1u, std::thread::hardware_concurrency()); ++k)
      pool.emplace_back(work);
    for (auto& t : pool)
      t.join();

    // Per original texel, the detail's average; spread back over the high
    // texels the way the original is sampled (bilinear between texel
    // centres), so detail / that keeps each texel's light.
    std::vector<float> lowSum(static_cast<size_t>(mp.w) * mp.h, 0.f), lowN(lowSum.size(), 0.f);
    for (int py = 0; py < H; ++py)
      for (int px = 0; px < W; ++px) {
        const size_t i = static_cast<size_t>(py) * W + px;
        if (!covered[i])
          continue;
        const size_t j = static_cast<size_t>(py / scale) * mp.w + px / scale;
        lowSum[j] += detail[i];
        lowN[j] += 1.f;
      }
    std::vector<float> low(lowSum.size(), -1.f);
    for (size_t j = 0; j < low.size(); ++j)
      if (lowN[j] > 0.f)
        low[j] = lowSum[j] / lowN[j];
    const auto sample_low = [&](int x, int y) {
      x = std::clamp(x, 0, mp.w - 1);
      y = std::clamp(y, 0, mp.h - 1);
      return low[static_cast<size_t>(y) * mp.w + x];
    };
    const auto sample_map = [&](int x, int y, int c) {
      x = std::clamp(x, 0, mp.w - 1);
      y = std::clamp(y, 0, mp.h - 1);
      return static_cast<float>(mp.rgba[(static_cast<size_t>(y) * mp.w + x) * 4 + c]);
    };
    std::vector<uint8_t> result(static_cast<size_t>(W) * H * 4);
    for (int py = 0; py < H; ++py)
      for (int px = 0; px < W; ++px) {
        const float fx = (px + 0.5f) / scale - 0.5f, fy = (py + 0.5f) / scale - 0.5f;
        const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
        const float ax = fx - ix, ay = fy - iy;
        const float wts[4]{(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
        const int xs[4]{ix, ix + 1, ix, ix + 1}, ys[4]{iy, iy, iy + 1, iy + 1};
        const size_t i = static_cast<size_t>(py) * W + px;
        float gain = 1.f;
        if (covered[i]) {
          float l = 0.f, lw = 0.f;
          for (int k = 0; k < 4; ++k) {
            const float v = sample_low(xs[k], ys[k]);
            if (v >= 0.f) {
              l += v * wts[k];
              lw += wts[k];
            }
          }
          if (lw > 0.f && l > 1e-4f)
            gain = std::clamp(detail[i] / (l / lw), 0.f, maxGain);
        }
        if (std::getenv("BAKE_SHOW") != nullptr) {  // the traced detail itself
          const auto g = static_cast<uint8_t>(std::clamp(std::lround(detail[i] * 180.f), 0l, 255l));
          result[i * 4] = result[i * 4 + 1] = result[i * 4 + 2] = covered[i] ? g : 0;
          result[i * 4 + 3] = 255;
          continue;
        }
        for (int c = 0; c < 4; ++c) {
          float v = 0.f;
          for (int k = 0; k < 4; ++k)
            v += sample_map(xs[k], ys[k], c) * wts[k];
          if (c < 3)
            v *= gain;
          result[i * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(v), 0l, 255l));
        }
      }
    FILE* o = std::fopen((out + "/" + mp.name + ".pam").c_str(), "wb");
    if (o != nullptr) {
      std::fprintf(o, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", W, H);
      std::fwrite(result.data(), 1, result.size(), o);
      std::fclose(o);
    }
    size_t cov = 0;
    for (auto c : covered)
      cov += c;
    std::fprintf(stderr, "%s %dx%d -> %dx%d, %zu triangles, %.0f%% covered\n", mp.name.c_str(), mp.w, mp.h, W, H,
                 mp.tris.size(), 100.0 * cov / covered.size());
  }
  return 0;
}
