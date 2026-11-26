"""Packs a Linux build into a release tarball:
GodzillaSTE-Recomp-v<version>-Linux-x64.tar.gz

The Linux counterpart of package.ps1, and written in Python so it also runs on
Windows (for a cross-compiled build) and still gives the program and play.sh
their executable bit. The tarball holds the program, play.sh, recomplauncher.json
(so RecompLauncher can set the game up and start it), the button-icon drawings
in textures_ps/, README.md, LICENSE and the bundled libraries' licenses. SDL2,
OpenSSL 3 and libepoxy come from the system (SteamOS has them). Nothing from
the game disc goes in.

Note: godzilla_ste_recomp is the game's default.xbe translated to C and
compiled. Whether to publish it is the project owner's decision; this script
only builds the tarball locally.

    python package_linux.py --version 0.3.0-beta.1 --program build-linux/godzilla_ste_recomp
"""
import argparse
import io
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.abspath(__file__))


def add_file(tar, src, name, mode):
    info = tar.gettarinfo(src, name)
    info.mode = mode
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    with open(src, "rb") as f:
        tar.addfile(info, f)


def add_text(tar, name, data, mode, mtime):
    data = data.replace(b"\r\n", b"\n")    # a Windows checkout may have CRLF
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = int(mtime)
    tar.addfile(info, io.BytesIO(data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--program", default=os.path.join(ROOT, "build-linux", "godzilla_ste_recomp"))
    args = ap.parse_args()
    version = args.version.lstrip("v")
    if not os.path.isfile(args.program):
        sys.exit("Build first: %s not found." % args.program)
    with open(args.program, "rb") as f:
        if f.read(4) != b"\x7fELF":
            sys.exit("%s is not a Linux program." % args.program)

    out = os.path.join(ROOT, "dist")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "GodzillaSTE-Recomp-v%s-Linux-x64.tar.gz" % version)
    with tarfile.open(path, "w:gz", format=tarfile.GNU_FORMAT) as tar:
        add_file(tar, args.program, "godzilla_ste_recomp", 0o755)
        with open(os.path.join(ROOT, "play.sh"), "rb") as f:
            add_text(tar, "play.sh", f.read(), 0o755, os.path.getmtime(os.path.join(ROOT, "play.sh")))
        for name in ("recomplauncher.json", "README.md", "LICENSE"):
            add_file(tar, os.path.join(ROOT, name), name, 0o644)
        tex = os.path.join(ROOT, "textures_ps")
        for dirpath, _, files in os.walk(tex):
            for fn in sorted(files):
                src = os.path.join(dirpath, fn)
                rel = os.path.relpath(src, ROOT).replace(os.sep, "/")
                add_file(tar, src, rel, 0o644)
        ach = os.path.join(ROOT, "third_party", "RecompAchievements")
        if os.path.isfile(os.path.join(ach, "LICENSE")):
            add_file(tar, os.path.join(ach, "LICENSE"), "licenses/RecompAchievements.txt", 0o644)
            for lib in ("rcheevos", "cjson"):
                d = os.path.join(ach, "sdk", "third_party", lib)
                lic = sorted(f for f in os.listdir(d) if f.startswith("LICENSE")) if os.path.isdir(d) else []
                if lic:
                    add_file(tar, os.path.join(d, lic[0]), "licenses/%s.txt" % lib, 0o644)
    print("Wrote " + path)


if __name__ == "__main__":
    main()
