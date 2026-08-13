# Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
# Regenerates data/ (what actually gets packed into the SPIFFS image) from
# web_src/ (the documented source -- keep comments there, they're real
# design rationale, same as everywhere else in this project). Hooked as a
# pre-action on the SPIFFS image target so it runs before both `buildfs`
# and `uploadfs`, never touching web_src/ itself.
#
# data/ is git-ignored -- it's a build artifact, regenerated every time,
# not a second copy of the source to keep in sync by hand.
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
DATA_DIR = env.get("PROJECT_DATA_DIR", os.path.join(PROJECT_DIR, "data"))


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


def write_gz(dst_path, text_bytes):
    # mtime=0 keeps output byte-identical across builds for identical
    # input (gzip embeds a timestamp by default otherwise).
    gz_data = gzip.compress(text_bytes, compresslevel=9, mtime=0)
    with open(dst_path, "wb") as f:
        f.write(gz_data)
    return len(gz_data)


def generate_data_dir(source, target, env):
    if not os.path.isdir(WEB_SRC_DIR):
        print("strip_web_assets: no web_src/ directory, nothing to do")
        return

    if os.path.isdir(DATA_DIR):
        shutil.rmtree(DATA_DIR)
    os.makedirs(DATA_DIR)

    sizes = []
    for name in os.listdir(WEB_SRC_DIR):
        src_path = os.path.join(WEB_SRC_DIR, name)
        if not os.path.isfile(src_path):
            continue

        if name.endswith(".html"):
            with open(src_path, "r", encoding="utf-8") as f:
                original = f.read()
            processed = strip_html(original)
        elif name.endswith(".css"):
            with open(src_path, "r", encoding="utf-8") as f:
                original = f.read()
            processed = strip_css(original)
        elif name.endswith(".js"):
            with open(src_path, "r", encoding="utf-8") as f:
                original = f.read()
            processed = minify_js(original)
        else:
            dst_path = os.path.join(DATA_DIR, name)
            shutil.copy2(src_path, dst_path)
            continue

        gz_len = write_gz(os.path.join(DATA_DIR, name + ".gz"), processed.encode("utf-8"))
        sizes.append("{}: {} -> {} gzipped".format(name, len(original.encode("utf-8")), gz_len))

    print("strip_web_assets: " + "; ".join(sizes))


# "spiffs" here matches board_build.filesystem's default in this platform
# (board.get("build.filesystem", "spiffs")) -- this project doesn't
# override it, and min_spiffs.csv's partition type agrees ("spiffs, data,
# spiffs"). If that default ever changes, this target name needs to change
# with it (e.g. to littlefs.bin).
env.AddPreAction("$BUILD_DIR/spiffs.bin", generate_data_dir)
