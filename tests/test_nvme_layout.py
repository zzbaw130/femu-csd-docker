"""布局/身份与破坏性操作边界；仅对临时普通文件运行真实 sfdisk。"""

from contextlib import ExitStack, redirect_stdout
from copy import deepcopy
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nvme_layout", ROOT / "scripts/guest/nvme-layout.py")
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)
DEVICE = "/dev/nvme0n1"
SIZE = 16 * 1024**3


def table_for(lba_bytes=512):
    plan = layout.plan_layout(SIZE, lba_bytes)
    return {
        "label": "gpt", "unit": "sectors", "device": DEVICE,
        "sectorsize": lba_bytes, "id": "1299f00a-e714-43c6-84d8-6228e4d9793a",
        "partitions": [
            {"node": DEVICE + "p1", "name": layout.DATA_LABEL, "type": layout.DATA_TYPE,
             "uuid": "5507f260-505e-4a88-a18d-124563636601",
             "start": plan["data"]["start_lba"], "size": plan["data"]["num_blocks"]},
            {"node": DEVICE + "p2", "name": layout.SPILL_LABEL, "type": layout.SPILL_TYPE,
             "uuid": "5507f260-505e-4a88-a18d-124563636602",
             "start": plan["spill"]["start_lba"], "size": plan["spill"]["num_blocks"]},
        ],
    }


class DiskModel:
    def __init__(self, *, blank=False, legacy=False, mounted=False, spill_mounted=False,
                 spill_signature=False, target_occupied=False, model=layout.FEMU_MODEL):
        self.blank, self.legacy, self.mounted = blank, legacy, mounted
        self.spill_mounted, self.spill_signature = spill_mounted, spill_signature
        self.target_occupied = target_occupied
        self.model = model
        self.calls = []

    def __call__(self, *args, input=None, check=True):
        self.calls.append((args, input))
        command = args[0]
        output, code = "", 0
        if command == "blockdev":
            output = str(SIZE if args[1] == "--getsize64" else 512)
        elif command == "wipefs":
            signatures = []
            if args[-1] == DEVICE and not self.blank:
                signatures = [{"type": "ext4" if self.legacy else "gpt"}]
            elif args[-1] == DEVICE + "p2" and self.spill_signature:
                signatures = [{"type": "ext4"}]
            output = json.dumps({"signatures": signatures})
        elif command == "lsblk":
            if "MODEL" in args:
                output = self.model
            else:
                node = args[-1]
                mounted = self.mounted if node in (DEVICE, DEVICE + "p1") else self.spill_mounted
                output = json.dumps({"blockdevices": [{
                    "name": node, "type": "disk", "mountpoints": ["/data"] if mounted else [None]
                }]})
        elif command == "sfdisk" and "--json" in args:
            if self.legacy:
                code = 1
            else:
                output = json.dumps({"partitiontable": table_for()})
        elif command == "sfdisk":
            self.legacy = False
        elif command == "blkid":
            output = "ext4"
        elif command == "findmnt":
            output, code = ("259:1", 0) if self.mounted else ("", 1)
            if self.target_occupied:
                output, code = "8:1", 0
        return SimpleNamespace(stdout=output, stderr="", returncode=code)

    def mutations(self):
        return [args for args, _ in self.calls
                if args[0] in ("mount", "mkfs.ext4")
                or (args[0] == "sfdisk" and "--json" not in args)]


class LayoutTests(unittest.TestCase):
    def test_geometry_512_and_4096(self):
        for sector in (512, 4096):
            for size in (128 * layout.MIB, SIZE, SIZE + 17 * sector):
                with self.subTest(sector=sector, size=size):
                    plan = layout.plan_layout(size, sector)
                    data, spill = plan["data"], plan["spill"]
                    self.assertEqual((data["start_lba"] * sector) % layout.MIB, 0)
                    self.assertEqual((spill["start_lba"] * sector) % layout.MIB, 0)
                    self.assertEqual(data["start_lba"] + data["num_blocks"], spill["start_lba"])
                    end = (spill["start_lba"] + spill["num_blocks"]) * sector
                    self.assertLess(end, size)
                    usable = (data["num_blocks"] + spill["num_blocks"]) * sector
                    self.assertLess(abs(data["num_blocks"] * sector - usable * .8), layout.MIB)

    def test_reject_bad_geometry(self):
        for size, sector in ((layout.MIB, 512), (SIZE, 1024), (SIZE + 1, 4096)):
            with self.subTest(size=size, sector=sector), self.assertRaises(layout.LayoutError):
                layout.plan_layout(size, sector)

    def test_roles_follow_labels_not_partition_numbers(self):
        table = table_for()
        table["partitions"].reverse()
        table["partitions"][0]["node"] = DEVICE + "p1"  # spill
        table["partitions"][1]["node"] = DEVICE + "p2"  # data
        roles = layout.validate_layout(table, DEVICE, 512, layout.plan_layout(SIZE, 512))
        self.assertEqual(roles["spill"]["node"], DEVICE + "p1")
        self.assertEqual(roles["data"]["node"], DEVICE + "p2")

    def test_reject_mismatched_identity_and_ranges(self):
        mutations = [
            lambda t: t.update(sectorsize=4096),
            lambda t: t.update(device="/dev/nvme1n1"),
            lambda t: t.update(label="dos"),
            lambda t: t["partitions"][1].update(name=layout.DATA_LABEL),
            lambda t: t["partitions"][1].update(type=layout.DATA_TYPE),
            lambda t: t["partitions"][1].update(start=2048),
            lambda t: t["partitions"][1].update(uuid=t["partitions"][0]["uuid"]),
            lambda t: t["partitions"].pop(),
        ]
        for mutate in mutations:
            table = deepcopy(table_for())
            mutate(table)
            with self.subTest(table=table), self.assertRaises(layout.LayoutError):
                layout.validate_layout(table, DEVICE, 512, layout.plan_layout(SIZE, 512))

    def test_manifest_uses_native_lba_and_atomic_replacement(self):
        table = table_for(4096)
        roles = layout.validate_layout(table, DEVICE, 4096, layout.plan_layout(SIZE, 4096))
        manifest = layout.make_manifest(DEVICE, SIZE, 4096, table, roles, "/data")
        self.assertEqual(manifest["spill"]["start_lba"], table["partitions"][1]["start"])
        self.assertEqual(manifest["spill"]["size_bytes"], manifest["spill"]["num_blocks"] * 4096)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config/spill.json"
            layout.write_manifest(path, {"old": True})
            layout.write_manifest(path, manifest)
            self.assertEqual(json.loads(path.read_text()), manifest)
            self.assertEqual(path.stat().st_mode & 0o777, 0o644)
            self.assertEqual([p.name for p in path.parent.iterdir()], ["spill.json"])

    def prepare(self, model, directory, *, repartition=False):
        args = SimpleNamespace(device=DEVICE, mountpoint=str(Path(directory) / "data"),
                               config=str(Path(directory) / "config/spill.json"),
                               repartition=repartition)
        actual_stat = os.stat

        def fake_stat(path, *args, **kwargs):
            if str(path) == DEVICE + "p1":
                return SimpleNamespace(st_rdev=os.makedev(259, 1))
            return actual_stat(path, *args, **kwargs)

        with ExitStack() as stack:
            stack.enter_context(patch.object(layout, "run", side_effect=model))
            stack.enter_context(patch.object(layout.os, "geteuid", return_value=0))
            stack.enter_context(patch.object(layout.os, "chown"))
            stack.enter_context(patch.object(layout.os, "stat", side_effect=fake_stat))
            stack.enter_context(patch.object(layout.shutil, "which", return_value="/mock/bin"))
            stack.enter_context(patch.object(layout.pwd, "getpwnam", return_value=SimpleNamespace(pw_uid=1000)))
            stack.enter_context(patch.object(layout.grp, "getgrnam", return_value=SimpleNamespace(gr_gid=1000)))
            stack.enter_context(patch.object(layout.Path, "is_block_device", return_value=True))
            stack.enter_context(patch.object(layout, "wait_partition"))
            stack.enter_context(redirect_stdout(io.StringIO()))
            layout.prepare(args)
        return json.loads(Path(args.config).read_text())

    def test_blank_disk_creates_two_partitions_formats_only_data(self):
        model = DiskModel(blank=True)
        with tempfile.TemporaryDirectory() as directory:
            manifest = self.prepare(model, directory)
        self.assertEqual(manifest["spill"]["partition"], DEVICE + "p2")
        mkfs = [args for args, _ in model.calls if args[0] == "mkfs.ext4"]
        self.assertEqual(len(mkfs), 1)
        self.assertEqual(mkfs[0][-1], DEVICE + "p1")
        partition_calls = [text for args, text in model.calls if args[0] == "sfdisk" and text]
        self.assertEqual(len(partition_calls), 1)
        self.assertIn('name="femu-spill"', partition_calls[0])

    def test_valid_mounted_layout_is_reused(self):
        model = DiskModel(mounted=True)
        with tempfile.TemporaryDirectory() as directory:
            self.prepare(model, directory)
        self.assertEqual(model.mutations(), [])

    def test_legacy_whole_disk_is_not_overwritten(self):
        model = DiskModel(legacy=True)
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(layout.LayoutError, "--repartition"):
            self.prepare(model, directory)
        self.assertEqual(model.mutations(), [])

    def test_explicit_repartition_rebuilds_unmounted_legacy_disk(self):
        model = DiskModel(legacy=True)
        with tempfile.TemporaryDirectory() as directory:
            self.prepare(model, directory, repartition=True)
        self.assertTrue(any(args[0] == "mkfs.ext4" for args in model.mutations()))

    def test_repartition_refuses_mounted_disk(self):
        model = DiskModel(legacy=True, mounted=True)
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(layout.LayoutError):
            self.prepare(model, directory, repartition=True)
        self.assertEqual(model.mutations(), [])

    def test_blank_disk_is_not_formatted_when_mountpoint_is_occupied(self):
        model = DiskModel(blank=True, target_occupied=True)
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(layout.LayoutError):
            self.prepare(model, directory)
        self.assertEqual(model.mutations(), [])

    def test_spill_must_be_unmounted_and_unformatted(self):
        for model in (DiskModel(spill_mounted=True), DiskModel(spill_signature=True)):
            with self.subTest(model=model), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(layout.LayoutError):
                    self.prepare(model, directory)
                self.assertEqual(model.mutations(), [])

    def test_real_ssd_model_is_rejected_before_any_mutation(self):
        model = DiskModel(blank=True, model="Production SSD")
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(layout.LayoutError):
            self.prepare(model, directory)
        self.assertEqual(model.mutations(), [])

    @unittest.skipUnless(shutil.which("sfdisk"), "需要 util-linux sfdisk")
    def test_real_sfdisk_on_regular_image(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "disk.img"
            with image.open("wb") as stream:
                stream.truncate(128 * layout.MIB)
            plan = layout.plan_layout(image.stat().st_size, 512)
            subprocess.run(
                ["sfdisk", "--no-reread", "--no-tell-kernel", str(image)],
                input=layout.partition_input(plan), text=True, check=True, capture_output=True,
            )
            table = json.loads(subprocess.check_output(["sfdisk", "--json", str(image)]))["partitiontable"]
            self.assertEqual(table["partitions"][0]["name"], layout.DATA_LABEL)
            self.assertEqual(table["partitions"][1]["name"], layout.SPILL_LABEL)
            self.assertEqual(table["partitions"][1]["type"].upper(), layout.SPILL_TYPE)
            # 普通 image 文件的分区节点命名不同；验证其他实际生成的 GPT 属性。
            table["device"] = DEVICE
            for number, part in enumerate(table["partitions"], 1):
                part["node"] = f"{DEVICE}p{number}"
            layout.validate_layout(table, DEVICE, 512, plan)


if __name__ == "__main__":
    unittest.main()
