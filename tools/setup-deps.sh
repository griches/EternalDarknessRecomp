#!/bin/sh
# Fetch the pinned dependencies into .deps/. Safe to rerun: existing checkouts
# are moved to the pin, not recloned.
#
#   DolRecomp  ExpansionPak upstream, the commit whose CPU header GXRuntime
#              adopted (2026-07-17); later commits change the generated ABI.
#   GXRuntime  aharonahdoot, the standalone runtime (CPU core, devices, Aurora).
set -e
cd "$(dirname "$0")/.."
mkdir -p .deps

pin() {
  dir=.deps/$1; url=$2; rev=$3
  [ -d "$dir/.git" ] || git clone --quiet "$url" "$dir"
  git -C "$dir" fetch --quiet origin
  git -C "$dir" checkout --quiet "$rev"
  echo "$1 $(git -C "$dir" log -1 --format='%h %cs')"
}

pin DolRecomp https://github.com/ExpansionPak/DolRecomp.git 2595de5
pin GXRuntime https://github.com/aharonahdoot/GXRuntime.git 8a47b0e

# Recompiler fixes: paired-single ops whose destination is also a source
# (ps_merge10 f7,f7,f7 in PSQUATMultiply), single-precision ops filling ps1.
if git -C .deps/DolRecomp apply --check ../../patches/dolrecomp/eternal-darkness.patch 2>/dev/null; then
  git -C .deps/DolRecomp apply ../../patches/dolrecomp/eternal-darkness.patch
  echo "DolRecomp: applied patches/dolrecomp/eternal-darkness.patch"
else
  echo "DolRecomp: patch already applied (or conflicts; check git -C .deps/DolRecomp status)"
fi

# Port fixes to GXRuntime (texmap selection, near-plane snapping, EFB present).
if git -C .deps/GXRuntime apply --check ../../patches/gxruntime/eternal-darkness.patch 2>/dev/null; then
  git -C .deps/GXRuntime apply ../../patches/gxruntime/eternal-darkness.patch
  echo "GXRuntime: applied patches/gxruntime/eternal-darkness.patch"
else
  echo "GXRuntime: patch already applied (or conflicts; check git -C .deps/GXRuntime status)"
fi
