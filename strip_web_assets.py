# Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
# Regenerates src/web_assets_generated.h (flash-resident const uint8_t[]
# arrays, one per file) from web_src/ (the documented source -- keep
# comments there, they're real design rationale, same as everywhere else in
# this project). Runs eagerly at script-import time (PlatformIO/SCons
# extra_scripts execute at configuration time, before any compilation),
# not as a target pre-action -- WebRemoteHandler.cpp #includes the
# generated header, so it must exist before *any* source file compiles,
# not just before a separate filesystem-image target the way the old
# SPIFFS-based version worked.
#
# 2026-08-13: switched from writing data/ (a SPIFFS image source) to
# generating this header directly. The old version's response-serving code
# in WebRemoteHandler.cpp loaded each file from SPIFFS into a
# `new (std::nothrow) char[size]` heap buffer once at boot, never freed --
# ~5.7KB of heap permanently reserved for the device's entire uptime, for
# no reason connected to the feature actually working (confirmed by reading
# ESPAsyncWebServer's own source: the beginResponse() overload used already
# resolves to AsyncProgmemResponse, which reads via memcpy_P -- plain
# memcpy on ESP32, since flash is memory-mapped on this chip unlike AVR --
# and genuinely does not care whether the pointer is heap or flash). See
# docs/HEAP_FIX_IMPLEMENTATION_PLAN.md, Branch 0. SPIFFS is no longer used
# anywhere in this codebase as of this change -- see no_spiffs.csv.
#
# src/web_assets_generated.h is a build artifact (regenerated every build
# from web_src/), same as data/ was before -- git-ignored, not a second
# copy of the source to keep in sync by hand.
#
# gzip IS used (again) -- real-hardware testing (2026-08-13) found the
# actual constraint: this device has a hard reliable-response-size ceiling
# around 4KB, confirmed by direct binary search (a diagnostic route
# returning a configurable-size plain payload: 4000 bytes always came back
# intact, 5000+ came back empty or truncated), independent of which
# AsyncWebServer response class serves it. Earlier failures blamed on
# AsyncProgmemResponse specifically, and later on gzip's binary content,
# were both really this same ceiling -- the working fix is keeping every
# individual response under it, not switching response classes or dropping
# compression. index.html's inline <script> was pulled out to its own
# app.js specifically so each of the three files (index.html, style.css,
# app.js) can be minified and gzipped to comfortably clear 4KB
# individually; combined into one response they don't (measured: ~5.7KB
# gzipped combined, confirmed too big).
#
# app.js additionally goes through terser (mangle + compress) before gzip
# -- comment/whitespace stripping alone only shrinks gzipped JS by a few
# percent (gzip already compresses repeated whitespace/tokens well), but
# terser's identifier mangling and dead-code elimination cut it roughly in
# half even after gzip (measured: 5235 -> 2407 bytes). This adds a real
# build-time dependency this project didn't have before: `terser` must be
# on PATH (e.g. `npm install -g terser`). If it's missing, this script
# falls back to shipping app.js only comment-stripped (not mangled) --
# louder to notice (print + larger file) than silently breaking the build,
# but the resulting gzip size may exceed the safe ceiling on this
# hardware; install terser to fix that.
import gzip
import os
import re
import shutil
import subprocess
from SCons.Script import Import

Import("env")

PROJECT_DIR = env["PROJECT_DIR"]
WEB_SRC_DIR = os.path.join(PROJECT_DIR, "web_src")
GENERATED_HEADER = os.path.join(PROJECT_DIR, "src", "web_assets_generated.h")


def strip_html(text):
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    lines = []
    for line in text.split("\n"):
        if line.strip().startswith("//"):
            continue
        lines.append(line)
    text = "\n".join(lines)
    text = re.sub(r"\n\s*\n+", "\n", text)
    return text


def strip_css(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"\n\s*\n+", "\n", text)
    return text


def minify_js(text):
    terser = shutil.which("terser")
    if not terser:
        print(
            "strip_web_assets: WARNING -- terser not found on PATH, "
            "shipping app.js comment-stripped only (not mangled). "
            "Install with `npm install -g terser` -- without it, "
            "app.js's gzipped size may exceed this device's ~4KB "
            "reliable response ceiling."
        )
        # Reuse the // line-comment stripping logic (same conservative
        # rule: only strips a // that starts a line).
        lines = [l for l in text.split("\n") if not l.strip().startswith("//")]
        return "\n".join(lines)

    result = subprocess.run(
        [terser, "--compress", "--mangle"],
        input=text,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print("strip_web_assets: terser failed:", result.stderr)
        print("strip_web_assets: falling back to comment-stripped app.js")
        lines = [l for l in text.split("\n") if not l.strip().startswith("//")]
        return "\n".join(lines)
    return result.stdout


def gzip_bytes(text_bytes):
    # mtime=0 keeps output byte-identical across builds for identical
    # input (gzip embeds a timestamp by default otherwise).
    return gzip.compress(text_bytes, compresslevel=9, mtime=0)


def c_identifier(filename):
    # "index.html" -> "index_html_gz", "style.css" -> "style_css_gz"
    return re.sub(r"[^0-9a-zA-Z_]", "_", filename) + "_gz"


def format_byte_array(data, var_name):
    lines = ["static const uint8_t {}[] = {{".format(var_name)]
    row = []
    for i, b in enumerate(data):
        row.append("0x{:02x}".format(b))
        if len(row) == 20:
            lines.append("    " + ",".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ",".join(row) + ",")
    lines.append("};")
    lines.append("static const size_t {}_len = {};".format(var_name, len(data)))
    return "\n".join(lines)


def generate_header(source=None, target=None, env=None):
    if not os.path.isdir(WEB_SRC_DIR):
        print("strip_web_assets: no web_src/ directory, nothing to do")
        return

    sizes = []
    blocks = []
    for name in sorted(os.listdir(WEB_SRC_DIR)):
        src_path = os.path.join(WEB_SRC_DIR, name)
        if not os.path.isfile(src_path):
            continue
        if not (name.endswith(".html") or name.endswith(".css") or name.endswith(".js")):
            continue

        with open(src_path, "r", encoding="utf-8") as f:
            original = f.read()

        if name.endswith(".html"):
            processed = strip_html(original)
        elif name.endswith(".css"):
            processed = strip_css(original)
        else:
            processed = minify_js(original)

        gz_data = gzip_bytes(processed.encode("utf-8"))
        var_name = c_identifier(name)
        blocks.append(format_byte_array(gz_data, var_name))
        sizes.append("{}: {} -> {} gzipped".format(name, len(original.encode("utf-8")), len(gz_data)))

    header_lines = [
        "// Auto-generated by strip_web_assets.py from web_src/ -- do not edit.",
        "// Regenerated on every build; not checked in (see .gitignore).",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]
    header_lines.append("\n\n".join(blocks))
    header_lines.append("")

    os.makedirs(os.path.dirname(GENERATED_HEADER), exist_ok=True)
    with open(GENERATED_HEADER, "w", encoding="utf-8") as f:
        f.write("\n".join(header_lines))

    print("strip_web_assets: " + "; ".join(sizes))


generate_header()
