#!/usr/bin/env python3
"""The frozen SIMT model shared by the docs, the gates and the simulator.

Chapter 11 makes one JSON file the single source of truth for the numbers that
every later chapter would otherwise re-state: warp size, warps per block,
special-register functs, active-mask width and address spaces.

Keep this module dependency-free: chapter 12's simulator imports it, and the
chapter-11 gate runs it without the LLVM tree.
"""

import argparse
import json
import os
import sys

MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "scripts", "simt-model.json")


class Model:
    def __init__(self, path=MODEL_PATH):
        with open(path, encoding="utf-8") as f:
            self.data = json.load(f)

    @property
    def warp_size(self):
        return self.data["warp_size"]

    @property
    def warps_per_block(self):
        return self.data["warps_per_block"]

    @property
    def threads_per_block(self):
        return self.data["threads_per_block"]

    @property
    def all_active(self):
        return self.data["active_mask"]["all_active"]

    @property
    def special_funct(self):
        return {r["name"]: r["funct"] for r in self.data["special_registers"]}

    @property
    def launch(self):
        return self.data["launch"]

    @property
    def args_funct(self):
        return self.data["launch"]["args_funct"]

    # --- thread identity -------------------------------------------------
    def laneid(self, tid):
        return tid % self.warp_size

    def warpid(self, tid):
        return tid // self.warp_size

    def tid(self, warpid, laneid):
        return warpid * self.warp_size + laneid

    def gid(self, blockid, tid):
        return blockid * self.threads_per_block + tid

    def activemask(self, lanes):
        mask = 0
        for lane in lanes:
            if not 0 <= lane < self.warp_size:
                raise ValueError("lane out of range")
            mask |= 1 << lane
        return mask

    def special(self, name, *, tid, blockid, active_lanes,
                ntid=None, nwarps=None):
        if name == "laneid":
            return self.laneid(tid)
        if name == "warpid":
            return self.warpid(tid)
        if name == "blockid":
            return blockid
        if name == "tid":
            return tid
        if name == "ntid":
            return self.threads_per_block if ntid is None else ntid
        if name == "nwarps":
            return self.warps_per_block if nwarps is None else nwarps
        if name == "activemask":
            return self.activemask(active_lanes)
        raise KeyError(name)

    # --- checks ----------------------------------------------------------
    def selftest(self):
        assert self.warp_size > 0
        assert self.warps_per_block > 0
        assert self.warp_size * self.warps_per_block == self.threads_per_block
        assert self.all_active == (1 << self.warp_size) - 1
        assert self.data["active_mask"]["bits"] == self.warp_size

        for tid in range(self.threads_per_block):
            lane, warp = self.laneid(tid), self.warpid(tid)
            assert 0 <= lane < self.warp_size
            assert 0 <= warp < self.warps_per_block
            assert self.tid(warp, lane) == tid

        assert self.gid(2, 5) == 2 * self.threads_per_block + 5
        assert self.activemask(range(self.warp_size)) == self.all_active
        assert self.activemask([0, 2, 4, 6]) == 0x55
        assert self.activemask([1, 3, 5, 7]) == 0xAA

        assert self.special("laneid", tid=13, blockid=0, active_lanes=[]) == 5
        assert self.special("warpid", tid=13, blockid=0, active_lanes=[]) == 1
        assert self.special("tid", tid=13, blockid=0, active_lanes=[]) == 13
        assert self.special("blockid", tid=0, blockid=7, active_lanes=[]) == 7
        assert self.special("ntid", tid=0, blockid=0, active_lanes=[]) == 32
        assert self.special("nwarps", tid=0, blockid=0, active_lanes=[]) == 4
        assert self.special("activemask", tid=0, blockid=0,
                            active_lanes=range(self.warp_size)) == 0xFF

        names = set(self.special_funct)
        assert names == {"laneid", "warpid", "blockid", "tid", "ntid",
                         "nwarps", "activemask"}
        assert self.data["sys_funct"]["exit"] == 0
        assert self.data["sys_funct"]["ret"] == 1
        assert self.args_funct == 9
        assert self.launch["args_register"] == "ur0"
        assert self.launch["grid_dims"] == 1
        assert self.launch["max_threads_per_block"] == self.threads_per_block
        return True

    def check_doc(self, path):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        required = [
            f"warp_size = {self.warp_size}",
            f"warps_per_block = {self.warps_per_block}",
            f"threads_per_block = {self.threads_per_block}",
            f"tid = warpid * {self.warp_size} + laneid",
            f"gid = blockid * {self.threads_per_block} + tid",
            f"{self.all_active:#x}",
            "active mask",
            "每个 warp 一个 PC",
            "uniform",
            "global",
            "shared",
            "private",
            "constant",
        ]
        missing = [s for s in required if s not in text]
        if missing:
            raise AssertionError("doc is missing: " + ", ".join(missing))
        return True


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--check-doc", metavar="PATH")
    ap.add_argument("--encoding", metavar="NAME")
    args = ap.parse_args(argv)

    model = Model()
    if args.selftest:
        model.selftest()
        print("OK: SIMT model self-test")
        return 0
    if args.check_doc:
        model.check_doc(args.check_doc)
        print("OK: SIMT model matches " + args.check_doc)
        return 0
    if args.encoding:
        if args.encoding == "args":
            funct = model.args_funct
        else:
            funct = model.special_funct[args.encoding]
        word = (0x12 << 26) | (1 << 21) | funct
        print(",".join(f"0x{(word >> (8 * i)) & 0xFF:02x}" for i in range(4)))
        return 0

    print("warp_size=%d warps_per_block=%d threads_per_block=%d" %
          (model.warp_size, model.warps_per_block, model.threads_per_block))
    return 0


if __name__ == "__main__":
    sys.exit(main())
