"""Check committed 1.0.10 states without loading plug-ins or requiring JUCE."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import re
import uuid
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent / "fixtures" / "plugins-1.0.10"
JUCE_ALPHABET = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def xml_from_binary(data):
    require(len(data) >= 8 and data[:4] == b"VC2!", "Invalid JUCE XML header")
    length = struct.unpack_from("<I", data, 4)[0]
    require(0 < length <= len(data) - 8, "Truncated JUCE XML payload")
    return ET.fromstring(data[8:8 + length])


def juce_base64(text):
    length_text, separator, payload = text.strip().partition(".")
    require(separator and length_text.isdecimal(), "Invalid JUCE base64 length")
    length = int(length_text)
    require(len(payload) == (length * 8 + 5) // 6, "Truncated JUCE base64 payload")
    value = 0
    for index, char in enumerate(payload):
        require(char in JUCE_ALPHABET, "Invalid JUCE base64 character")
        value |= JUCE_ALPHABET.index(char) << (6 * index)
    require(value.bit_length() <= length * 8, "Nonzero JUCE base64 padding")
    return value.to_bytes(length, "little")


def component_xml(data, plugin_format):
    if plugin_format == "vst2":
        require(len(data) >= 160 and data[:4] == b"CcnK" and data[8:12] == b"FBCh",
                "Invalid VST2 FXB chunk header")
        byte_size = struct.unpack_from(">I", data, 4)[0]
        chunk_size = struct.unpack_from(">I", data, 156)[0]
        # JUCE's released host writes zero to the optional FXB byteSize field.
        require(byte_size in (0, len(data) - 8) and chunk_size + 160 == len(data),
                "Invalid VST2 FXB chunk size")
        return xml_from_binary(data[160:])
    require(plugin_format == "vst3", "Unknown plug-in format")
    outer = xml_from_binary(data)
    component = outer.find("IComponent")
    require(component is not None, "Missing VST3 IComponent state")
    return xml_from_binary(juce_base64("".join(component.itertext())))


def tree_value(element):
    return (element.tag, element.attrib, (element.text or "").strip(),
            [tree_value(child) for child in element])


def fixture_file(directory, name):
    path = (directory / name).resolve()
    require(path.is_relative_to(directory.resolve()), "Fixture path leaves its directory")
    return path


def check_metadata(directory, manifest, provenance):
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    module_info = (directory / "moduleinfo.json").read_bytes()
    require(hashlib.sha256(module_info).hexdigest() == provenance["moduleInfo"]["sha256"],
            "Released moduleinfo hash mismatch")
    # The shipped SDK moduleinfo contains trailing commas; only CID strings are needed here.
    cids = re.findall(r'"CID"\s*:\s*"([0-9A-F]{32})"', module_info.decode("utf-8"))
    classes = metadata["classes"]
    actual_cids = [uuid.UUID(bytes_le=bytes(c["class_id_bytes"])).hex.upper() for c in classes]
    require(cids and sorted(cids) == sorted(actual_cids), "Factory/moduleinfo class IDs differ")
    audio_classes = [c for c in classes if c["category"] == "Audio Module Class"]
    require(len(audio_classes) == 1, "Expected one audio component")
    component = audio_classes[0]
    require(component["name"] == manifest["pluginName"] and component["version"] == "1.0.10",
            "Factory reports wrong plug-in identity/version")
    controller = component["controller"]
    controller_id = uuid.UUID(bytes_le=bytes(controller["controller_class_id_bytes"])).hex.upper()
    require(controller_id in actual_cids, "Controller class is absent from factory")
    parameters = controller["parameters"]
    host_parameters = manifest["cases"][0]["parameters"]
    require(len(parameters) == len(host_parameters) > 0, "Native/host parameter counts differ")
    require(len({p["id"] for p in parameters}) == len(parameters), "Duplicate VST3 parameter IDs")
    for native, host in zip(parameters, host_parameters):
        require(native["index"] == host["index"] and native["title"] == host["name"],
                "Native/host parameter ordering differs")
        require(0 <= native["id"] <= 0xffffffff, "Invalid native parameter ID")
        require(math.isclose(native["default_normalized"], host["defaultNormalizedValue"], abs_tol=1e-6),
                "Native/host parameter defaults differ")


def check_fixtures(root=ROOT):
    provenance = json.loads((root / "provenance.json").read_text(encoding="utf-8"))
    require(provenance["version"] == "1.0.10", "Wrong provenance version")
    binaries = {(b["plugin"], b["format"]): b for b in provenance["binaries"]}
    require(len(binaries) == len(provenance["binaries"]) == 6, "Expected six released binaries")
    count = 0
    for plugin, suffix, tag in (("track", "Track", "HEARASIDE_TRACK"),
                                ("hub", "Hub", "HEARASIDE_HUB"),
                                ("app", "App Audio", "HEARASIDE_APP")):
        for plugin_format in ("vst3", "vst2"):
            directory = root / plugin / plugin_format / "states"
            manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
            require(manifest["pluginName"] == f"HEARASIDE {suffix}", "Wrong plug-in name")
            require(manifest["pluginFormat"] == ("VST3" if plugin_format == "vst3" else "VST"),
                    "Wrong plug-in format")
            require(manifest["pluginVersion"] in ("1.0.10", "1.0.10.0"), "Wrong plug-in version")
            require(manifest["roundTripPassed"] is True, "Capture round-trip failed")
            if plugin_format == "vst3":
                check_metadata(directory.parent, manifest, binaries[plugin, plugin_format])
            cases = manifest["cases"]
            require(len(cases) == 6 and len({case["label"] for case in cases}) == 6,
                    "Expected six distinct state cases")
            for case in cases:
                data = fixture_file(directory, case["stateFile"]).read_bytes()
                require(len(data) == case["stateBytes"], "State size differs from manifest")
                actual = component_xml(data, plugin_format)
                expected = ET.parse(fixture_file(directory, case["componentXmlFile"])).getroot()
                require(actual.tag == tag, f"Unexpected component XML tag: {actual.tag}")
                if plugin_format == "vst2":
                    require(struct.unpack_from(">I", data, 16)[0] == manifest["uniqueId"],
                            "FXB/manifest plug-in unique IDs differ")
                require(tree_value(actual) == tree_value(expected), "Binary/XML state mismatch")
                parameters = case["parameters"]
                require(parameters and [p["index"] for p in parameters] == list(range(len(parameters))),
                        "Parameter indexes are missing or unordered")
                for parameter in parameters:
                    for key in ("normalizedValue", "defaultNormalizedValue"):
                        value = parameter[key]
                        require(math.isfinite(value) and 0 <= value <= 1, "Invalid normalized parameter")
                count += 1
    return count


def check_release(release_root, installer=None):
    provenance = json.loads((ROOT / "provenance.json").read_text(encoding="utf-8"))
    entries = list(provenance["binaries"])
    entries += [b["moduleInfo"] for b in provenance["binaries"] if "moduleInfo" in b]
    for entry in entries:
        data = fixture_file(release_root, entry["archiveMember"]).read_bytes()
        require(len(data) == entry["size"] and hashlib.sha256(data).hexdigest() == entry["sha256"],
                f"Released artifact hash mismatch: {entry['archiveMember']}")
    if installer is not None:
        data = installer.read_bytes()
        expected = provenance["installer"]
        require(len(data) == expected["size"] and hashlib.sha256(data).hexdigest() == expected["sha256"],
                "Installer hash mismatch")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-root", type=Path, help="Also verify extracted binaries against provenance")
    parser.add_argument("--installer", type=Path, help="Also verify installer (requires --release-root)")
    args = parser.parse_args()
    if args.installer and not args.release_root:
        parser.error("--installer requires --release-root")
    try:
        print(f"Validated {check_fixtures()} binary/XML state pairs from HEARASIDE 1.0.10")
        if args.release_root:
            check_release(args.release_root, args.installer)
            print("Released artifact hashes match provenance")
    except (ValueError, KeyError, OSError, ET.ParseError) as error:
        print(f"Fixture check failed: {error}", file=sys.stderr)
        sys.exit(1)
