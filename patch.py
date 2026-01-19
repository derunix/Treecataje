import os
import glob
import gzip
import hashlib
from typing import TYPE_CHECKING, Any, List
from os import makedirs, remove
from os.path import basename, dirname, exists, isfile, join

try:
    import requests  # optional when ALLOW_NET_MINIFY=1
except Exception:  # pragma: no cover
    requests = None  # type: ignore

if TYPE_CHECKING:
    Import: Any = None
    env: Any = {}

from SCons.Script import Import  # type: ignore
Import("env")  # provided by PlatformIO

FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
board_mcu = env.BoardConfig()
mcu = board_mcu.get("build.mcu", "")
patchflag_path = join(FRAMEWORK_DIR, "tools", "sdk", mcu, "lib", ".patched")

# ---- net80211 patch: guard and simplify ----
def _patch_wifi_lib():
    if isfile(patchflag_path):
        return
    libdir = join(FRAMEWORK_DIR, "tools", "sdk", mcu, "lib")
    original_file = join(libdir, "libnet80211.a")
    patched_file = join(libdir, "libnet80211.a.patched")
    if not isfile(original_file):
        print(f"[net80211] skip: missing {original_file}")
        return
    # Copy original -> patched
    try:
        with open(original_file, 'rb') as src, open(patched_file, 'wb') as dst:
            dst.write(src.read())
    except Exception as e:
        print(f"[net80211] copy failed: {e}")
        return
    # Weaken a known sanity-check symbol; skip invalid 's' symbol from previous script
    tool_pkg = f"toolchain-xtensa-{mcu}"
    cmd = (
        f"pio pkg exec -p {tool_pkg} -- xtensa-{mcu}-elf-objcopy "
        f" --weaken-symbol=ieee80211_raw_frame_sanity_check {patched_file} {original_file}"
    )
    try:
        env.Execute(cmd)
        # touch flag
        with open(patchflag_path, "w") as fp:
            fp.write("")
        print("[net80211] patched successfully")
    except Exception as e:
        print(f"[net80211] patch failed: {e}")

# ---- web assets gzip/embed with optional minify ----
MINIFY_WEB = os.environ.get('BRUCE_MINIFY_WEB', '1') not in ('0', 'false', 'False')
ALLOW_NET_MINIFY = os.environ.get('BRUCE_MINIFY_USE_NET', '0') in ('1', 'true', 'True') and requests is not None

def hash_file(file_path: str) -> str:
    h = hashlib.sha256()
    with open(file_path, 'rb') as f:
        for chunk in iter(lambda: f.read(4096), b''):
            h.update(chunk)
    return h.hexdigest()

def hash_files(file_paths: List[str]) -> str:
    h = hashlib.sha256()
    for fp in file_paths:
        h.update(hash_file(fp).encode('utf-8'))
    return h.hexdigest()

def save_checksum_file(hash_value: str, output_file: str) -> None:
    with open(output_file, 'w') as f:
        f.write(hash_value)

def load_checksum_file(input_file: str) -> str:
    with open(input_file, 'r') as f:
        return f.readline().strip()

def _offline_minify(data: bytes, kind: str) -> bytes:
    try:
        txt = data.decode('utf-8', errors='ignore')
        import re
        if kind == 'css':
            txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
            txt = re.sub(r'\s+', ' ', txt)
        elif kind == 'js':
            txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
            txt = re.sub(r'//.*', '', txt)
            txt = re.sub(r'\s+', ' ', txt)
        elif kind == 'html':
            txt = re.sub(r'<!--.*?-->', '', txt, flags=re.S)
            txt = re.sub(r'>\s+<', '><', txt)
        return txt.encode('utf-8')
    except Exception:
        return data

def minify_css(c):
    data = c.read()
    if not MINIFY_WEB:
        return data
    if ALLOW_NET_MINIFY:
        try:
            r = requests.post("https://www.toptal.com/developers/cssminifier/api/raw", {"input": data.decode('utf-8','ignore')}, timeout=5)
            if r.ok:
                return r.text.encode('utf-8')
        except Exception:
            pass
    return _offline_minify(data, 'css')

def minify_js(js):
    data = js.read()
    if not MINIFY_WEB:
        return data
    if ALLOW_NET_MINIFY:
        try:
            r = requests.post("https://www.toptal.com/developers/javascript-minifier/api/raw", {"input": data.decode('utf-8','ignore')}, timeout=5)
            if r.ok:
                return r.text.encode('utf-8')
        except Exception:
            pass
    return _offline_minify(data, 'js')

def minify_html(html):
    data = html.read()
    if not MINIFY_WEB:
        return data
    if ALLOW_NET_MINIFY:
        try:
            r = requests.post("https://www.toptal.com/developers/html-minifier/api/raw", {"input": data.decode('utf-8','ignore')}, timeout=5)
            if r.ok:
                return r.text.encode('utf-8')
        except Exception:
            pass
    return _offline_minify(data, 'html')

# gzip web files
def prepare_www_files():
    HEADER_FILE = join(env.get("PROJECT_DIR"), "include", "webFiles.h")
    filetypes_to_gzip = ["html", "css", "js"]
    data_src_dir = join(env.get("PROJECT_DIR"), "embedded_resources/web_interface")
    checksum_file = join(data_src_dir, "checksum.sha256")
    checksum = ""

    if not exists(data_src_dir):
        print(f'Error: Source directory "{data_src_dir}" does not exist!')
        return

    if exists(checksum_file):
        checksum = load_checksum_file(checksum_file)

    files_to_gzip: List[str] = []
    for extension in filetypes_to_gzip:
        files_to_gzip.extend(glob.glob(join(data_src_dir, "*." + extension)))

    files_checksum = hash_files(files_to_gzip)
    if files_checksum == checksum:
        print("[GZIP & EMBED INTO HEADER] - Nothing to process.")
        return

    print(f"[GZIP & EMBED INTO HEADER] - Processing {len(files_to_gzip)} files.")

    makedirs(dirname(HEADER_FILE), exist_ok=True)

    with open(HEADER_FILE, "w") as header:
        header.write("#ifndef WEB_FILES_H\n#define WEB_FILES_H\n\n#include <Arduino.h>\n\n")
        header.write("// THIS FILE IS AUTOGENERATED DO NOT MODIFY IT. MODIFY FILES IN /embedded_resources/web_interface\n\n")

        for file in files_to_gzip:
            gz_file = file + ".gz"
            with open(file, "rb") as src, gzip.open(gz_file, "wb") as dst:
                ext = basename(file).rsplit(".", 1)[-1].lower()
                if ext == 'html':
                    minified = minify_html(src)
                elif ext == 'css':
                    minified = minify_css(src)
                elif ext == 'js':
                    minified = minify_js(src)
                else:
                    raise ValueError(f"Unsupported file type: {ext}")
                dst.write(minified)

            with open(gz_file, "rb") as gz:
                compressed_data = gz.read()
                var_name = basename(file).replace(".", "_")

                header.write(f"const uint8_t {var_name}[] PROGMEM = {{\n")
                for i in range(0, len(compressed_data), 15):
                    hex_chunk = ", ".join(f"0x{byte:02X}" for byte in compressed_data[i:i+15])
                    header.write(f"  {hex_chunk},\n")
                header.write("};\n\n")
                header.write(f"const uint32_t {var_name}_size = {len(compressed_data)};\n\n")

            remove(gz_file)

        header.write("#endif // WEB_FILES_H\n")

    save_checksum_file(files_checksum, checksum_file)
    print(f"[DONE] Gzipped files embedded into {HEADER_FILE}")

# Execute steps
_patch_wifi_lib()
prepare_www_files()