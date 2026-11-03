"""X86 target vocabulary for the C++ legalizer-corpus reader.

Only this module knows that the input is X86: the feature environment,
the LLT spellings used by the source, and the reviewed boundary panels.
"""

# The Subtarget predicates the X86 legalizer constructor reads.  A new one in
# the C++ source is a hard error until it is added here (see parse.py).
FEATURE_KEYS = (
    "is64Bit",
    "canUseCMOV",
    "hasSSE1",
    "hasSSE2",
    "hasSSE41",
    "hasAVX",
    "hasAVX2",
    "hasAVX512",
    "hasVLX",
    "hasDQI",
    "hasBWI",
    "useSoftFloat",
    "hasX87",
    "hasPOPCNT",
    "hasLZCNT",
    "hasBMI",
    "hasVPOPCNTDQ",
    "isTarget64BitLP64",
)

# Configs mirror the four FullConfigs in X86LegalizerInfoDifferentialTest.cpp.
# C2 is the witness config and is fully featured, so every guard is taken.
CONFIGS = {
    "C0": {
        "is64Bit": False, "canUseCMOV": False, "hasSSE1": False, "hasSSE2": False,
        "hasSSE41": False, "hasAVX": False, "hasAVX2": False, "hasAVX512": False,
        "hasVLX": False, "hasDQI": False, "hasBWI": False, "useSoftFloat": False,
        "hasX87": False, "hasPOPCNT": False, "hasLZCNT": False, "hasBMI": False,
        "hasVPOPCNTDQ": False, "isTarget64BitLP64": False,
    },
    "C1": {
        "is64Bit": True, "canUseCMOV": True, "hasSSE1": True, "hasSSE2": True,
        "hasSSE41": False, "hasAVX": False, "hasAVX2": False, "hasAVX512": False,
        "hasVLX": False, "hasDQI": False, "hasBWI": False, "useSoftFloat": False,
        "hasX87": True, "hasPOPCNT": False, "hasLZCNT": False, "hasBMI": False,
        "hasVPOPCNTDQ": False, "isTarget64BitLP64": True,
    },
    "C2": {
        "is64Bit": True, "canUseCMOV": True, "hasSSE1": True, "hasSSE2": True,
        "hasSSE41": True, "hasAVX": True, "hasAVX2": True, "hasAVX512": True,
        "hasVLX": True, "hasDQI": True, "hasBWI": True, "useSoftFloat": False,
        "hasX87": True, "hasPOPCNT": True, "hasLZCNT": True, "hasBMI": True,
        "hasVPOPCNTDQ": True, "isTarget64BitLP64": True,
    },
    "C3": {
        "is64Bit": False, "canUseCMOV": True, "hasSSE1": False, "hasSSE2": False,
        "hasSSE41": False, "hasAVX": False, "hasAVX2": False, "hasAVX512": False,
        "hasVLX": False, "hasDQI": False, "hasBWI": False, "useSoftFloat": False,
        "hasX87": True, "hasPOPCNT": True, "hasLZCNT": True, "hasBMI": True,
        "hasVPOPCNTDQ": False, "isTarget64BitLP64": False,
    },
}

# The reviewed 12-atom witness/full panel.  This is a review constant, not a
# property derived from the C++ source; changes to it are visible in review.
BOUNDARY_PANEL = (
    ("s", 3), ("s", 7), ("s", 24), ("s", 33), ("s", 65), ("s", 100),
    ("s", 129), ("s", 200), ("s", 256),
    ("v", 3, ("s", 32)), ("v", 16, ("s", 16)), ("v", 64, ("s", 8)),
)

# The reviewed 60-atom deep panel (x86-legalizer-deep-panel.json).
DEEP_PANEL = (
    ("s", 100), ("s", 1024), ("s", 12), ("s", 127), ("s", 129), ("s", 15),
    ("s", 160), ("s", 17), ("s", 192), ("s", 2), ("s", 20), ("s", 200),
    ("s", 224), ("s", 23), ("s", 24), ("s", 255), ("s", 256), ("s", 3),
    ("s", 31), ("s", 33), ("s", 384), ("s", 4), ("s", 40), ("s", 48),
    ("s", 5), ("s", 512), ("s", 6), ("s", 63), ("s", 65), ("s", 7),
    ("s", 72), ("s", 9), ("s", 96),
    ("v", 12, ("s", 16)), ("v", 12, ("s", 32)), ("v", 16, ("s", 16)),
    ("v", 16, ("s", 64)), ("v", 2, ("s", 128)), ("v", 2, ("s", 16)),
    ("v", 2, ("s", 8)), ("v", 32, ("s", 32)), ("v", 32, ("s", 64)),
    ("v", 3, ("s", 128)), ("v", 3, ("s", 16)), ("v", 3, ("s", 32)),
    ("v", 3, ("s", 64)), ("v", 3, ("s", 8)), ("v", 4, ("s", 128)),
    ("v", 4, ("s", 16)), ("v", 5, ("s", 32)), ("v", 5, ("s", 64)),
    ("v", 64, ("s", 16)), ("v", 64, ("s", 8)), ("v", 6, ("s", 16)),
    ("v", 6, ("s", 32)), ("v", 6, ("s", 64)), ("v", 7, ("s", 16)),
    ("v", 7, ("s", 32)), ("v", 7, ("s", 64)), ("v", 8, ("s", 8)),
)

# Rule sets whose slot-1 address-space points are skipped by the reviewed
# witness budget (X86LegalizerInfoDifferentialTest.cpp:skipsWitnessSlot1AS).
SKIP_WITNESS_SLOT1_AS = {
    "X86_MERGE_VALUES", "X86_UNMERGE_VALUES", "X86_ANYEXT", "X86_SEXT",
    "X86_FCMP",
}

DEFAULT_CONFIG = "C2"
DEFAULT_SOURCE_PATH = "llvm/lib/Target/X86/GISel/X86LegalizerInfo.cpp"
