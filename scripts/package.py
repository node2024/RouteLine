"""Create allowlisted server and source archives. Run the build/tests before packaging."""
import argparse
import hashlib
import pathlib
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build", default="build")
parser.add_argument("--workshop-build", help="Folder of compiled RouteLine Workshop assets")
args = parser.parse_args()
build = (root / args.build).resolve()
binary = build / "routeline.so"
if not binary.is_file() or binary.read_bytes()[:4] != b"\x7fELF":
    raise SystemExit("Build the Linux routeline.so before packaging.")
release = root / "release"
release.mkdir(exist_ok=True)
version = "0.16.19"
server = release / f"routeline-{version}-linux.zip"
source = release / f"routeline-{version}-source.zip"
server_files = ["addons/metamod/routeline.vdf", "addons/routeline/config.json",
                "addons/routeline/gamedata.json", "cfg/routeline.cfg"]
archives = [server, source]
workshop_files = ["addoninfo.txt", "particles/routeline/route.vpcf_c", "particles/routeline/marker.vpcf_c",
                  "particles/routeline/marker_flat.vpcf_c", "particles/routeline/marker_flat_blue.vpcf_c",
                  "particles/routeline/marker_flat_red.vpcf_c", "materials/routeline/marker_flat.vtex_c",
                  "particles/routeline/jump_flat_amber.vpcf_c", "particles/routeline/jump_flat_green.vpcf_c",
                  "materials/routeline/jump_flat.vtex_c", "materials/routeline/marker_mesh.vmat_c",
                  "materials/routeline/marker_mesh_vmat_g_tcolor_19031fd4.vtex_c",
                  "models/routeline/landing_mark.vmdl_c", "models/routeline/jump_mark.vmdl_c"]
if args.workshop_build:
    compiled = (root / args.workshop_build).resolve()
    for name in workshop_files:
        asset = compiled / name
        if not asset.is_file() or not asset.stat().st_size:
            raise SystemExit(f"Missing Workshop output: {asset}")
with zipfile.ZipFile(server, "w", zipfile.ZIP_DEFLATED) as z:
    for name in server_files:
        z.write(root / "deploy" / name, name)
    z.write(binary, "addons/routeline/bin/linuxsteamrt64/routeline.so")
source_formats = {
    "src": {".cpp", ".h"},
    "tests": {".cpp", ".h", ".py"},
    "scripts": {".py"},
    "third_party/api": {".h"},
    "workshop": {".bat", ".png", ".ps1", ".vpcf", ".vtex", ".vmat", ".vmdl", ".obj"},
}
allow_files = ["CMakeLists.txt", "bootstrap.sh", "third_party/nlohmann/json.hpp", "workshop/addoninfo.txt"]
allow_files += [f"deploy/{name}" for name in server_files]
with zipfile.ZipFile(source, "w", zipfile.ZIP_DEFLATED) as z:
    for name in allow_files:
        z.write(root / name, name)
    for name, extensions in source_formats.items():
        for file in sorted((root / name).rglob("*")):
            if file.is_file() and file.suffix in extensions:
                z.write(file, file.relative_to(root).as_posix())
if args.workshop_build:
    workshop = release / f"routeline-{version}-workshop.zip"
    with zipfile.ZipFile(workshop, "w", zipfile.ZIP_DEFLATED) as z:
        for name in ["Install-RouteLine-Addon.bat", "Install-Addon.ps1"]:
            z.write(root / "workshop" / name, name)
        for name in workshop_files:
            z.write(compiled / name, f"game/csgo_addons/routeline/{name}")
            source_name = name[:-2] if name.endswith("_c") else name
            if (root / "workshop" / source_name).is_file():
                z.write(root / "workshop" / source_name, f"content/csgo_addons/routeline/{source_name}")
        for asset_source in ["materials/routeline/marker_flat.png", "materials/routeline/jump_flat.png",
                             "models/routeline/landing_mark.obj", "models/routeline/jump_mark.obj"]:
            z.write(root / "workshop" / asset_source, f"content/csgo_addons/routeline/{asset_source}")
    archives.append(workshop)
with (release / "SHA256SUMS.txt").open("w", encoding="utf-8", newline="\n") as f:
    for path in archives:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        f.write(f"{digest}  {path.name}\n")
        print(f"{path.name}: {path.stat().st_size:,} bytes / {digest}")
