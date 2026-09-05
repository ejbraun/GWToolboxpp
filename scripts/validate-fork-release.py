import argparse
import hashlib
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--bin", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
versions = (root / "cmake/fork_versions.cmake").read_text()
upstream = re.search(r'set\(GWTOOLBOXDLL_VERSION "([^"]+)"\)', (root / "CMakeLists.txt").read_text())[1]
for name, constant, notes in (
    ("DBBox", "DBBOX_PLUGIN_VERSION", "plugins/DBBox/DBBox.patch.txt"),
    ("GWToolboxdll", "GWTOOLBOX_FORK_VERSION", "GWToolboxdll/GWToolboxdll.patch.txt"),
):
    version = int(re.search(rf"set\({constant} (\d+)\)", versions)[1])
    assert 0 < version <= 2147483647, (name, version)
    headings = re.findall(r"^## v(\d+) - \d{4}-\d{2}-\d{2}$", (root / notes).read_text(), re.M)
    assert headings and list(map(int, headings)) == sorted(set(map(int, headings))), notes
    assert int(headings[-1]) == version, (name, "Append patch notes for the new revision")
    if args.bin:
        manifest = json.loads((args.bin / f"{name}.version.json").read_text())
        assert manifest["name"] == name and manifest["version"] == version, manifest
        assert manifest["sha256"] == hashlib.sha256((args.bin / f"{name}.dll").read_bytes()).hexdigest(), name
        assert manifest["toolbox_abi"] > 0 and manifest["build_id"], name
        assert (args.bin / f"{name}.pdb").stat().st_size > 0, name
        if name == "GWToolboxdll":
            assert manifest["display_version"] == f"{upstream}.{version}", manifest
print("DBBox and Toolbox release metadata is valid.")
