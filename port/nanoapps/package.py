#!/usr/bin/env python3
"""Package nanoCDJ for NanoApps: build/nanoCDJ.nanoapp/ (+ build/nanoCDJ.zip).

Same on-device layout as NanoApps' `./start package` (manifest.napp, app.hbapp,
icon.bin), built from NanoApps' own Python helpers but without their repo's
apps/ tree or Font Awesome: the icon is drawn here.

    ./build.sh && python3 package.py
Then copy build/nanoCDJ.nanoapp into the Apps folder on the iPod and eject.
"""
import hashlib, os, plistlib, struct, subprocess, sys, tempfile, zipfile, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
NANO = Path(os.environ.get("NANOAPPS", HERE / "../../third_party/NanoApps")).resolve()
if not (NANO / "tools" / "package_app.py").is_file():
    sys.exit(f"NanoApps not found at {NANO}: run `git submodule update --init` (or set NANOAPPS)")
sys.path.insert(0, str(NANO / "tools"))
sys.path.insert(0, str(NANO / "tools" / "dependencies" / "ipodhax"))
from ipodhax.silverdb import pack_silverdb          # noqa: E402
from package_app import manifest_for, validate_folder  # noqa: E402
from PIL import Image, ImageDraw                     # noqa: E402

# constants from NanoApps tools/build_apps.py
HBSCREEN, HBLAYOUT = 229474306, 229474305
HBAP, HBSD = 0x48426170, 0x48425344
# NanoApps' compiled screen-layout database (its tools/screens, built with its Rust
# silverutil). Vendored here, extracted from the NanoApps release, so packaging
# needs no Rust toolchain. MIT, (c) the NanoApps authors.
SCREEN = HERE / "hb_silverdb.bin"
SIZE, FMT, ICON_BASE_ID = 112, 0x1888, 229441873
SOURCE_NAME = "nanocdj"                              # stable identity: hb.nanocdj


def draw_icon(path, accent):
    """A jog wheel on a dark disc, in the springboard's 88x89 disc slot."""
    ss = 4
    img = Image.new("RGBA", (SIZE * ss, SIZE * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = 12 * ss, 6 * ss, 100 * ss, 95 * ss
    d.ellipse((x0 + 2 * ss, y0 + 4 * ss, x1 - 2 * ss, y1 + 3 * ss), fill=(0, 0, 0, 90))   # shadow
    d.ellipse((x0, y0, x1, y1), fill=(28, 28, 32, 255))
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    r = (x1 - x0) / 2
    for k, col in ((0.80, accent), (0.72, (28, 28, 32, 255))):                           # orange ring
        d.ellipse((cx - r * k, cy - r * k, cx + r * k, cy + r * k), fill=col)
    d.ellipse((cx - r * 0.6, cy - r * 0.6, cx + r * 0.6, cy + r * 0.6), fill=(52, 52, 58, 255))  # platter
    d.ellipse((cx - r * 0.13, cy - r * 0.13, cx + r * 0.13, cy + r * 0.13), fill=accent)       # spindle
    mx, my = cx + r * 0.38, cy - r * 0.32                                                    # marker
    d.ellipse((mx - r * 0.08, my - r * 0.08, mx + r * 0.08, my + r * 0.08), fill=(235, 235, 235, 255))
    gloss = Image.new("RGBA", img.size, (0, 0, 0, 0))                                        # gloss, blended
    ImageDraw.Draw(gloss).chord((x0, y0, x1, y1), 200, 340, fill=(255, 255, 255, 28))
    img.alpha_composite(gloss)
    img.resize((SIZE, SIZE), Image.LANCZOS).save(path)


def main():
    hbapp = HERE / "build" / f"{SOURCE_NAME}.hbapp"
    if not hbapp.is_file():
        sys.exit("build/nanocdj.hbapp missing: run ./build.sh first")
    if not SCREEN.is_file():
        sys.exit(f"{SCREEN} missing (it ships with the repo)")
    meta = plistlib.loads((HERE / "Info.plist").read_bytes())
    label = meta["CFBundleName"][:23]
    bundle_id = meta["CFBundleIdentifier"]
    color = meta.get("HBIconColor", "#ff8c00").lstrip("#")
    accent = tuple(int(color[i:i + 2], 16) for i in (0, 2, 4)) + (255,)
    sbid = "hb." + SOURCE_NAME
    app_id = 0x10000000 | (int.from_bytes(hashlib.sha256(bundle_id.encode()).digest()[:4], "little") & 0x0FFFFFFF)

    with tempfile.TemporaryDirectory(prefix="nanocdj_pkg_") as td:
        td = Path(td)
        icons = td / "icons"
        icons.mkdir()
        draw_icon(icons / f"{ICON_BASE_ID}_{FMT:04x}.png", accent)
        shutil.copy(icons / f"{ICON_BASE_ID}_{FMT:04x}.png", HERE / "build" / "icon.png")
        with open(td / "icons.bin", "wb") as f:
            pack_silverdb(f, icons)
        data = (td / "icons.bin").read_bytes()
        ds = struct.unpack_from("<I", data, 4)[0]
        off, ln = struct.unpack_from("<II", data, 28 + 4)
        icon = data[ds + off:ds + off + ln]

        appf = td / f"{SOURCE_NAME}.app"
        subprocess.run([sys.executable, str(NANO / "tools" / "mkapp.py"), "bundle", str(appf),
                        "--id", hex(app_id), "--kind", str(int(meta.get("HBAppKind", 1))),
                        "--sbid", sbid, "--name", sbid,
                        "--label-id", hex(app_id), "--label-tag", hex(HBAP), "--label-text", label,
                        "--screen-id", str(HBSCREEN), "--layout-id", str(HBLAYOUT),
                        "--screen-tag", hex(HBSD), "--screen", str(SCREEN)],
                       check=True, stdout=subprocess.DEVNULL)
        exe = hbapp.read_bytes()
        manifest = manifest_for(appf.read_bytes(), exe, icon, int(meta.get("HBMinimumResidentVersion", 0)))

    folder_name = validate_folder(label + ".nanoapp")
    out = HERE / "build" / folder_name
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    files = {"manifest.napp": manifest, "app.hbapp": exe, "icon.bin": icon}
    for name, blob in files.items():
        (out / name).write_bytes(blob)
    zpath = HERE / "build" / f"{label}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for name, blob in files.items():
            z.writestr(f"{folder_name}/{name}", blob)
    print(f"packaged {out.relative_to(HERE)} ({len(exe)} byte app, {len(icon)} byte icon) and {zpath.name}")
    print("Copy the .nanoapp folder into Apps on the iPod, then eject.")


if __name__ == "__main__":
    main()
