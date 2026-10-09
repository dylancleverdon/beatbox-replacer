#!/bin/bash
# Builds BeatboxReplacer-macOS.pkg: the VST3 and the AU, installed for all users into
# /Library/Audio/Plug-Ins/{VST3,Components}, then owned by the logged-in user (see postinstall).
# The bundles must already be signed (CI ad-hoc signs them). Run on macOS.
#
# Usage: installer/mac/build_pkg.sh <BeatboxReplacer.vst3> <BeatboxReplacer.component> <version> <out.pkg>
set -euo pipefail

if [ $# -ne 4 ]; then
    echo "usage: $0 <path/to/BeatboxReplacer.vst3> <path/to/BeatboxReplacer.component> <version> <out.pkg>" >&2
    exit 2
fi

vst3="$1"
component="$2"
version="$3"
out="$4"
here="$(cd "$(dirname "$0")" && pwd)"
id_base="com.dylancleverdon.beatboxreplacer"

for bundle in "$vst3" "$component"; do
    if [ ! -d "$bundle/Contents" ]; then
        echo "error: not a bundle: $bundle" >&2
        exit 1
    fi
done

work="$(mktemp -d "${TMPDIR:-/tmp}/bbr-pkg.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/pkgs" "$(dirname "$out")"

# One idempotent postinstall handles both bundles; attached to both components so it runs
# whichever ones get installed.
cp "$here/postinstall" "$work/scripts/postinstall"
chmod 755 "$work/scripts/postinstall"

# make_component <bundle> <name> <install location>
make_component()
{
    local bundle="$1" name="$2" location="$3"
    local root="$work/root-$name"
    local plist="$work/$name-components.plist"

    mkdir -p "$root"
    ditto "$bundle" "$root/$(basename "$bundle")"
    xattr -cr "$root" 2>/dev/null || true

    # Both bundles share one CFBundleIdentifier, so Installer would "relocate" one into the
    # other's location (or into a copy elsewhere on disk) unless relocation is off.
    pkgbuild --analyze --root "$root" "$plist"
    python3 - "$plist" <<'PY'
import plistlib, sys
path = sys.argv[1]
with open(path, "rb") as f:
    entries = plistlib.load(f)
if not isinstance(entries, list) or not entries:
    sys.exit(f"error: unexpected component plist {path}: {entries!r}")
for entry in entries:
    entry["BundleIsRelocatable"] = False
    entry["BundleIsVersionChecked"] = False   # always install this build (also allows rollback)
    entry["BundleOverwriteAction"] = "upgrade"
with open(path, "wb") as f:
    plistlib.dump(entries, f)
PY
    echo "--- $name component plist"
    plutil -p "$plist"

    pkgbuild --root "$root" \
             --component-plist "$plist" \
             --identifier "$id_base.$name.pkg" \
             --version "$version" \
             --install-location "$location" \
             --scripts "$work/scripts" \
             "$work/pkgs/BeatboxReplacer-$name.pkg"
}

make_component "$vst3" vst3 "/Library/Audio/Plug-Ins/VST3"
make_component "$component" au "/Library/Audio/Plug-Ins/Components"

productbuild --synthesize \
             --package "$work/pkgs/BeatboxReplacer-vst3.pkg" \
             --package "$work/pkgs/BeatboxReplacer-au.pkg" \
             "$work/distribution.xml"

python3 - "$work/distribution.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

path = sys.argv[1]
tree = ET.parse(path)
root = tree.getroot()

inserted = 0

def child(tag):
    # Existing element, or a new one placed after the ones added before it (ahead of the
    # synthesized pkg-refs and choices).
    global inserted
    element = root.find(tag)
    if element is None:
        element = ET.Element(tag)
        root.insert(inserted, element)
        inserted += 1
    return element

child("title").text = "Beatbox Replacer"

options = child("options")
options.set("customize", "never")
options.set("hostArchitectures", "x86_64,arm64")   # no Rosetta prompt on Apple silicon

domains = child("domains")
domains.set("enable_localSystem", "true")
domains.set("enable_currentUserHome", "false")
domains.set("enable_anywhere", "false")

os_versions = child("allowed-os-versions")
if os_versions.find("os-version") is None:
    ET.SubElement(os_versions, "os-version", {"min": "10.15"})

child("welcome").attrib.update({"file": "welcome.txt", "mime-type": "text/plain"})
child("conclusion").attrib.update({"file": "conclusion.txt", "mime-type": "text/plain"})

if hasattr(ET, "indent"):
    ET.indent(tree, "    ")
tree.write(path, encoding="utf-8", xml_declaration=True)
PY
echo "--- distribution.xml"
cat "$work/distribution.xml"

productbuild --distribution "$work/distribution.xml" \
             --package-path "$work/pkgs" \
             --resources "$here/resources" \
             "$out"

# Verify: two components, nothing relocatable, payload paths as expected.
pkgutil --expand "$out" "$work/expanded"
python3 - "$work/expanded" <<'PY'
import glob, os, sys
import xml.etree.ElementTree as ET

expanded = sys.argv[1]
infos = sorted(glob.glob(os.path.join(expanded, "*.pkg", "PackageInfo")))
if len(infos) != 2:
    sys.exit(f"error: expected 2 component packages, found {len(infos)}")
for path in infos:
    info = ET.parse(path).getroot()
    relocate = info.find("relocate")
    targets = [] if relocate is None else list(relocate)
    print(f"{os.path.basename(os.path.dirname(path))}: id={info.get('identifier')} "
          f"version={info.get('version')} install-location={info.get('install-location')} "
          f"relocate targets={len(targets)}")
    if targets:
        sys.exit("error: package has relocate targets:\n" + open(path).read())
PY

for item in "vst3:./BeatboxReplacer.vst3/Contents/MacOS/BeatboxReplacer" \
            "au:./BeatboxReplacer.component/Contents/MacOS/BeatboxReplacer"; do
    bom="$work/expanded/BeatboxReplacer-${item%%:*}.pkg/Bom"
    listing="$(lsbom -s "$bom")"
    if ! grep -qxF "${item#*:}" <<< "$listing"; then
        echo "error: ${item#*:} missing from $bom" >&2
        echo "$listing" >&2
        exit 1
    fi
done

if ! grep -q "<title>Beatbox Replacer</title>" "$work/expanded/Distribution"; then
    echo "error: Distribution has no title" >&2
    exit 1
fi

echo "Built $out ($version)"
