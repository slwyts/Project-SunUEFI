"""Real dtc/fdtoverlay tests on synthetic FDTs; no device access."""

import copy
import importlib.util
import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("piano_composer", ROOT / "tools/compose_piano_dtb.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)
DTC = ROOT / "build/kernel-topics/piano-panel/scripts/dtc/dtc"
OVERLAY = DTC.with_name("fdtoverlay")
CELL = lambda value: struct.pack(">I", value)


class FdtContractTests(unittest.TestCase):
    def base(self):
        return {"tree": {"/": {"#address-cells": CELL(1), "#size-cells": CELL(0)},
                         "/gpio@0": {"reg": CELL(0), "phandle": CELL(1), "#gpio-cells": CELL(2),
                                     "gpio-controller": b""}}, "reservations": [(0x12340000, 4096)], "boot_cpu": 0}

    def test_header_bounds_reserve_map_and_duplicate_phandles(self):
        blob = tool.write_fdt(self.base())
        self.assertEqual(self.base()["tree"], tool.read_fdt(blob)["tree"])
        for offset, value in ((0, 0), (4, len(blob) - 1), (8, 0xfffffff0),
                              (12, len(blob)), (16, 41), (32, 0xffffffff), (36, 0xffffffff)):
            corrupt = bytearray(blob)
            struct.pack_into(">I", corrupt, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                tool.read_fdt(corrupt)
        duplicate = self.base()
        duplicate["tree"]["/other"] = {"phandle": CELL(1)}
        with self.assertRaisesRegex(ValueError, "duplicate phandle"):
            tool.read_fdt(tool.write_fdt(duplicate))

    def test_unresolved_gpio_and_endpoint_reciprocity_are_rejected(self):
        base = self.base()
        base["tree"]["/device"] = {"reset-gpios": CELL(9) + CELL(2) + CELL(0)}
        parsed = tool.read_fdt(tool.write_fdt(base))
        with self.assertRaisesRegex(ValueError, "unresolved phandle"):
            tool.verify_references(parsed, ["/device"])
        base["tree"]["/device"] = {"reset-gpios": CELL(1) + CELL(2)}
        with self.assertRaisesRegex(ValueError, "cell mismatch"):
            tool.verify_references(tool.read_fdt(tool.write_fdt(base)), ["/device"])
        base["tree"]["/endpoint-a"] = {"phandle": CELL(2), "remote-endpoint": CELL(3)}
        base["tree"]["/endpoint-b"] = {"phandle": CELL(3)}
        with self.assertRaisesRegex(ValueError, "non-reciprocal"):
            tool.verify_references(tool.read_fdt(tool.write_fdt(base)), ["/endpoint-a"])


@unittest.skipUnless(DTC.exists() and OVERLAY.exists(), "local real dtc/fdtoverlay required")
class CompositionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.kernel = self.root / "kernel"
        self.kernel.mkdir()
        self.source = self.kernel / "test.dtso"
        self.source.write_text('''/dts-v1/;
/plugin/;
/ {
    fragment@0 {
        target-path = "/";
        __overlay__ {
            test_node: audit-device@1 {
                compatible = "piano,test-topology";
                reg = <1>;
                status = "disabled";
                reset-gpios = <&stock_gpio 5 0>;
                ep_a: endpoint-a { remote-endpoint = <&ep_b>; };
                ep_b: endpoint-b { remote-endpoint = <&ep_a>; };
            };
        };
    };
};
''')
        header = self.kernel / "include/dt-bindings/gpio/gpio.h"
        header.parent.mkdir(parents=True)
        header.write_text("#define GPIO_ACTIVE_HIGH 0\n")
        def git(*args):
            return subprocess.check_output(["git", "-C", str(self.kernel), *args], text=True).strip()
        git("init", "-q")
        git("config", "user.name", "DTB fixture")
        git("config", "user.email", "dtb@example.invalid")
        git("add", ".")
        git("commit", "-q", "-m", "synthetic overlay fixture")
        self.base = self.root / "base.dtb"
        self.base.write_bytes(tool.write_fdt(FdtContractTests().base()))
        self.recipe = {"base_sha256": tool.sha(self.base.read_bytes()), "overlay_source": "test.dtso",
                       "overlay_commit": git("rev-parse", "HEAD"), "overlay_sha256": tool.sha(self.source.read_bytes()),
                       "gpio_header_sha256": tool.sha(header.read_bytes()), "base_aliases": {"stock_gpio": "/gpio@0"},
                       "new_nodes": ["/audit-device@1", "/audit-device@1/endpoint-a", "/audit-device@1/endpoint-b"],
                       "reference_nodes": ["/audit-device@1", "/audit-device@1/endpoint-a", "/audit-device@1/endpoint-b"],
                       "disabled_nodes": ["/audit-device@1"], "allowed_property_changes": [],
                       "overlay_symbols": ["test_node", "ep_a", "ep_b"], "unresolved_integration": [{"status": "unknown"}],
                       "reference_sources": []}
        self.review = self.root / "review.json"
        self.output = self.root / "output"

    def compose(self):
        self.review.write_text(json.dumps(self.recipe))
        return tool.compose(self.base, self.review, self.kernel, self.output, "csot", DTC, OVERLAY)

    def test_real_overlay_resolves_references_without_replaying_base(self):
        result = self.compose()
        final = tool.read_fdt((self.output / "piano-display-candidate.dtb").read_bytes())
        self.assertFalse(result["hardware_verified"])
        self.assertFalse(result["enable_allowed"])
        self.assertEqual(3, len(result["delta"]["added_nodes"]))
        self.assertEqual([], result["delta"]["property_changes"])
        self.assertEqual(FdtContractTests().base()["tree"]["/gpio@0"], final["tree"]["/gpio@0"])
        self.assertEqual([(0x12340000, 4096)], final["reservations"])
        self.assertGreater(int.from_bytes(final["tree"]["/audit-device@1"]["phandle"], "big"), 1)
        self.assertEqual(result["final"]["sha256"], self.compose()["final"]["sha256"])

    def test_input_sha_and_canonical_commit_drift_are_rejected(self):
        for field in ("base_sha256", "overlay_sha256", "gpio_header_sha256"):
            original = self.recipe[field]
            self.recipe[field] = "0" * 64
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.compose()
            self.recipe[field] = original
        self.source.write_text(self.source.read_text() + "// uncommitted drift\n")
        self.recipe["overlay_sha256"] = tool.sha(self.source.read_bytes())
        with self.assertRaisesRegex(ValueError, "canonical overlay"):
            self.compose()
        self.assertFalse(self.output.exists())

    def test_overlay_replay_alias_collision_and_unreviewed_changes_fail(self):
        self.compose()
        self.base.write_bytes((self.output / "piano-display-candidate.dtb").read_bytes())
        self.recipe["base_sha256"] = tool.sha(self.base.read_bytes())
        with self.assertRaisesRegex(ValueError, "collision"):
            self.compose()
        original = FdtContractTests().base()
        original["tree"]["/__symbols__"] = {"stock_gpio": b"/wrong-path\0"}
        self.base.write_bytes(tool.write_fdt(original))
        self.recipe["base_sha256"] = tool.sha(self.base.read_bytes())
        with self.assertRaisesRegex(ValueError, "alias collision"):
            self.compose()
        before = FdtContractTests().base()["tree"]
        after = copy.deepcopy(before)
        after["/"]["bootargs"] = b"unsafe\0"
        with self.assertRaises(ValueError):
            tool.check_delta(before, after, {"new_nodes": [], "allowed_property_changes": [],
                                            "disabled_nodes": []})


if __name__ == "__main__":
    unittest.main()
