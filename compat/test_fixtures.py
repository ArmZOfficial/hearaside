import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from check_fixtures import component_xml, juce_base64, xml_from_binary
from check_golden import check_golden


class StateEnvelopeTests(unittest.TestCase):
    def test_declared_xml_length_cannot_exceed_buffer(self):
        with self.assertRaisesRegex(ValueError, "Truncated"):
            xml_from_binary(b"VC2!" + struct.pack("<I", 100) + b"<state/>")

    def test_xml_envelope_preserves_unicode_and_allows_wrapper_trailer(self):
        payload = '<state name="เสียงร้อง"/>'.encode()
        state = b"VC2!" + struct.pack("<I", len(payload)) + payload + b"\0\0"
        self.assertEqual(xml_from_binary(state).attrib["name"], "เสียงร้อง")

    def test_juce_base64_uses_little_endian_bits(self):
        self.assertEqual(juce_base64("1.AB"), b"\x81")

    def test_juce_base64_rejects_truncation_and_bad_padding(self):
        for text in ("1.A", "1.A+", "1.A?", "-1.AC"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                juce_base64(text)

    def test_fxb_chunk_cannot_extend_past_file(self):
        header = bytearray(160)
        header[:4] = b"CcnK"
        header[8:12] = b"FBCh"
        struct.pack_into(">I", header, 4, 152)
        struct.pack_into(">I", header, 156, 999)
        with self.assertRaisesRegex(ValueError, "chunk size"):
            component_xml(header, "vst2")

    def test_golden_inventory_rejects_corrupted_and_extra_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "core").mkdir()
            sample = root / "core" / "test.f32"
            data = struct.pack("<f", 0.5)
            sample.write_bytes(data)
            record = {"path": "core/test.f32", "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()}
            (root / "provenance.json").write_text(json.dumps({"files": [record]}))
            self.assertEqual(check_golden(root), 1)
            sample.write_bytes(struct.pack("<f", 0.25))
            with self.assertRaisesRegex(ValueError, "differs"):
                check_golden(root)
            sample.write_bytes(data)
            (root / "core" / "extra.f32").write_bytes(data)
            with self.assertRaisesRegex(ValueError, "inventory"):
                check_golden(root)


if __name__ == "__main__":
    unittest.main()
