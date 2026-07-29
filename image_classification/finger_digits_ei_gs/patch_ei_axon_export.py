#!/usr/bin/env python3
"""
patch_ei_axon_export.py

Automates the fixes needed to run an Edge Impulse "Nordic Axon NPU library"
image-classification export via run_classifier() on an nRF54LM20B (Axon NPU).

Fixes automated (see project debug notes for full rationale):

  nordic_axon.h (the two real EI SDK bugs, generated code -> lost on re-export):
    7a. Input buffer sized as height*width only, missing channel_cnt -> overflow
        for multi-channel (RGB) models.
    7b. EI's image DSP outputs HWC (interleaved) pixels, but the compiled Axon
        model expects CHW (planar). The wrapper does no transpose, so the model
        runs cleanly but returns a constant, confidently-wrong class. This patch
        adds a transpose-while-quantizing loop.

  prj.conf:
    - CONFIG_EDGE_IMPULSE_SDK=n            (avoid version-mismatched SDK module)
    - CONFIG_NRF_AXON_MODEL_NAME="<name>"
    - CONFIG_MAIN_STACK_SIZE=40960         (run_classifier's int8 vector on stack)
    - CONFIG_NEWLIB_LIBC=y / CONFIG_FPU=y  (float-heavy DSP, ~110KB heap matrix)
    - CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE=<MAX_IL_BUFFER_USED>
      (auto-read from the model's own generated header -- no manual copying)

  CMakeLists.txt:
    - DTS_ROOT / EXTRA_CONF_FILE moved BEFORE find_package(Zephyr REQUIRED ...)
      (Zephyr reads these during find_package; setting them after is a silent
      no-op)
    - add_subdirectory(ei-model/edge-impulse-sdk/cmake/zephyr) + include dirs +
      NRF_AXON_MODEL_NAME compile definition, inserted if missing.

USAGE
-----
    python patch_ei_axon_export.py --project-root . --model-name finger_digits_v3_1

    Options:
      --ei-model-dir NAME   subfolder holding the EI export (default: ei-model)
      --dry-run             show unified diffs, write nothing
      --no-backup           skip writing .bak files (backups are on by default)

Every patch is idempotent: markers are inserted so re-running this script
after a fresh EI re-export (or a repeated run) does not double-patch.
"""

import argparse
import difflib
import re
import sys
from pathlib import Path

MARK_7A = "// [ei-axon-patch] 7a: channel_cnt included in vector_size"
MARK_7B = "// [ei-axon-patch] 7b: HWC -> CHW transpose applied"
MARK_PRJCONF = "# [ei-axon-patch] settings below managed by patch_ei_axon_export.py"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write(path: Path, content: str, dry_run: bool, make_backup: bool):
    if dry_run:
        return
    if make_backup:
        backup = path.with_suffix(path.suffix + ".bak")
        if not backup.exists():
            backup.write_text(read(path), encoding="utf-8")
    path.write_text(content, encoding="utf-8")


def show_diff(label: str, before: str, after: str):
    if before == after:
        print(f"  [skip] {label}: no changes needed (already patched or pattern not found)")
        return
    diff = difflib.unified_diff(
        before.splitlines(keepends=True),
        after.splitlines(keepends=True),
        fromfile=f"{label} (before)",
        tofile=f"{label} (after)",
    )
    print(f"  [patch] {label}:")
    sys.stdout.writelines("    " + line for line in diff)


# ---------------------------------------------------------------------------
# 1. nordic_axon.h  --  bugs 7a and 7b
# ---------------------------------------------------------------------------

# Matches the vector_size declaration regardless of whether it's the broken
# 2-operand form or the already-patched 3-operand form. Dimension expressions
# in real exports are long indexed struct paths, e.g.:
#   nrf_axon_compiled_model->inputs[nrf_axon_compiled_model->external_input_ndx].dimensions.height
# so we capture generically up to the known ".dimensions.height/.width/.channel_cnt"
# suffix rather than assuming a short prefix.
DIM_LINE_RE = re.compile(
    r"uint32_t\s+vector_size\s*=\s*"
    r"(?P<height>[^\n;*]+?\.dimensions\.height)\s*\*\s*"
    r"(?P<width>[^\n;*]+?\.dimensions\.width)"
    r"(?:\s*\*\s*(?P<channel>[^\n;*]+?\.dimensions\.channel_cnt))?"
    r"\s*;"
)

# Matches the plain linear quantization loop as it actually appears in EI
# exports: a single-line assignment, possibly with a `//TODO` comment line
# (and/or the `@FIXME CHECK IF MODEL IS TRANSPOSED` block comment) between the
# `for (...)  {` and the assignment.
LOOP_RE = re.compile(
    r"for\s*\(\s*size_t\s+i\s*=\s*0\s*;\s*i\s*<\s*matrix->rows\s*\*\s*matrix->cols\s*;\s*i\+\+\s*\)\s*\{"
    r"(?P<body>(?:[^{}])*?)"
    r"input_vector\[i\]\s*=\s*\(int8_t\)\(\(matrix->buffer\[i\]\s*/\s*graph_config->input_scale\)"
    r"\s*\+\s*graph_config->input_zeropoint\)\s*;"
    r"(?P<tail>[^{}]*?)\}",
    re.DOTALL,
)


def patch_nordic_axon_h(content: str) -> tuple[str, list[str]]:
    notes = []

    # --- 7a: vector_size must include channel_cnt ---------------------------
    dim_match = DIM_LINE_RE.search(content)
    height_expr = width_expr = channel_expr = None

    if dim_match is None:
        notes.append(
            "7a/7b: WARNING -- 'uint32_t vector_size = ...dimensions.height * "
            "...dimensions.width;' pattern not found at all. File structure may "
            "differ significantly from expected -- inspect manually."
        )
    else:
        height_expr = dim_match.group("height")
        width_expr = dim_match.group("width")
        channel_expr = dim_match.group("channel")

        if channel_expr:
            notes.append("7a: vector_size already includes channel_cnt -- skipping")
        else:
            channel_expr = re.sub(r"\.dimensions\.height$", ".dimensions.channel_cnt", height_expr)
            new_line = (
                f"uint32_t vector_size = {height_expr} * {width_expr} * {channel_expr};"
                f"  {MARK_7A}"
            )
            content = content[: dim_match.start()] + new_line + content[dim_match.end():]
            notes.append("7a: patched vector_size to include channel_cnt")

    # --- 7b: transpose HWC -> CHW while quantizing ---------------------------
    if MARK_7B in content:
        notes.append("7b: marker found -- already patched, skipping")
        return content, notes

    loop_match = LOOP_RE.search(content)
    if loop_match is None:
        notes.append(
            "7b: marker not found and loop pattern not found either -- if this "
            "file was already patched by hand with different formatting, add "
            f"the marker comment '{MARK_7B}' near the fix to mark it done. "
            "Otherwise inspect manually -- this is the critical fix."
        )
        return content, notes

    if height_expr is None:
        notes.append(
            "7b: WARNING -- found the quantization loop but could not determine "
            "the model's height/width/channel_cnt expressions (7a pattern didn't "
            "match), so the transpose fix cannot be safely generated. Skipping 7b."
        )
        return content, notes

    replacement_7b = f"""{{
        {MARK_7B}
        const uint32_t __ei_axon_in_h = {height_expr};
        const uint32_t __ei_axon_in_w = {width_expr};
        const uint32_t __ei_axon_in_c = {channel_expr};
        const size_t __ei_axon_feature_count = (size_t)matrix->rows * matrix->cols;

        if (__ei_axon_feature_count == (size_t)__ei_axon_in_h * __ei_axon_in_w * __ei_axon_in_c) {{
            const uint32_t __ei_axon_plane = __ei_axon_in_h * __ei_axon_in_w;
            for (uint32_t p = 0; p < __ei_axon_plane; p++) {{
                for (uint32_t c = 0; c < __ei_axon_in_c; c++) {{
                    float v = matrix->buffer[p * __ei_axon_in_c + c];        // HWC source
                    input_vector[c * __ei_axon_plane + p] =                  // CHW destination
                        (int8_t)((v / graph_config->input_scale) + graph_config->input_zeropoint);
                }}
            }}
        }} else {{
            // single-channel / non-image models: plain linear copy
            for (size_t i = 0; i < __ei_axon_feature_count; i++) {{
                input_vector[i] = (int8_t)((matrix->buffer[i] / graph_config->input_scale) + graph_config->input_zeropoint);
            }}
        }}
    }}"""

    content = content[: loop_match.start()] + replacement_7b + content[loop_match.end():]
    notes.append("7b: patched quantization loop with HWC->CHW transpose")

    return content, notes


# ---------------------------------------------------------------------------
# 2. prj.conf  --  required Kconfig settings
# ---------------------------------------------------------------------------

def find_max_il_buffer(ei_model_dir: Path, model_name: str) -> int | None:
    """Read <NAME>_MAX_IL_BUFFER_USED from the model's generated Axon header."""
    candidate = ei_model_dir / "nordic-axon-model" / f"nrf_axon_model_{model_name}_.h"
    if not candidate.exists():
        matches = list((ei_model_dir / "nordic-axon-model").glob("nrf_axon_model_*.h")) \
            if (ei_model_dir / "nordic-axon-model").exists() else []
        if len(matches) == 1:
            candidate = matches[0]
        else:
            return None
    text = read(candidate)
    m = re.search(r"MAX_IL_BUFFER_USED\s+(\d+)", text)
    return int(m.group(1)) if m else None


def patch_prj_conf(content: str, model_name: str, il_buffer_size: int | None) -> tuple[str, list[str]]:
    notes = []
    required = {
        "CONFIG_EDGE_IMPULSE_SDK": "n",
        "CONFIG_NRF_AXON_MODEL_NAME": f'"{model_name}"',
        "CONFIG_MAIN_STACK_SIZE": "40960",
        "CONFIG_NEWLIB_LIBC": "y",
        "CONFIG_FPU": "y",
        "CONFIG_CPP": "y",
        "CONFIG_STD_CPP11": "y",
        "CONFIG_REQUIRES_FULL_LIBCPP": "y",
    }
    if il_buffer_size is not None:
        required["CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE"] = str(il_buffer_size)
    else:
        notes.append(
            "WARNING: could not auto-detect MAX_IL_BUFFER_USED -- "
            "CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE NOT set. Find "
            "<NAME>_MAX_IL_BUFFER_USED in nordic-axon-model/nrf_axon_model_<name>_.h "
            "and set it manually, or check --ei-model-dir / --model-name are correct."
        )

    lines = content.splitlines()
    seen = set()
    out_lines = []
    key_pattern = re.compile(r"^\s*(CONFIG_[A-Z0-9_]+)\s*=")

    for line in lines:
        m = key_pattern.match(line)
        if m and m.group(1) in required:
            key = m.group(1)
            seen.add(key)
            new_line = f"{key}={required[key]}"
            out_lines.append(new_line)
        else:
            out_lines.append(line)

    missing = [k for k in required if k not in seen]
    if missing:
        out_lines.append("")
        out_lines.append(MARK_PRJCONF)
        for k in missing:
            out_lines.append(f"{k}={required[k]}")

    new_content = "\n".join(out_lines) + ("\n" if content.endswith("\n") else "")
    changed_keys = [k for k in required if k in seen] 
    if changed_keys or missing:
        notes.append(f"set/verified keys: {', '.join(sorted(required.keys()))}")
    return new_content, notes


# ---------------------------------------------------------------------------
# 3. CMakeLists.txt  --  ordering + required includes
# ---------------------------------------------------------------------------

def patch_cmakelists(content: str, ei_model_dir_name: str) -> tuple[str, list[str]]:
    notes = []
    lines = content.splitlines()

    find_pkg_idx = next(
        (i for i, l in enumerate(lines) if re.search(r"find_package\s*\(\s*Zephyr", l)),
        None,
    )
    if find_pkg_idx is None:
        notes.append("WARNING: no find_package(Zephyr ...) line found -- skipping reorder check")
    else:
        dts_idx = next((i for i, l in enumerate(lines) if "DTS_ROOT" in l), None)
        conf_idx = next((i for i, l in enumerate(lines) if "EXTRA_CONF_FILE" in l), None)

        needs_reorder = (dts_idx is not None and dts_idx > find_pkg_idx) or \
                         (conf_idx is not None and conf_idx > find_pkg_idx)

        if dts_idx is None and conf_idx is None:
            insert_lines = [
                "list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})",
                f"set(EXTRA_CONF_FILE ${{CMAKE_CURRENT_SOURCE_DIR}}/{ei_model_dir_name}/conf_overlay.conf)",
            ]
            lines = lines[:find_pkg_idx] + insert_lines + lines[find_pkg_idx:]
            notes.append("inserted DTS_ROOT / EXTRA_CONF_FILE before find_package(Zephyr) (were missing)")
        elif needs_reorder:
            moved = []
            remaining = []
            for i, l in enumerate(lines):
                if i in (dts_idx, conf_idx):
                    moved.append(l)
                else:
                    remaining.append(l)
            new_find_pkg_idx = next(
                i for i, l in enumerate(remaining) if re.search(r"find_package\s*\(\s*Zephyr", l)
            )
            lines = remaining[:new_find_pkg_idx] + moved + remaining[new_find_pkg_idx:]
            notes.append("moved DTS_ROOT / EXTRA_CONF_FILE to before find_package(Zephyr) (were after -- silently ignored)")
        else:
            notes.append("DTS_ROOT / EXTRA_CONF_FILE already correctly ordered before find_package(Zephyr)")

    content = "\n".join(lines) + ("\n" if content.endswith("\n") else "")

    required_snippets = [
        f"add_subdirectory({ei_model_dir_name}/edge-impulse-sdk/cmake/zephyr)",
        f"zephyr_include_directories({ei_model_dir_name}/nordic-axon-model)",
        f"target_include_directories(app PRIVATE {ei_model_dir_name})",
        "zephyr_compile_definitions(NRF_AXON_MODEL_NAME=${CONFIG_NRF_AXON_MODEL_NAME})",
    ]
    additions = [s for s in required_snippets if s not in content]
    if additions:
        content = content.rstrip("\n") + "\n\n# [ei-axon-patch] required for EI export's bundled SDK\n"
        content += "\n".join(additions) + "\n"
        notes.append(f"appended {len(additions)} missing required line(s) (add_subdirectory/include/compile definitions)")
    else:
        notes.append("all required add_subdirectory/include/compile-definition lines already present")

    if "src/main.cpp" not in content and "main.cpp" not in content:
        notes.append(
            "NOTE: did not find src/main.cpp in target_sources -- if this app still "
            "builds main.c, you must rename/rewrite it as main.cpp (EI SDK is C++). "
            "Not auto-fixed since this touches your application source, not build config."
        )

    return content, notes


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project-root", default=".", help="App root containing CMakeLists.txt / prj.conf / ei-model/")
    ap.add_argument("--model-name", required=True, help='e.g. finger_digits_v3_1 (matches CONFIG_NRF_AXON_MODEL_NAME)')
    ap.add_argument("--ei-model-dir", default="ei-model", help="Name of the unzipped EI export folder (default: ei-model)")
    ap.add_argument("--dry-run", action="store_true", help="Show diffs only, write nothing")
    ap.add_argument("--no-backup", action="store_true", help="Skip writing .bak backup files")
    args = ap.parse_args()

    root = Path(args.project_root).resolve()
    ei_model_dir = root / args.ei_model_dir
    make_backup = not args.no_backup

    if not root.exists():
        sys.exit(f"error: project root does not exist: {root}")
    if not ei_model_dir.exists():
        sys.exit(f"error: EI export dir not found: {ei_model_dir} (check --ei-model-dir)")

    print(f"Project root : {root}")
    print(f"EI model dir : {ei_model_dir}")
    print(f"Model name   : {args.model_name}")
    print(f"Mode         : {'DRY RUN (no files written)' if args.dry_run else 'WRITE'}")
    print()

    # --- nordic_axon.h ---
    axon_h = ei_model_dir / "edge-impulse-sdk" / "classifier" / "inferencing_engines" / "nordic_axon.h"
    print(f"1. nordic_axon.h  ({axon_h})")
    if not axon_h.exists():
        print("  [skip] file not found -- check --ei-model-dir path")
    else:
        before = read(axon_h)
        after, notes = patch_nordic_axon_h(before)
        for n in notes:
            print(f"  - {n}")
        show_diff("nordic_axon.h", before, after)
        write(axon_h, after, args.dry_run, make_backup)
    print()

    # --- prj.conf ---
    prj_conf = root / "prj.conf"
    print(f"2. prj.conf  ({prj_conf})")
    if not prj_conf.exists():
        print("  [skip] file not found at project root")
    else:
        il_size = find_max_il_buffer(ei_model_dir, args.model_name)
        if il_size is not None:
            print(f"  - auto-detected MAX_IL_BUFFER_USED = {il_size}")
        before = read(prj_conf)
        after, notes = patch_prj_conf(before, args.model_name, il_size)
        for n in notes:
            print(f"  - {n}")
        show_diff("prj.conf", before, after)
        write(prj_conf, after, args.dry_run, make_backup)
    print()

    # --- CMakeLists.txt ---
    cmake_file = root / "CMakeLists.txt"
    print(f"3. CMakeLists.txt  ({cmake_file})")
    if not cmake_file.exists():
        print("  [skip] file not found at project root")
    else:
        before = read(cmake_file)
        after, notes = patch_cmakelists(before, args.ei_model_dir)
        for n in notes:
            print(f"  - {n}")
        show_diff("CMakeLists.txt", before, after)
        write(cmake_file, after, args.dry_run, make_backup)
    print()

    print("Done." + (" (dry run -- nothing written)" if args.dry_run else ""))
    print("Next: west build -b nrf54lm20dk/nrf54lm20b/cpuapp . && confirm SELFTEST PASS before trusting live inference.")


if __name__ == "__main__":
    main()
