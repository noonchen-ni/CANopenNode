"""Converter unit tests, plus optional validation against the real C++ model loader."""

from __future__ import annotations

import configparser
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from eds_to_json import ConversionError, Converter, USER_INPUT, convert_eds, guide_markdown, integer_expression, main, read_eds


def sections() -> dict[str, dict[str, str]]:
    return {
        "DeviceInfo": {"VendorNumber": "0x300", "ProductNumber": "0x4644", "RevisionNumber": "0"},
        "1018sub4": {"DefaultValue": "123"},
        "1000": {"DefaultValue": "0x20192"},
        "1017": {"DefaultValue": "0"},
        "2000": {"ObjectType": "7", "DataType": "6", "AccessType": "rw",
                 "DefaultValue": "3", "PDOMapping": "1"},
        "1400": {"ObjectType": "9"},
        "1400sub1": {"DefaultValue": "$NODEID+0x200"},
        "1400sub2": {"DefaultValue": "255"},
        "1400sub5": {"DefaultValue": "0"},
        "1600": {"ObjectType": "9"},
        "1600sub0": {"DefaultValue": "1"},
        "1600sub1": {"DefaultValue": "0x20000010"},
    }


def parse(data: dict[str, dict[str, str]]) -> tuple[dict, list[str]]:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_dict(data)
    converter = Converter(parser)
    return converter.convert(), converter.warnings


def fill_for_loader(template: dict) -> dict:
    """Supply test-only defaults; production conversion must keep unknowns explicit."""
    model = copy.deepcopy(template)
    model.update(node_id=10, bitrate_kbps=125, worker_period_us=1000)
    model["identity"]["serial_number"] = 123

    def fill(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if item == USER_INPUT:
                    value[key] = (False if key == "local_write" else 128 if key == "size"
                                  else "" if key == "value" and value.get("type") == "VISIBLE_STRING"
                                  else [] if key == "value" and value.get("type") == "OCTET_STRING"
                                  else 0)
                else:
                    fill(item)
        elif isinstance(value, list):
            for item in value:
                fill(item)

    fill(model)
    return model


class ConverterTests(unittest.TestCase):
    def test_constants_and_restricted_expressions(self):
        for text, expected in {
            "0125": (125, False), "0X80": (128, False), "-2": (-2, False),
            "$NODEID+0x200": (512, True), "$nodeid + 512": (512, True),
            "0x200 + $NODEID": (512, True), "$NODEID": (0, True),
        }.items():
            with self.subTest(text=text):
                self.assertEqual(integer_expression(text), expected)
        for text in ("$NODEID*2", "__import__('os')", "1.5", "$NODEID-1"):
            with self.subTest(text=text), self.assertRaises(ConversionError):
                integer_expression(text)

    def test_template_and_actual_pdo_mapping(self):
        data = sections()
        data["1600sub2"] = {"DefaultValue": "0xDEAD0020"}  # Unused capacity, not an active mapping.
        model, warnings = parse(data)
        self.assertEqual(model["node_id"], USER_INPUT)
        self.assertEqual(model["bitrate_kbps"], USER_INPUT)
        self.assertEqual(model["worker_period_us"], USER_INPUT)
        self.assertEqual(model["identity"]["vendor_id"], 768)
        self.assertEqual(model["heartbeat_ms"], 0)
        self.assertEqual(model["rpdos"][0]["cob_id"], 512)
        self.assertEqual(model["rpdos"][0]["cob_id_offset"], "NODE_ID")
        self.assertEqual(model["rpdos"][0]["mapping"], [{"index": "0x2000", "subindex": 0}])
        self.assertIs(model["objects"][0]["local_write"], True)
        self.assertTrue(any("/node_id" in warning for warning in warnings))

    def test_missing_and_unsupported_defaults(self):
        data = sections()
        del data["2000"]["DefaultValue"]
        model, warnings = parse(data)
        self.assertEqual(model["objects"][0]["value"], 0)
        self.assertTrue(any("[2000] DefaultValue" in warning and "using default 0" in warning for warning in warnings))
        data["2000"]["DefaultValue"] = "$NODEID*2"
        model, warnings = parse(data)
        self.assertEqual(model["objects"][0]["value"], USER_INPUT)
        self.assertTrue(any("'$NODEID*2'" in warning for warning in warnings))
        data["2000"]["DefaultValue"] = "2+3"
        model, warnings = parse(data)
        self.assertEqual(model["objects"][0]["value"], USER_INPUT)
        self.assertTrue(any("'2+3'" in warning for warning in warnings))
        data["2000"]["DefaultValue"] = "broken"
        with self.assertRaises(ConversionError):
            parse(data)

    def test_missing_values_use_type_appropriate_zero_defaults(self):
        data = sections()
        for index, data_type in enumerate(("1", "2", "3", "4", "5", "6", "7", "8", "0x11",
                                           "0x15", "0x1B", "9", "0xA"), 0x2100):
            data[f"{index:04X}"] = {"DataType": data_type, "AccessType": "ro", "PDOMapping": "0"}
        model, _ = parse(data)
        for obj in model["objects"][1:]:
            expected = "" if obj["type"] == "VISIBLE_STRING" else [] if obj["type"] == "OCTET_STRING" else 0
            with self.subTest(type=obj["type"]):
                self.assertEqual(obj["value"], expected)
                self.assertIs(obj["local_write"], True)
        self.assertEqual(model["objects"][0]["value"], 3)  # Existing EDS defaults are retained.

    def test_missing_numeric_communication_defaults_and_identity(self):
        data = sections()
        del data["1400sub2"]["DefaultValue"]
        del data["1400sub5"]["DefaultValue"]
        del data["DeviceInfo"]["ProductNumber"]
        del data["DeviceInfo"]["RevisionNumber"]
        del data["1018sub4"]["DefaultValue"]
        data["1005"] = {"DefaultValue": "0x80"}
        model, warnings = parse(data)
        self.assertEqual(model["rpdos"][0]["transmission_type"], 0)
        self.assertEqual(model["rpdos"][0]["event_ms"], 0)
        self.assertEqual(model["sync"]["window_us"], 0)
        self.assertEqual(model["sync"]["counter_overflow"], 0)
        self.assertEqual(model["identity"]["product_code"], 0)
        self.assertEqual(model["identity"]["revision_number"], 0)
        self.assertEqual(model["identity"]["serial_number"], USER_INPUT)
        self.assertTrue(any("/rpdos/0/transmission_type" in warning and "using default 0" in warning for warning in warnings))

    def test_input_guide_describes_every_remaining_placeholder(self):
        data = sections()
        data["DeviceInfo"].update(BaudRate_125="1", BaudRate_500="1", BaudRate_1000="0")
        data["2001"] = {"DataType": "9", "AccessType": "rw", "PDOMapping": "0",
                        "DefaultValue": "example", "ParameterName": "Device label"}
        data["2002"] = {"DataType": "3", "AccessType": "rw", "PDOMapping": "0", "DefaultValue": "$NODEID*2"}
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_dict(data)
        converter = Converter(parser)
        model = converter.convert()
        guide = converter.input_guide(model, "model.json")
        self.assertEqual(guide["model_file"], "model.json")
        self.assertEqual(len(guide["inputs"]), json.dumps(model).count(USER_INPUT))
        hints = {hint["path"]: hint for hint in guide["inputs"]}
        self.assertEqual(hints["/bitrate_kbps"]["eds_supported_values"], [125, 500])
        self.assertEqual(hints["/worker_period_us"]["example"], 1000)
        self.assertIn("1..127", hints["/node_id"]["expected"])
        string = hints["/objects/1/size"]
        self.assertEqual(string["parameter_name"], "Device label")
        self.assertEqual(string["od_address"], "0x2001:00")
        self.assertEqual(string["label"], "index=0x2001, subindex=0x00, size")
        self.assertEqual(string["editor_search"], "0x2001")
        self.assertEqual(string["expected"], "integer 8..4096")
        self.assertGreaterEqual(string["example"], 8)
        expression = hints["/objects/2/value"]
        self.assertEqual(expression["eds_default"], "$NODEID*2")
        self.assertIn("-32768..32767", expression["expected"])
        for hint in guide["inputs"]:
            value = model
            for part in hint["path"].split("/")[1:]:
                value = value[int(part)] if isinstance(value, list) else value[part]
            self.assertEqual(value, USER_INPUT)
            self.assertIn("description", hint)
            self.assertIn("example", hint)
        self.assertEqual(guide["warnings"], converter.warnings)

    def test_missing_active_mapping_and_type_remain_placeholders(self):
        data = sections()
        del data["1600sub1"]["DefaultValue"]
        model, warnings = parse(data)
        self.assertEqual(model["rpdos"][0]["mapping"], USER_INPUT)
        self.assertTrue(any("active mapping words missing" in warning for warning in warnings))
        data = sections()
        del data["2000"]["DataType"]
        model, warnings = parse(data)
        self.assertEqual(model["objects"][0]["type"], USER_INPUT)
        self.assertTrue(any("verify 16-bit width" in warning for warning in warnings))

    def test_markdown_guide_preserves_details_and_escapes_source_text(self):
        data = sections()
        data["2001"] = {"DataType": "9", "AccessType": "rw", "PDOMapping": "0",
                        "DefaultValue": "label", "ParameterName": "Label | `name` <tag>"}
        data["2002"] = {"DataType": "3", "AccessType": "rw", "PDOMapping": "0", "DefaultValue": "$NODEID*2"}
        data["DeviceInfo"]["BaudRate_125"] = "1"
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_dict(data)
        converter = Converter(parser)
        model = converter.convert()
        guide = converter.input_guide(model, "my model.json")
        markdown = guide_markdown(guide)
        self.assertTrue(markdown.startswith("# EDS conversion input guide\n"))
        self.assertIn("[my model.json](./my%20model.json)", markdown)
        self.assertIn(f"## Required inputs ({len(guide['inputs'])})", markdown)
        self.assertIn("EDS-supported values: 125.", markdown)
        self.assertIn(r"Label \| \`name\` \<tag\>", markdown)
        self.assertIn("- OD address: 0x2001:00", markdown)
        self.assertIn("### `index=0x2001, subindex=0x00, size`", markdown)
        self.assertNotIn("### `/objects/1/size`", markdown)
        self.assertIn("Search in the editor for: `0x2001`", markdown)
        self.assertIn(r'Original EDS default: "$NODEID\*2"', markdown)
        self.assertIn(f"## Conversion warnings ({len(guide['warnings'])})", markdown)
        for hint in guide["inputs"]:
            self.assertIn(hint["path"], markdown)
        empty = dict(guide, inputs=[], warnings=[])
        self.assertIn("No unresolved input placeholders remain.", guide_markdown(empty))

    def test_boolean_mapping_uses_runtime_byte_width(self):
        data = sections()
        data["2000"].update(DataType="1", DefaultValue="1")
        data["1600sub1"]["DefaultValue"] = "0x20000008"
        model, _ = parse(data)
        self.assertEqual(model["objects"][0]["type"], "BOOLEAN")
        data["1600sub1"]["DefaultValue"] = "0x20000001"
        with self.assertRaisesRegex(ConversionError, "bit length"):
            parse(data)
        data["2000"]["DefaultValue"] = "2"
        with self.assertRaisesRegex(ConversionError, "BOOLEAN"):
            parse(data)

    def test_signed_hex_wide_integer_and_offsets(self):
        data = sections()
        data["2001"] = {"DataType": "3", "AccessType": "ro", "PDOMapping": "0", "DefaultValue": "0xFFFF"}
        data["2002"] = {"DataType": "0x1B", "AccessType": "rw", "PDOMapping": "0",
                        "DefaultValue": "18446744073709551615"}
        data["2003"] = {"DataType": "7", "AccessType": "rw", "PDOMapping": "0", "DefaultValue": "$NODEID+0x80"}
        model, _ = parse(data)
        self.assertEqual(model["objects"][1]["value"], -1)
        self.assertEqual(model["objects"][2]["value"], 2**64 - 1)
        self.assertEqual(model["objects"][3]["value_offset"], "NODE_ID")
        self.assertEqual(json.loads(json.dumps(model))["objects"][2]["value"], 2**64 - 1)

    def test_writable_string_size_is_not_guessed(self):
        data = sections()
        data["2001"] = {"DataType": "9", "AccessType": "rw", "PDOMapping": "0",
                        "DefaultValue": "20% complete"}
        model, warnings = parse(data)
        entry = model["objects"][1]
        self.assertEqual(entry["value"], "20% complete")
        self.assertEqual(entry["size"], USER_INPUT)
        self.assertEqual(entry["access"], "rw")
        self.assertTrue(any("string storage capacity" in warning for warning in warnings))

    def test_read_only_string_capacity_is_inferred(self):
        for access, data_type, default, size in (
            ("ro", "9", "abc", 4), ("const", "9", "abc", 4), ("ro", "9", "", 1),
            ("ro", "0xA", "0102", 2), ("const", "0xA", "", 1),
        ):
            with self.subTest(access=access, type=data_type, default=default):
                data = sections()
                data["2001"] = {"DataType": data_type, "AccessType": access, "PDOMapping": "0",
                                "DefaultValue": default}
                model, warnings = parse(data)
                entry = model["objects"][1]
                self.assertEqual(entry["size"], size)
                self.assertEqual(entry["access"], "ro")
                self.assertIs(entry["local_write"], True)
                self.assertTrue(any("index=0x2001" in warning and "capacity inferred" in warning for warning in warnings))
        data["2001"].update(DataType="9", DefaultValue="\u00e9")
        model, _ = parse(data)
        self.assertEqual(model["objects"][1]["value"], USER_INPUT)
        self.assertEqual(model["objects"][1]["size"], USER_INPUT)
        data["2001"]["DefaultValue"] = "a" * 4095
        self.assertEqual(parse(data)[0]["objects"][1]["size"], 4096)
        data["2001"]["DefaultValue"] = "a" * 4096
        with self.assertRaisesRegex(ConversionError, "4096-byte"):
            parse(data)

    def test_record_member_guide_uses_searchable_index_and_subindex(self):
        data = sections()
        data["2001"] = {"ObjectType": "9", "SubNumber": "2"}
        data["2001sub0"] = {"DataType": "5", "AccessType": "ro", "PDOMapping": "0", "DefaultValue": "10"}
        data["2001subA"] = {"DataType": "9", "AccessType": "rw", "PDOMapping": "0", "DefaultValue": "abc"}
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_dict(data)
        converter = Converter(parser)
        model = converter.convert()
        guide = converter.input_guide(model, "model.json")
        hint = next(hint for hint in guide["inputs"] if hint["path"] == "/objects/1/members/1/size")
        self.assertEqual(hint["label"], "index=0x2001, subindex=0x0A, size")
        self.assertIn("### `index=0x2001, subindex=0x0A, size`", guide_markdown(guide))

    def test_record_and_sparse_array_preserve_subindices(self):
        data = sections()
        data["2001"] = {"ObjectType": "8", "SubNumber": "3"}
        data["2001sub0"] = {"DataType": "5", "AccessType": "rw", "DefaultValue": "2", "PDOMapping": "0"}
        data["2001sub1"] = {"DataType": "6", "AccessType": "rw", "DefaultValue": "1", "PDOMapping": "0"}
        data["2001subA"] = {"DataType": "7", "AccessType": "ro", "DefaultValue": "2", "PDOMapping": "0"}
        model, warnings = parse(data)
        obj = model["objects"][1]
        self.assertEqual(obj["kind"], "record")
        self.assertEqual([m["subindex"] for m in obj["members"]], [0, 1, 10])
        self.assertEqual(obj["members"][0], {"subindex": 0, "type": "UNSIGNED8", "value": 10, "access": "ro"})
        self.assertTrue(any("normalized" in warning for warning in warnings))

    def test_invalid_mappings_are_errors(self):
        for word in ("0x20000008", "0x22220010", "0x20000110"):
            data = sections()
            data["1600sub1"]["DefaultValue"] = word
            with self.subTest(word=word), self.assertRaises(ConversionError):
                parse(data)
        data = sections()
        data["2000"]["AccessType"] = "ro"
        with self.assertRaisesRegex(ConversionError, "direction"):
            parse(data)
        data = sections()
        data["1600sub0"]["DefaultValue"] = "5"
        for sub in range(2, 6):
            data[f"1600sub{sub}"] = {"DefaultValue": "0x20000010"}
        with self.assertRaisesRegex(ConversionError, "exceeds 64"):
            parse(data)

    def test_disabled_pdos_and_fifth_slot(self):
        data = sections()
        data["1400sub1"]["DefaultValue"] = "0x80000200"
        data["1404"] = {"ObjectType": "9"}
        data["1404sub1"] = {"DefaultValue": "$NODEID+0x240"}
        data["1404sub2"] = {"DefaultValue": "254"}
        data["1404sub5"] = {"DefaultValue": "0"}
        data["1604sub0"] = {"DefaultValue": "0"}
        data["1604"] = {"ObjectType": "9"}
        model, warnings = parse(data)
        self.assertFalse(model["rpdos"][0]["enabled"])
        self.assertEqual(len(model["rpdos"]), 5)
        self.assertEqual(model["rpdos"][4]["cob_id_offset"], "NODE_ID")
        self.assertFalse(model["rpdos"][4]["enabled"])
        self.assertEqual(model["rpdos"][1]["mapping"], USER_INPUT)
        self.assertTrue(any("empty mapping" in warning for warning in warnings))

    def test_unsupported_features_are_reported(self):
        data = sections()
        data["1008"] = {"DataType": "9", "AccessType": "const"}
        data["2001"] = {"DataType": "0xF", "AccessType": "rw", "PDOMapping": "0"}
        model, warnings = parse(data)
        self.assertEqual(model["objects"][1]["type"], USER_INPUT)
        self.assertTrue(any("[1008] omitted" in warning for warning in warnings))
        self.assertTrue(any("unsupported or missing type" in warning for warning in warnings))
        data["2001"]["CompactSubObj"] = "5"
        with self.assertRaisesRegex(ConversionError, "compact"):
            parse(data)

    def test_cli_warnings_errors_encoding_and_source_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input.eds", Path(directory) / "output.json"
            source.write_text("[DeviceInfo]\nVendorName=caf\u00e9\n", encoding="cp1252")
            with contextlib.redirect_stderr(io.StringIO()) as errors, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(source), str(output), "--encoding", "cp1252"]), 0)
            self.assertIn("WARNING:", errors.getvalue())
            self.assertEqual(json.loads(output.read_text())["node_id"], USER_INPUT)
            guide_path = output.with_suffix(".inputs.md")
            guide = guide_path.read_text()
            self.assertIn("[output.json](./output.json)", guide)
            self.assertIn("/node_id", guide)
            self.assertFalse(output.with_suffix(".inputs.json").exists())
            output.write_text("unchanged", encoding="utf-8")
            guide_path.write_text("unchanged guide", encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(source), str(output), "--encoding", "cp1252", "--warnings-as-errors"]), 1)
                self.assertEqual(main([str(source), str(source)]), 1)
            self.assertEqual(output.read_text(), "unchanged")
            self.assertEqual(guide_path.read_text(), "unchanged guide")
            self.assertIn("caf\u00e9", source.read_text(encoding="cp1252"))
            protected_source = Path(directory) / "protected.inputs.md"
            protected_source.write_text("[DeviceInfo]\nVendorNumber=1\n", encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main([str(protected_source), str(Path(directory) / "protected.json")]), 1)
            self.assertEqual(protected_source.read_text(), "[DeviceInfo]\nVendorNumber=1\n")

    def test_cli_output_defaults_to_eds_folder(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / "nested eds folder"
            folder.mkdir()
            source = folder / "device.v1.EDS"
            source.write_text("[DeviceInfo]\nVendorName=caf\u00e9\n", encoding="cp1252")
            output = folder / "device.v1.json"
            guide = folder / "device.v1.inputs.md"
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(source), "--encoding", "cp1252"]), 0)
            self.assertEqual(json.loads(output.read_text())["node_id"], USER_INPUT)
            self.assertIn("[device.v1.json](./device.v1.json)", guide.read_text())
            output.write_text("unchanged", encoding="utf-8")
            guide.write_text("unchanged guide", encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(source), "--encoding", "cp1252", "--warnings-as-errors"]), 1)
            self.assertEqual(output.read_text(), "unchanged")
            self.assertEqual(guide.read_text(), "unchanged guide")
            self.assertIn("caf\u00e9", source.read_text(encoding="cp1252"))

    def test_supplied_kinco_eds(self):
        source = ROOT / "eds" / "Kinco_FD_20250417_V1.2.eds"
        if not source.exists():
            self.skipTest("customer-supplied Kinco EDS is not present")
        model, warnings = convert_eds(source)
        self.assertEqual(len(model["rpdos"]), 8)
        self.assertEqual(len(model["tpdos"]), 8)
        self.assertEqual(model["rpdos"][0]["mapping"],
                         [{"index": "0x607A", "subindex": 0}, {"index": "0x6060", "subindex": 0},
                          {"index": "0x6040", "subindex": 0}])
        self.assertEqual(model["rpdos"][4]["cob_id"], 0x240)
        self.assertEqual(model["rpdos"][4]["cob_id_offset"], "NODE_ID")
        self.assertEqual(model["identity"]["serial_number"], USER_INPUT)
        self.assertTrue(warnings)
        converter = read_eds(source)
        model = converter.convert()
        guide = converter.input_guide(model, "kinco.template.json")
        self.assertEqual({hint["path"] for hint in guide["inputs"]},
                         {"/node_id", "/bitrate_kbps", "/worker_period_us", "/identity/serial_number"})
        manufacturer = next(obj for obj in model["objects"] if obj["index"] == "0x6504")
        self.assertEqual(manufacturer["size"], len(manufacturer["value"].encode("ascii")) + 1)
        self.assertEqual(manufacturer["size"], 31)
        self.assertEqual(manufacturer["access"], "ro")
        for obj in model["objects"]:
            for value in obj.get("members", [obj]):
                if obj.get("kind") == "record" and value["subindex"] == 0:
                    self.assertNotIn("local_write", value)
                else:
                    self.assertIs(value["local_write"], True)

    @unittest.skipUnless(os.environ.get("XNET_CO_MODEL_VALIDATOR"), "C++ model validator not configured")
    def test_generated_json_loads_in_real_runtime(self):
        templates = [parse(sections())[0]]
        kinco = ROOT / "eds" / "Kinco_FD_20250417_V1.2.eds"
        if kinco.exists():
            templates.append(convert_eds(kinco)[0])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "model.json"
            for template in templates:
                with self.subTest(objects=len(template["objects"])):
                    output.write_text(json.dumps(fill_for_loader(template)), encoding="utf-8")
                    result = subprocess.run([os.environ["XNET_CO_MODEL_VALIDATOR"], "--validate-model", str(output)],
                                            capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
