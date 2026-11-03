#!/usr/bin/env python3
"""Convert an EDS into an XNET CANopen JSON template (Python 3.10+, standard library)."""

from __future__ import annotations

import argparse
import configparser
import json
import math
import re
import struct
import sys
from collections import Counter
from pathlib import Path
from typing import Any
from urllib.parse import quote

USER_INPUT = "{USER_INPUT}"
TYPES = {
    0x01: ("BOOLEAN", 8), 0x02: ("INTEGER8", 8), 0x03: ("INTEGER16", 16),
    0x04: ("INTEGER32", 32), 0x05: ("UNSIGNED8", 8), 0x06: ("UNSIGNED16", 16),
    0x07: ("UNSIGNED32", 32), 0x08: ("REAL32", 32), 0x09: ("VISIBLE_STRING", None),
    0x0A: ("OCTET_STRING", None), 0x11: ("REAL64", 64),
    0x15: ("INTEGER64", 64), 0x1B: ("UNSIGNED64", 64),
}
INTEGER = r"(?:0[xX][0-9a-fA-F]+|[0-9]+)"
NODE_EXPRESSION = re.compile(rf"^\$NODEID(?:\s*\+\s*({INTEGER}))?$", re.IGNORECASE)
REVERSED_NODE_EXPRESSION = re.compile(rf"^({INTEGER})\s*\+\s*\$NODEID$", re.IGNORECASE)
INPUT_HINTS = {
    "node_id": ("integer 1..127, or 255 for LSS commissioning", "Choose the simulated slave's CANopen node ID.", 10),
    "bitrate_kbps": ("integer 1..1000", "Choose a bitrate supported by both the device and the XNET interface, in kbit/s.", 125),
    "worker_period_us": ("integer 100..100000", "Worker scheduling period in microseconds; 1000 is 1 ms.", 1000),
    "vendor_id": ("integer 1..4294967295", "Device vendor ID; must be nonzero.", 768),
    "product_code": ("integer 0..4294967295", "Device product code.", 1),
    "revision_number": ("integer 0..4294967295", "Device revision number.", 0),
    "serial_number": ("integer 1..4294967295", "Choose a nonzero serial number unique to this simulated device.", 1001),
    "type": ("one of " + ", ".join(name for name, _ in TYPES.values()), "Choose a supported OD storage type.", "UNSIGNED32"),
    "access": ("ro, wo, or rw", "Network SDO access permission, independent of host local_write.", "rw"),
    "pdo": ("none, tx, rx, or both", "PDO eligibility relative to the simulated slave; must agree with actual mappings.", "none"),
    "enabled": ("boolean", "Whether this PDO is enabled; an enabled PDO requires a nonempty mapping.", False),
    "mapping": ("array of index/subindex objects, at most 8 entries and 64 payload bits",
                "Decode the active EDS mapping words; each entry must map the complete target value.",
                [{"index": "0x2000", "subindex": 0}]),
    "transmission_type": ("integer 0..240, 254, or 255",
                          "0 waits for SYNC after a request; 1..240 are cyclic SYNC; 254/255 are asynchronous.", 255),
    "event_ms": ("integer 0..65535", "TPDO event period or RPDO timeout, in milliseconds; zero disables the timer.", 0),
    "inhibit_100us": ("integer 0..65535", "Minimum transmission separation in units of 100 microseconds.", 0),
    "heartbeat_ms": ("integer 0..65535", "Heartbeat period in milliseconds; zero disables heartbeat production.", 1000),
    "device_type": ("integer 0..4294967295", "CANopen device type from object 0x1000.", 0),
    "guard_time_ms": ("integer 0..65535", "Node guard time in milliseconds; guard time times life factor must fit uint32 microseconds.", 1000),
    "life_time_factor": ("integer 0..255", "Node guarding lifetime multiplier; zero disables timeout monitoring.", 3),
    "period_us": ("integer 0..4294967295", "Expected SYNC period in microseconds; zero disables period monitoring.", 0),
    "window_us": ("integer 0..4294967295", "Synchronous window length in microseconds; zero disables the window limit.", 0),
    "counter_overflow": ("integer 0 or 2..240", "SYNC counter rollover; zero selects SYNC frames without a counter.", 0),
}


class ConversionError(ValueError):
    """EDS content cannot be represented safely."""


def integer(text: str) -> int:
    text = text.strip()
    if not re.fullmatch(r"[+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+)", text):
        raise ConversionError(f"invalid integer {text!r}")
    return int(text, 16 if text.lstrip("+-").lower().startswith("0x") else 10)


def integer_expression(text: str) -> tuple[int, bool]:
    match = NODE_EXPRESSION.fullmatch(text.strip())
    if match:
        return integer(match.group(1) or "0"), True
    match = REVERSED_NODE_EXPRESSION.fullmatch(text.strip())
    if match:
        return integer(match.group(1)), True
    return integer(text), False


def minimum_string_capacity(type_name: str, value: Any) -> int:
    if value == USER_INPUT:
        return 1
    if type_name == "VISIBLE_STRING" and isinstance(value, str):
        return len(value.encode("ascii")) + 1
    if type_name == "OCTET_STRING" and isinstance(value, list):
        return max(1, len(value))
    raise ConversionError(f"invalid {type_name} string default")


class Converter:
    def __init__(self, source: configparser.ConfigParser):
        if source.defaults():
            raise ConversionError("[DEFAULT] inheritance is not supported in EDS files")
        self.sections: dict[str, dict[str, str]] = {}
        self.names: dict[str, str] = {}
        for name in source.sections():
            key = name.lower()
            if key in self.sections:
                raise ConversionError(f"duplicate section [{name}] (case-insensitive)")
            self.sections[key] = dict(source[name])
            self.names[key] = name
        self.used: set[tuple[str, str]] = set()
        self.warnings: list[str] = []
        self.entries: dict[tuple[int, int], dict[str, Any]] = {}
        self.widths: dict[tuple[int, int], int | None] = {}

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def take(self, section: str, key: str) -> str | None:
        section, key = section.lower(), key.lower()
        self.used.add((section, key))
        return self.sections.get(section, {}).get(key)

    def missing(self, section: str, key: str, path: str) -> str:
        self.warn(f"[{section}] {key} -> {path}: unavailable; supply {USER_INPUT}")
        return USER_INPUT

    def number(self, section: str, key: str, path: str, maximum: int = 0xFFFFFFFF,
               offset: bool = False, default: int | None = None) -> dict[str, Any]:
        raw = self.take(section, key)
        if raw is None or not raw.strip():
            if default is not None:
                self.warn(f"[{section}] {key} -> {path}: unavailable; using default {default}")
                return {"value": default}
            return {"value": self.missing(section, key, path)}
        try:
            value, relative = integer_expression(raw)
        except ConversionError:
            if not re.search(r"[$+*/()%^-]", raw):
                raise ConversionError(f"[{section}] {key}: invalid number {raw!r}") from None
            self.warn(f"[{section}] {key} -> {path}: unsupported expression {raw!r}; supply {USER_INPUT}")
            return {"value": USER_INPUT}
        if relative and not offset:
            self.warn(f"[{section}] {key} -> {path}: NODE_ID offset unsupported here; supply {USER_INPUT}")
            return {"value": USER_INPUT}
        if not 0 <= value <= maximum:
            raise ConversionError(f"[{section}] {key}: {value} outside 0..{maximum}")
        return {"value": value, **({"value_offset": "NODE_ID"} if relative else {})}

    def default(self, section: str, type_name: str, bits: int | None, path: str) -> dict[str, Any]:
        raw = self.take(section, "DefaultValue")
        if raw is None or (not raw.strip() and type_name not in {"VISIBLE_STRING", "OCTET_STRING"}):
            value: Any = "" if type_name == "VISIBLE_STRING" else [] if type_name == "OCTET_STRING" else 0
            self.warn(f"[{section}] DefaultValue -> {path}: unavailable; using default {value!r}")
            return {"value": value}
        if type_name == "VISIBLE_STRING":
            value = raw[1:-1] if raw.startswith('"') and raw.endswith('"') else raw
            if any(not 0x20 <= ord(c) <= 0x7E for c in value):
                self.warn(f"[{section}] DefaultValue -> {path}: not printable ASCII; supply {USER_INPUT}")
                return {"value": USER_INPUT}
            return {"value": value}
        if type_name == "OCTET_STRING":
            compact = re.sub(r"\s+", "", raw)
            if compact.lower().startswith("0x"):
                compact = compact[2:]
            if len(compact) % 2 or not re.fullmatch(r"[0-9a-fA-F]*", compact):
                raise ConversionError(f"[{section}] invalid hexadecimal OCTET_STRING default")
            return {"value": list(bytes.fromhex(compact))}
        if type_name.startswith("REAL"):
            try:
                value = float(raw)
                if not math.isfinite(value):
                    raise ValueError("non-finite")
                if type_name == "REAL32":
                    struct.pack("<f", value)
            except (ValueError, OverflowError):
                raise ConversionError(f"[{section}] invalid {type_name} default {raw!r}") from None
            return {"value": value}
        if bits is None:
            raise ConversionError(f"[{section}] unknown numeric width")
        try:
            value, relative = integer_expression(raw)
        except ConversionError:
            if not re.search(r"[$+*/()%^-]", raw):
                raise ConversionError(f"[{section}] invalid {type_name} default {raw!r}") from None
            self.warn(f"[{section}] DefaultValue -> {path}: unsupported expression {raw!r}; supply {USER_INPUT}")
            return {"value": USER_INPUT}
        signed = type_name.startswith("INTEGER")
        if signed and not relative and raw.lower().startswith("0x") and (1 << (bits - 1)) <= value < (1 << bits):
            value -= 1 << bits
        low, high = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
        if type_name == "BOOLEAN":
            high = 1
        if not low <= value <= high or (relative and type_name == "BOOLEAN"):
            raise ConversionError(f"[{section}] default {raw!r} cannot fit {type_name}")
        return {"value": value, **({"value_offset": "NODE_ID"} if relative else {})}

    def entry(self, index: int, sub: int, section: str, path: str, count: int | None = None) -> dict[str, Any]:
        raw_type = self.take(section, "DataType")
        data_type = integer(raw_type) if raw_type else -1
        type_info = TYPES.get(data_type)
        if type_info is None:
            self.warn(f"[{section}] DataType -> {path}/type: unsupported or missing type {raw_type!r}; supply {USER_INPUT}")
            entry: dict[str, Any] = {"type": USER_INPUT, "value": USER_INPUT}
            width = None
        else:
            type_name, width = type_info
            entry = {"type": type_name, **self.default(section, type_name, width, path + "/value")}
        access_raw = self.take(section, "AccessType")
        access = (access_raw or "").lower()
        if access not in {"ro", "wo", "rw", "const", "rwr", "rww"}:
            entry["access"] = self.missing(section, "supported AccessType", path + "/access")
        else:
            entry["access"] = {"const": "ro", "rwr": "rw", "rww": "rw"}.get(access, access)
        if type_info and width is None:
            minimum = minimum_string_capacity(entry["type"], entry["value"])
            if minimum > 4096:
                raise ConversionError(f"[{section}] string default exceeds the 4096-byte storage limit")
            if access in {"ro", "const"} and entry["value"] != USER_INPUT:
                entry["size"] = minimum
                self.warn(f"[{section}] index=0x{index:04X}, subindex=0x{sub:02X}: read-only string capacity "
                          f"inferred as {minimum} bytes; local writes cannot exceed this fixed capacity")
            else:
                entry["size"] = self.missing(section, "string storage capacity", path + "/size")
        raw_pdo = self.take(section, "PDOMapping")
        if raw_pdo is None:
            entry["pdo"] = self.missing(section, "PDOMapping", path + "/pdo")
        else:
            flag = integer(raw_pdo)
            if flag not in (0, 1):
                raise ConversionError(f"[{section}] PDOMapping must be 0 or 1")
            direction = {"ro": "tx", "const": "tx", "wo": "rx", "rwr": "tx", "rww": "rx", "rw": "both"}
            entry["pdo"] = direction.get(access, USER_INPUT) if flag else "none"
            if flag and type_info and width is None:
                raise ConversionError(f"[{section}] string PDO mapping is unsupported")
        if count is not None:
            if entry.get("type") != "UNSIGNED8" or entry.get("value") != count or entry.get("access") != "ro":
                self.warn(f"[{section}] -> {path}: subindex 0 normalized to read-only UNSIGNED8 highest subindex {count}")
            entry = {"type": "UNSIGNED8", "value": count, "access": "ro"}
            width = 8
        else:
            entry["local_write"] = True
        self.entries[index, sub] = entry
        self.widths[index, sub] = width
        return entry

    def applications(self) -> list[dict[str, Any]]:
        objects = []
        for section in sorted(self.sections):
            if not re.fullmatch(r"[0-9a-f]{4}", section):
                continue
            index = int(section, 16)
            if not 0x2000 <= index <= 0xBFFF:
                continue
            path = f"/objects/{len(objects)}"
            raw_kind = self.take(section, "ObjectType")
            if raw_kind is None:
                self.warn(f"[{section}] ObjectType absent; using EDS default VAR")
            kind = integer(raw_kind) if raw_kind is not None else 7
            obj: dict[str, Any] = {"index": f"0x{index:04X}"}
            if self.take(section, "CompactSubObj") is not None:
                raise ConversionError(f"[{section}] compact subobjects require expansion; conversion not supported")
            if kind == 7:
                obj.update(self.entry(index, 0, section, path))
            elif kind in (8, 9):
                subs = sorted((int(name.split("sub")[1], 16), name) for name in self.sections
                              if re.fullmatch(section + r"sub[0-9a-f]{1,2}", name))
                if not subs or subs[0][0] != 0 or subs[-1][0] > 254:
                    raise ConversionError(f"[{section}] requires explicit subentries including 0, with subindices <=254")
                if len({sub for sub, _ in subs}) != len(subs):
                    raise ConversionError(f"[{section}] duplicate numeric subindex")
                raw_count = self.take(section, "SubNumber")
                if raw_count is not None and integer(raw_count) != len(subs):
                    self.warn(f"[{section}] SubNumber differs from explicit subentry count; using explicit sections")
                obj["kind"] = "record"
                obj["members"] = [
                    {"subindex": sub, **self.entry(index, sub, name, f"{path}/members/{pos}",
                                                  subs[-1][0] if sub == 0 else None)}
                    for pos, (sub, name) in enumerate(subs)
                ]
                if kind == 8:
                    self.warn(f"[{section}] ARRAY emitted as RECORD to preserve explicit per-subentry metadata and indices")
            else:
                raise ConversionError(f"[{section}] unsupported ObjectType {raw_kind!r}")
            objects.append(obj)
        if len(objects) > 4096:
            raise ConversionError("more than 4096 application objects")
        return objects

    def pdos(self, transmit: bool) -> list[dict[str, Any]]:
        comm_base, map_base = (0x1800, 0x1A00) if transmit else (0x1400, 0x1600)
        key = "tpdos" if transmit else "rpdos"
        slots = [i for i in range(512) if f"{comm_base+i:04x}" in self.sections or f"{map_base+i:04x}" in self.sections]
        if slots and max(slots) >= 8:
            raise ConversionError(f"{key}: runtime supports at most eight PDO slots")
        result = []
        for slot in range(max(slots, default=-1) + 1):
            comm, mapping = f"{comm_base+slot:04x}", f"{map_base+slot:04x}"
            path = f"/{key}/{slot}"
            cob = self.number(comm + "sub1", "DefaultValue", path + "/cob_id", offset=True)
            pdo: dict[str, Any] = {"cob_id": cob["value"]}
            if "value_offset" in cob:
                pdo["cob_id_offset"] = cob["value_offset"]
            if isinstance(cob["value"], int):
                raw_id = cob["value"]
                if raw_id & 0x3FFFF800:
                    raise ConversionError(f"[{comm}sub1] unsupported extended or reserved COB-ID bits")
                pdo["cob_id"] = raw_id & 0x7FF
                pdo["enabled"] = not bool(raw_id & 0x80000000)
                if transmit:
                    self.warn(f"[{comm}sub1] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs")
            else:
                pdo["enabled"] = self.missing(comm + "sub1", "PDO enable bit", path + "/enabled")
            pdo["transmission_type"] = self.number(comm + "sub2", "DefaultValue",
                                                    path + "/transmission_type", 255, default=0)["value"]
            if isinstance(pdo["transmission_type"], int) and 241 <= pdo["transmission_type"] <= 253:
                raise ConversionError(f"[{comm}sub2] unsupported PDO transmission type")
            for sub, field in ((3, "inhibit_100us"), (5, "event_ms")) if transmit else ((5, "event_ms"),):
                pdo[field] = self.number(f"{comm}sub{sub}", "DefaultValue", path + "/" + field, 65535, default=0)["value"]
            count = self.number(mapping + "sub0", "DefaultValue", path + "/mapping", 8)["value"]
            if not isinstance(count, int):
                pdo["mapping"] = USER_INPUT
            else:
                mapped = []
                total = 0
                incomplete = False
                for sub in range(1, count + 1):
                    word = self.number(f"{mapping}sub{sub:x}", "DefaultValue", f"{path}/mapping/{sub-1}")["value"]
                    if not isinstance(word, int):
                        incomplete = True
                        continue
                    index, subindex, bits = word >> 16, (word >> 8) & 0xFF, word & 0xFF
                    target = (index, subindex)
                    if target not in self.entries:
                        raise ConversionError(f"[{mapping}sub{sub:x}] mapped target 0x{index:04X}:{subindex:02X} was not imported")
                    if not bits or (self.widths[target] is not None and self.widths[target] != bits):
                        raise ConversionError(f"[{mapping}sub{sub:x}] mapped bit length {bits} differs from target type width")
                    if self.widths[target] is None:
                        self.warn(f"[{mapping}sub{sub:x}] mapped target type unresolved; verify {bits}-bit width after supplying type")
                    direction = self.entries[target].get("pdo")
                    if direction == USER_INPUT:
                        self.warn(f"[{mapping}sub{sub:x}] mapped target PDO permission unresolved; verify direction after supplying it")
                    elif direction not in (("tx", "both") if transmit else ("rx", "both")):
                        raise ConversionError(f"[{mapping}sub{sub:x}] target does not permit this PDO direction")
                    total += bits
                    mapped.append({"index": f"0x{index:04X}", "subindex": subindex})
                if total > 64:
                    raise ConversionError(f"[{mapping}] payload {total} bits exceeds 64")
                pdo["mapping"] = USER_INPUT if incomplete else mapped
                if incomplete:
                    self.warn(f"[{mapping}] -> {path}/mapping: active mapping words missing; supply the complete mapping list")
                if not count and pdo["enabled"] is True:
                    self.warn(f"[{mapping}] empty mapping: PDO emitted disabled despite enabled EDS COB-ID")
                    pdo["enabled"] = False
            self.warn(f"[{comm}]/[{mapping}] runtime PDO communication/mapping records are fixed and SDO-read-only")
            result.append(pdo)
        return result

    def convert(self) -> dict[str, Any]:
        root: dict[str, Any] = {
            "schema_version": 1,
            "node_id": self.missing("runtime", "node ID", "/node_id"),
            "bitrate_kbps": self.missing("DeviceInfo", "selected bitrate (EDS lists supported rates)", "/bitrate_kbps"),
            "worker_period_us": self.missing("runtime", "worker period", "/worker_period_us"),
        }
        root["identity"] = {}
        for sub, name, fallback in ((1, "vendor_id", "VendorNumber"), (2, "product_code", "ProductNumber"),
                                     (3, "revision_number", "RevisionNumber"), (4, "serial_number", None)):
            section = f"1018sub{sub}"
            default = 0 if name in ("product_code", "revision_number") else None
            if "defaultvalue" in self.sections.get(section, {}) or fallback is None:
                value = self.number(section, "DefaultValue", "/identity/" + name, default=default)["value"]
            else:
                value = self.number("DeviceInfo", fallback, "/identity/" + name, default=default)["value"]
            if name in ("vendor_id", "serial_number") and value == 0:
                value = self.missing(section, f"nonzero {name} required by runtime", "/identity/" + name)
            root["identity"][name] = value
        for section, field, maximum in (("1000", "device_type", 0xFFFFFFFF), ("1017", "heartbeat_ms", 65535)):
            root[field] = self.number(section, "DefaultValue", "/" + field, maximum, default=0)["value"]
        service_fields = (
            ("node_guarding", (("100c", "guard_time_ms", 65535), ("100d", "life_time_factor", 255))),
            ("sync", (("1005", "cob_id", 0x7FF), ("1006", "period_us", 0xFFFFFFFF),
                      ("1007", "window_us", 0xFFFFFFFF), ("1019", "counter_overflow", 240))),
            ("emcy", (("1014", "cob_id", 0x800007FF), ("1015", "inhibit_100us", 65535))),
        )
        for group, fields in service_fields:
            if not any(section in self.sections for section, _, _ in fields):
                self.warn(f"/{group}: absent from EDS; omitted, so documented runtime defaults apply")
                continue
            root[group] = {}
            for section, field, maximum in fields:
                value = self.number(section, "DefaultValue", f"/{group}/{field}", maximum,
                                    field == "cob_id", None if field == "cob_id" else 0)
                if group == "emcy" and field == "cob_id" and isinstance(value["value"], int) and value["value"] & 0x7FFFF800:
                    raise ConversionError(f"[{section}] unsupported EMCY COB-ID bits")
                if group == "sync" and field == "counter_overflow" and value["value"] == 1:
                    raise ConversionError(f"[{section}] SYNC counter overflow must be 0 or 2..240")
                root[group][field] = value["value"]
                if "value_offset" in value:
                    root[group][field + "_offset"] = value["value_offset"]
        root["objects"] = self.applications()
        root["rpdos"] = self.pdos(False)
        root["tpdos"] = self.pdos(True)
        translated = {"1000", "1005", "1006", "1007", "100c", "100d", "1014", "1015", "1017", "1018", "1019"}
        for section in self.sections:
            if re.fullmatch(r"[0-9a-f]{4}", section):
                index = int(section, 16)
                if section not in translated and not 0x2000 <= index <= 0xBFFF and not (
                        0x1400 <= index <= 0x1BFF):
                    self.warn(f"[{section}] omitted: outside application range and not a translated communication setting")
        self.report_unused()
        self.warn("EDS describes storage and protocol defaults, not device behavior; CiA 402/vendor behavior still requires a model")
        return root

    def input_guide(self, model: dict[str, Any], model_file: str) -> dict[str, Any]:
        inputs: list[dict[str, Any]] = []

        def describe(path: str, parent: dict[str, Any], key: str) -> dict[str, Any]:
            expected, description, example = INPUT_HINTS.get(
                key, ("value matching the runtime JSON schema", "Supply the unresolved configuration value.", 0))
            hint: dict[str, Any] = {"path": path, "expected": expected, "description": description, "example": example}
            if path.startswith("/objects/"):
                obj = model["objects"][int(path.split("/")[2])]
                index = obj["index"]
                sub = parent.get("subindex", 0)
                section = f"{int(index, 16):04x}" + (f"sub{sub:x}" if obj.get("kind") == "record" else "")
                fields = self.sections.get(section, {})
                hint.update(od_address=f"{index}:{sub:02X}", eds_section=self.names.get(section, section),
                            label=f"index={index}, subindex=0x{sub:02X}, {key}", editor_search=index)
                if "parametername" in fields:
                    hint["parameter_name"] = fields["parametername"]
                if "defaultvalue" in fields:
                    hint["eds_default"] = fields["defaultvalue"]
            if key == "size":
                minimum = minimum_string_capacity(parent["type"], parent["value"])
                hint.update(expected=f"integer {minimum}..4096", example=minimum,
                            description="Fixed storage capacity in bytes. Must fit the initial value and future writes; "
                                        "VISIBLE_STRING also requires one byte for its terminating NUL.")
            elif key == "value":
                type_name = parent.get("type", USER_INPUT)
                if type_name.startswith(("INTEGER", "UNSIGNED")):
                    bits = int(type_name[7:] if type_name.startswith("INTEGER") else type_name[8:])
                    low, high = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if type_name.startswith("INTEGER") else (0, (1 << bits) - 1)
                    hint["expected"] = f"{type_name} integer {low}..{high}"
                else:
                    hint["expected"] = {
                        "BOOLEAN": "boolean or integer 0/1", "REAL32": "finite IEEE-754 32-bit number",
                        "REAL64": "finite IEEE-754 64-bit number", "VISIBLE_STRING": "printable ASCII string",
                        "OCTET_STRING": "array of integers 0..255",
                    }.get(type_name, "value matching the selected OD type")
                hint["description"] = "Resolve the EDS default manually; unsupported expressions are not replaced with zero."
                if type_name == "VISIBLE_STRING":
                    hint["example"] = ""
                elif type_name == "OCTET_STRING":
                    hint["example"] = []
            elif key == "cob_id":
                hint.update(expected="integer 0..2047, subject to CANopen reserved-ID and collision checks",
                            description="An absolute 11-bit CAN ID, or a base when cob_id_offset is NODE_ID.", example=512)
                if path.startswith("/emcy/"):
                    hint["expected"] = "integer 0..2047, optionally OR'd with the disable bit 0x80000000"
                    hint["description"] += " EMCY also permits the disable bit 0x80000000."
            elif key == "bitrate_kbps":
                rates = sorted(int(name[len("baudrate_"):]) for name, value in self.sections.get("deviceinfo", {}).items()
                               if re.fullmatch(r"baudrate_[0-9]+", name) and value == "1")
                if rates:
                    hint["eds_supported_values"] = rates
                    hint["example"] = 125 if 125 in rates else rates[0]
            return hint

        def walk(value: Any, path: str = "") -> None:
            if isinstance(value, dict):
                for key, item in value.items():
                    child = path + "/" + key
                    if item == USER_INPUT:
                        inputs.append(describe(child, value, key))
                    else:
                        walk(item, child)
            elif isinstance(value, list):
                for position, item in enumerate(value):
                    walk(item, path + "/" + str(position))

        walk(model)
        return {
            "model_file": model_file,
            "instructions": "Search the model file by OD index/subindex for application fields; JSON Pointer paths "
                            "are included only as a secondary reference for tools. Replace the whole "
                            "{USER_INPUT} string with a correctly typed JSON value. Examples are illustrative, "
                            "not confirmed device settings. This guide is not loaded by the simulator.",
            "automatic_defaults": {
                "local_write": "true for application values; structural subindex 0 stays read-only and not locally writable",
                "missing_od_values": "0 for numeric application defaults and supported numeric communication settings; "
                                     "empty string for VISIBLE_STRING; empty byte array for OCTET_STRING",
                "missing_transmission_type": "0: synchronous acyclic PDO, not asynchronous/event-driven (254/255)",
                "read_only_string_capacity": "inferred from the known initial value, including NUL for VISIBLE_STRING; "
                                             "local_write remains true, but longer host writes require increasing size",
                "exceptions": "Node ID, bitrate, worker period, required nonzero identity, string capacity, unknown "
                              "types, COB-IDs, mapping structure and unsupported expressions still require input",
            },
            "inputs": inputs,
            "warnings": self.warnings,
        }

    def report_unused(self) -> None:
        metadata: Counter[str] = Counter()
        ignored = {"parametername", "lowlimit", "highlimit", "objflags", "objecttype", "subnumber",
                   "datatype", "accesstype", "pdomapping", "defaultvalue"}
        for section, fields in self.sections.items():
            for key, raw in fields.items():
                if (section, key) in self.used:
                    continue
                if re.fullmatch(r"[0-9a-f]{4}(sub[0-9a-f]{1,2})?", section) and key in ignored:
                    metadata[key] += 1
                elif section in {"fileinfo", "deviceinfo", "comments", "dummyusage",
                                  "mandatoryobjects", "optionalobjects", "manufacturerobjects"}:
                    metadata[section] += 1
                else:
                    self.warn(f"[{self.names[section]}] {key}={raw!r}: not represented in JSON")
        if metadata:
            self.warn("EDS metadata/unused definitions not copied (field counts): "
                      + ", ".join(f"{key}={count}" for key, count in sorted(metadata.items()))
                      + "; names/limits/flags and supported-feature lists are not runtime validation rules")


def read_eds(path: Path, encoding: str = "utf-8-sig") -> Converter:
    parser = configparser.ConfigParser(interpolation=None, delimiters=("=",), strict=True)
    with path.open(encoding=encoding) as source:
        parser.read_file(source)
    return Converter(parser)


def convert_eds(path: Path, encoding: str = "utf-8-sig") -> tuple[dict[str, Any], list[str]]:
    converter = read_eds(path, encoding)
    return converter.convert(), converter.warnings


def guide_markdown(guide: dict[str, Any]) -> str:
    def text(value: Any) -> str:
        return re.sub(r"([\\`*_{}\[\]<>()#|])", r"\\\1", str(value)).replace("\r", "").replace("\n", "<br>")

    def example(value: Any) -> str:
        return text(json.dumps(value, ensure_ascii=True, allow_nan=False))

    model_file = guide["model_file"]
    lines = [
        "# EDS conversion input guide", "",
        f"Model: [{text(model_file)}](./{quote(model_file, safe='')})", "",
        text(guide["instructions"]), "",
        f"## Required inputs ({len(guide['inputs'])})", "",
    ]
    if guide["inputs"]:
        lines.extend(["| Field / editor search | Expected value | JSON example |",
                      "| --- | --- | --- |"])
        for hint in guide["inputs"]:
            label = hint.get("label", hint["path"])
            lines.append(f"| `{label}` | {text(hint['expected'])} | {example(hint['example'])} |")
        lines.extend(["", "## Input details", ""])
        for hint in guide["inputs"]:
            label = hint.get("label", hint["path"])
            lines.extend([f"### `{label}`", "", text(hint["description"]), ""])
            if "editor_search" in hint:
                lines.extend([f"- Search in the editor for: `{hint['editor_search']}`",
                              f"- JSON Pointer (secondary reference for tools): `{hint['path']}`", ""])
            if "eds_supported_values" in hint:
                lines.extend(["EDS-supported values: " + ", ".join(example(v) for v in hint["eds_supported_values"]) + ".", ""])
            if "od_address" in hint:
                lines.extend([f"- OD address: {text(hint['od_address'])}"])
            if "parameter_name" in hint:
                lines.append(f"- Parameter name: {text(hint['parameter_name'])}")
            if "eds_section" in hint:
                lines.append(f"- EDS section: {text(hint['eds_section'])}")
            if "eds_default" in hint:
                lines.append(f"- Original EDS default: {example(hint['eds_default'])}")
            if "od_address" in hint:
                lines.append("")
    else:
        lines.extend(["No unresolved input placeholders remain.", ""])
    lines.extend(["## Automatic defaults", ""])
    for name, description in guide["automatic_defaults"].items():
        lines.append(f"- **{text(name)}:** {text(description)}")
    lines.extend(["", f"## Conversion warnings ({len(guide['warnings'])})", ""])
    lines.extend(f"- {text(warning)}" for warning in guide["warnings"])
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("eds", type=Path)
    parser.add_argument("output", type=Path, nargs="?", help="Output JSON path (default: EDS path with .json extension)")
    parser.add_argument("--encoding", default="utf-8-sig", help="EDS text encoding (default: utf-8-sig)")
    parser.add_argument("--warnings-as-errors", action="store_true", help="Do not write output if conversion has warnings")
    args = parser.parse_args(argv)
    if args.output is None:
        args.output = args.eds.with_suffix(".json")
    guide_path = args.output.with_suffix(".inputs.md")
    try:
        if args.eds.resolve() in (args.output.resolve(), guide_path.resolve()):
            raise ConversionError("output and input guide must not overwrite the source EDS")
        converter = read_eds(args.eds, args.encoding)
        model = converter.convert()
        warnings = converter.warnings
        for warning in warnings:
            print(f"WARNING: {warning}", file=sys.stderr)
        if warnings and args.warnings_as_errors:
            print("ERROR: warnings-as-errors: output not written", file=sys.stderr)
            return 1
        text = json.dumps(model, indent=2, ensure_ascii=True, allow_nan=False) + "\n"
        guide = converter.input_guide(model, args.output.name)
        guide_text = guide_markdown(guide)
        if len(text.encode("utf-8")) > 4 * 1024 * 1024:
            raise ConversionError("generated JSON exceeds the runtime's 4 MiB limit")
        args.output.write_text(text, encoding="utf-8")
        guide_path.write_text(guide_text, encoding="utf-8")
    except (ConversionError, configparser.Error, OSError, UnicodeError, LookupError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {args.output} ({len(model['objects'])} application objects, {len(warnings)} warnings). "
          f"Replace all {USER_INPUT} placeholders before initialization.")
    print(f"Input guide: {guide_path} ({len(guide['inputs'])} fields to complete).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
