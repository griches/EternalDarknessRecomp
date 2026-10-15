#!/usr/bin/env python3
"""Apply required dispatch fixes to generated DolRecomp C.

The whole floating-point / paired-single / load-store operand-truncation and
split-lane defect class is now fixed in the DolRecomp emitter: every
FP op is emitted as a call to a shared helper (ppc_fadds/ppc_ps_madd_op/
ppc_lfs_op/ppc_frsp/...) that mirrors Dolphin's interpreter bit-exactly, so
none of the old post-hoc FP regexes (SINGLE_BINARY / PS_ROUND / LFS_ASSIGN /
FMR_ASSIGN / PS_FMA_*) apply any more -- and FMR_ASSIGN was in fact WRONG once
the semantics were pinned down (Dolphin's fmr is PS0-only, it must NOT mirror
to ps1). They are deleted here; the DolRecomp oracle differential
(tests/oracle, tests/diff) is the standing guard for FP correctness.

The one transform still required is the constant-time chunk dispatch: DolRecomp
still emits `dolrecomp_call()` as a linear range chain (see
KNOWLEDGE/recomp-codegen.md, upstream emit_dispatch_helpers). Delete this too
once the emitter grows section-indexed dispatch.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


DISPATCH_RANGE = re.compile(
    r"if \(address >= (0x[0-9A-F]+)u && address < (0x[0-9A-F]+)u "
    r"&& \(\(address - 0x[0-9A-F]+u\) & 3u\) == 0u\) "
    r"\{ (func_[0-9A-F]+)\(ctx\); return 1; \}"
)

DISPATCH_FUNCTION = re.compile(
    r"static inline int dolrecomp_call\(CPUState\* ctx, u32 address\) \{\n"
    r"(?P<body>.*?)"
    r"\n\}\n\n"
    r"(?=static inline int dolrecomp_run_blocks)",
    re.DOTALL,
)

FAST_DISPATCH_MARKER = "DolRecomp constant-time chunk dispatch"


def _constant_time_dispatch(match: re.Match) -> str:
    body = match.group("body")
    ranges = [
        (int(start, 16), int(end, 16), function)
        for start, end, function in DISPATCH_RANGE.findall(body)
    ]
    if len(ranges) < 2:
        return match.group(0)

    regular = ranges[1:]
    stride = regular[1][0] - regular[0][0] if len(regular) > 1 else 0
    if stride <= 0 or any(
        item[0] != regular[0][0] + index * stride
        for index, item in enumerate(regular)
    ):
        return match.group(0)
    if any(item[1] != regular[index + 1][0] for index, item in enumerate(regular[:-1])):
        return match.group(0)

    first_start, first_end, first_function = ranges[0]
    regular_start = regular[0][0]
    regular_end = regular[-1][1]
    functions = ",\n            ".join(item[2] for item in regular)
    return (
        "static inline int dolrecomp_call(CPUState* ctx, u32 address) {\n"
        "    if (ppc_host_call(ctx, address)) return 1;\n"
        f"    // {FAST_DISPATCH_MARKER}.\n"
        f"    if (address >= 0x{first_start:08X}u && address < 0x{first_end:08X}u && "
        f"((address - 0x{first_start:08X}u) & 3u) == 0u) {{\n"
        f"        {first_function}(ctx);\n"
        "        return 1;\n"
        "    }\n"
        f"    if (address >= 0x{regular_start:08X}u && address < 0x{regular_end:08X}u && "
        "((address - "
        f"0x{regular_start:08X}u) & 3u) == 0u) {{\n"
        "        static const DolRecompFunction functions[] = {\n"
        f"            {functions}\n"
        "        };\n"
        f"        const u32 index = (address - 0x{regular_start:08X}u) / "
        f"0x{stride:X}u;\n"
        "        functions[index](ctx);\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n\n"
    )


def fix_generated_dispatch(generated_dir: Path) -> int:
    """Replace DolRecomp's linear chunk search with indexed dispatch."""
    header = generated_dir / "generated.h"
    if not header.is_file():
        raise FileNotFoundError(f"generated header not found: {header}")

    original = header.read_text(encoding="utf-8")
    if FAST_DISPATCH_MARKER in original:
        return 0
    fixed, count = DISPATCH_FUNCTION.subn(_constant_time_dispatch, original, count=1)
    if count and fixed != original:
        header.write_text(fixed, encoding="utf-8")
        return 1
    return 0


def fix_generated_sources(generated_dir: Path) -> int:
    """Fix generated output and return the number of replacements."""
    chunks_dir = generated_dir / "chunks"
    if not chunks_dir.is_dir():
        raise FileNotFoundError(f"generated chunks not found: {chunks_dir}")
    return fix_generated_dispatch(generated_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "generated_dir",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "generated",
        help="DolRecomp generated directory (default: <repo>/generated)",
    )
    args = parser.parse_args()
    count = fix_generated_sources(args.generated_dir.resolve())
    print(f"Applied {count} generated-code correction(s).")


if __name__ == "__main__":
    main()
