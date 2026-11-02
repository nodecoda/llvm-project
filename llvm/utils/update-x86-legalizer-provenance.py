#!/usr/bin/env python3
"""Generate the pinned provenance artifacts for the X86 legalizer differential.

The differential test compares the hand-written reference oracle against the
TableGen-generated legalizer table.  Pinning the oracle's bytes says nothing
about what those bytes mean, so this script turns the reference into three
reviewable, deterministic artifacts under llvm/unittests/Target/X86:

  x86-legalizer-behavior-queries.json
      Three concrete queries per table descriptor (189 total): a point the
      table answers with Answer::Hit ("hit"), a boundary point with a
      different type tuple and the widest last type ("boundary"), and the hit
      point with address space 1 ("as").

  x86-legalizer-llt-literals.json
      The distinct LLT::... spellings the reference source uses.

  x86-legalizer-reference-provenance.json
      Source identity, the builder signatures, hashes of the behavior
      queries, and the reference's behavior fingerprint.

Only the Python 3 standard library is used.  Output is byte-stable: inputs are
read in file order, derived lists are sorted, and no timestamp is emitted.

Query generation rules
----------------------
For a descriptor, the candidate points are every point in
X86LegalizerTablePoints.inc whose opcode maps to that descriptor through
x86TableForOpcode, in points-file order.

"Hit" is approximated statically, mirroring X86LegalizerTable::query(): a point
is a Hit candidate when every used type classifies into X86TypeClasses (same
kind/bits/lanes), no pointer or pointer vector has a non-zero address space, no
type is a pointer vector (the reader sends every pointer vector down the
residual path), and the memory descriptor, when present, also classifies.  The
first candidate satisfying this approximation is the "hit" query.  The HIT_
OFFSETS table below is the single place to skip a candidate if a later test
discovers that the table does not actually answer that point with
Answer::Hit.

The "boundary" query is the first candidate (points-file order) whose type
tuple differs from the hit's and whose last type has the largest bit width
among all such candidates.  Pointer width is the LP64 pointer width used by the
default config (C2); vector width is lanes * element width; pointer-vector
width is lanes * pointer width.  Ties keep the earlier point.  This is a
deterministic heuristic: it walks away from the hit point along the widest
slot the domain enumerates.

The "as" query starts from the hit point and raises the first pointer type to
address space 1.  With no pointer type, a pointer vector's element address
space is raised; with neither, the memory descriptor becomes p1.  The table is
allowed to answer Residual or Hit for this query; the point is to exercise the
address-space residual path.
"""

import argparse
import hashlib
import json
import os
import re
import sys

POINTER_BITS = 64  # C2 is x86_64/LP64; all behavior queries use C2.

# descriptor name -> number of statically-Hit candidates to skip.
# Empty as generated: the first candidate of every descriptor is already a
# real table Hit.  Keep this table as the centrally documented escape hatch.
HIT_OFFSETS = {}

SCHEMA_QUERIES = "llvm-x86-legalizer-behavior-queries"
SCHEMA_LITERALS = "llvm-x86-legalizer-llt-literals"
SCHEMA_PROVENANCE = "llvm-x86-legalizer-reference-provenance"

SOURCE_COMMIT = "ebac17721d12"
SOURCE_BLOB = "5cefdb37ddf34444f25e8a329bb5f8f51f012675f13c484e479654b98601428c"
BUILDER_API_COMMIT = "ebac17721d12"
DELETE_AFTER = "not planned; retirement requires an approved replacement oracle"


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def normalize_ws(text):
    return " ".join(text.split())


def normalize_call(text):
    """Collapse trivia without leaving a space just inside parentheses."""
    text = normalize_ws(text)
    text = re.sub(r"\(\s+", "(", text)
    text = re.sub(r"\s+\)", ")", text)
    return text


def split_top_level(text):
    """Split on commas that are not inside (), {} or []."""
    parts = []
    depth = 0
    current = []
    for ch in text:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current and "".join(current).strip():
        parts.append("".join(current).strip())
    return parts


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def parse_type_classes(table_inc):
    """Return the X86TypeClasses entries as (kind, bits, lanes) triples."""
    start = table_inc.index("X86TypeClasses[]")
    end = table_inc.index("};", start)
    block = table_inc[start:end]
    classes = []
    pattern = re.compile(
        r"/\*\d+\*/\s*\{Kind_(\w+),\s*(\d+),\s*(\d+)\},\s*//\s*(\w+)")
    for match in pattern.finditer(block):
        classes.append((match.group(1), int(match.group(2)),
                        int(match.group(3)), match.group(4)))
    if not classes:
        raise SystemExit("no type classes found in %s" % table_inc)
    return classes


def parse_descriptors(table_inc):
    descriptors = []
    pattern = re.compile(
        r"\s*/\*(\d+)\*/\s*\{\{.*?\},\s*//\s*(X86_\w+)\s*$")
    for line in table_inc.splitlines():
        match = pattern.match(line)
        if match:
            descriptors.append((int(match.group(1)), match.group(2)))
    if len(descriptors) != 63:
        raise SystemExit("expected 63 descriptors, found %d" % len(descriptors))
    return descriptors


def parse_opcode_map(table_inc):
    return {match.group(1): int(match.group(2))
            for match in re.finditer(
                r"case TargetOpcode::(G_\w+): return (\d+);", table_inc)}


def parse_llt(expr):
    """Parse one points-file type expression into a small canonical dict."""
    expr = expr.strip()
    match = re.fullmatch(r"LLT::pointer\((\d+),\s*PointerBits\)", expr)
    if match:
        return {"kind": "pointer", "as": int(match.group(1)),
                "bits": POINTER_BITS, "lanes": 1,
                "short": "p" + match.group(1)}
    match = re.fullmatch(r"LLT::scalar\((\d+)\)", expr)
    if match:
        bits = int(match.group(1))
        return {"kind": "scalar", "as": 0, "bits": bits, "lanes": 1,
                "short": "s%d" % bits}
    match = re.fullmatch(
        r"LLT::fixed_vector\((\d+),\s*LLT::pointer\((\d+),\s*PointerBits\)\)",
        expr)
    if match:
        lanes, address_space = int(match.group(1)), int(match.group(2))
        return {"kind": "ptrvector", "as": address_space, "bits": POINTER_BITS,
                "lanes": lanes,
                "short": "v%dp%d" % (lanes, address_space)}
    match = re.fullmatch(r"LLT::fixed_vector\((\d+),\s*(\d+)\)", expr)
    if match:
        lanes, bits = int(match.group(1)), int(match.group(2))
        return {"kind": "vector", "as": 0, "bits": bits, "lanes": lanes,
                "short": "v%ds%d" % (lanes, bits)}
    if expr == "LLT()":
        return {"kind": "none", "as": 0, "bits": 0, "lanes": 0,
                "short": "none"}
    raise SystemExit("unsupported LLT expression: %s" % expr)


def parse_points(points_inc):
    points = []
    for line in points_inc.splitlines():
        stripped = line.strip()
        if not stripped.startswith("Out.push_back("):
            continue
        if not stripped.endswith(");"):
            raise SystemExit("malformed point line: %s" % line)
        inner = stripped[len("Out.push_back("):-2].strip()
        if not (inner.startswith("{") and inner.endswith("}")):
            raise SystemExit("malformed point body: %s" % line)
        fields = split_top_level(inner[1:-1])
        if len(fields) != 7:
            raise SystemExit("expected 7 point fields, got %d: %s" %
                             (len(fields), line))
        opcode_name = fields[0].replace("TargetOpcode::", "")
        num_types = int(fields[2])
        if not (fields[3].startswith("{") and fields[3].endswith("}")):
            raise SystemExit("malformed type list: %s" % line)
        type_exprs = split_top_level(fields[3][1:-1])
        types = [parse_llt(expr) for expr in type_exprs]
        if len(types) < num_types:
            raise SystemExit("too few types: %s" % line)
        points.append({
            "opcode": opcode_name,
            "name": fields[1].strip('"'),
            "num_types": num_types,
            "types": types[:num_types],
            "mem": parse_llt(fields[4]),
            "align": int(fields[5]),
            "has_mem": fields[6] == "true",
        })
    if len(points) != 1397:
        raise SystemExit("expected 1397 points, found %d" % len(points))
    return points


def classifies_as(expr, classes):
    wanted = {
        "pointer": "Pointer",
        "scalar": "Scalar",
        "vector": "Vector",
        "ptrvector": "PtrVector",
    }[expr["kind"]]
    for kind, bits, lanes, _ in classes:
        if kind != wanted:
            continue
        if expr["kind"] == "pointer":
            return True
        if bits == expr["bits"] and lanes == expr["lanes"]:
            return True
    return False


def is_static_hit(point, classes):
    for ty in point["types"]:
        if ty["kind"] == "none":
            continue
        if ty["kind"] == "ptrvector":
            # The reader answers every pointer vector from the residual path.
            return False
        if ty["kind"] == "pointer" and ty["as"] != 0:
            return False
        if not classifies_as(ty, classes):
            return False
    if point["has_mem"]:
        mem = point["mem"]
        if mem["kind"] == "pointer" and mem["as"] != 0:
            return False
        if not classifies_as(mem, classes):
            return False
    return True


def type_tuple(point):
    return tuple(ty["short"] for ty in point["types"])


def last_type_width(point):
    if not point["types"]:
        return 0
    ty = point["types"][-1]
    if ty["kind"] == "pointer":
        return ty["bits"]
    if ty["kind"] == "ptrvector":
        return ty["lanes"] * ty["bits"]
    if ty["kind"] == "vector":
        return ty["lanes"] * ty["bits"]
    return ty["bits"]


def make_as_point(hit):
    """Return (types, mem) for the hit point with address space 1."""
    types = [dict(ty) for ty in hit["types"]]
    changed = False
    for ty in types:
        if ty["kind"] == "pointer":
            ty["as"] = 1
            ty["short"] = "p1"
            changed = True
            break
    if not changed:
        for ty in types:
            if ty["kind"] == "ptrvector":
                ty["as"] = 1
                ty["short"] = "v%dp1" % ty["lanes"]
                changed = True
                break
    mem = dict(hit["mem"])
    if not changed and hit["has_mem"]:
        mem = {"kind": "pointer", "as": 1, "bits": POINTER_BITS, "lanes": 1,
               "short": "p1"}
        changed = True
    if not changed:
        # Nothing in the hit point carries an address space.  Keeping the point
        # unchanged is deterministic and still exercises the descriptor; the
        # condition is reported at generation time.
        sys.stderr.write("warning: %s has no pointer for the 'as' query\n" %
                         hit["name"])
    return types, mem


def memdesc_json(point, mem=None):
    if not point["has_mem"]:
        return None
    mem = point["mem"] if mem is None else mem
    return {"memory_ty": mem["short"], "align_bits": point["align"]}


def make_query(descriptor, kind, point, types=None, mem=None):
    types = point["types"] if types is None else types
    return {
        "id": "%s/%s" % (descriptor, kind),
        "descriptor": descriptor,
        "kind": kind,
        "config": "C2",
        "opcode": point["opcode"],
        "types": [ty["short"] for ty in types],
        "memdesc": memdesc_json(point, mem),
    }


def build_queries(descriptors, opcode_map, points, classes):
    by_opcode = {}
    for point in points:
        by_opcode.setdefault(point["opcode"], []).append(point)

    opcodes_by_descriptor = {}
    for opcode, table in opcode_map.items():
        opcodes_by_descriptor.setdefault(table, []).append(opcode)

    queries = []
    for table, descriptor in descriptors:
        candidates = []
        for opcode in opcodes_by_descriptor.get(table, []):
            candidates.extend(by_opcode.get(opcode, []))
        if not candidates:
            raise SystemExit("%s has no candidate points" % descriptor)

        hit_candidates = [point for point in candidates
                          if is_static_hit(point, classes)]
        offset = HIT_OFFSETS.get(descriptor, 0)
        if offset >= len(hit_candidates):
            raise SystemExit("%s hit offset %d out of range" % (descriptor, offset))
        hit = hit_candidates[offset]

        hit_tuple = type_tuple(hit)
        boundary_candidates = [point for point in candidates
                               if type_tuple(point) != hit_tuple]
        if not boundary_candidates:
            raise SystemExit("%s has no boundary candidate" % descriptor)
        widest = max(last_type_width(point) for point in boundary_candidates)
        boundary = next(point for point in boundary_candidates
                        if last_type_width(point) == widest)

        as_types, as_mem = make_as_point(hit)

        queries.append(make_query(descriptor, "hit", hit))
        queries.append(make_query(descriptor, "boundary", boundary))
        queries.append(make_query(descriptor, "as", hit, as_types, as_mem))

    queries.sort(key=lambda query: query["id"])
    if len(queries) != 189:
        raise SystemExit("expected 189 queries, found %d" % len(queries))
    return queries


def find_matching_paren(text, open_index):
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise SystemExit("unbalanced parentheses")


def canonical_calls(source, prefix):
    calls = []
    index = 0
    while True:
        start = source.find(prefix, index)
        if start < 0:
            break
        open_index = source.find("(", start + len(prefix))
        if open_index < 0:
            break
        end = find_matching_paren(source, open_index)
        calls.append((start, source[start:end + 1]))
        index = end + 1
    return calls


def extract_builders(source):
    loops = [(match.start(), [op.strip() for op in match.group(1).split(",")])
             for match in re.finditer(
                 r"for\s*\(unsigned Op\s*:\s*\{([^}]*)\}\)", source)]
    builders = []
    for start, call in canonical_calls(source, "getActionDefinitionsBuilder"):
        normalized = normalize_call(call)
        if normalized == "getActionDefinitionsBuilder(Op)":
            enclosing = [ops for loop_start, ops in loops if loop_start < start]
            if not enclosing:
                raise SystemExit("builder(Op) outside a loop")
            normalized = "getActionDefinitionsBuilder({%s})" % ", ".join(
                enclosing[-1])
        builders.append(normalized)
    unique = sorted(set(builders))
    if len(builders) != 58 or len(unique) != 58:
        raise SystemExit("expected 58 unique builders, found %d/%d" %
                         (len(builders), len(unique)))
    return unique


def extract_literals(source):
    # This artifact is a set of spellings, not a count of call sites: a
    # repeated LLT::... expression collapses to one entry.
    literals = set()
    for match in re.finditer(r"LLT::[A-Za-z_]\w*\(", source):
        end = find_matching_paren(source, match.end() - 1)
        literals.add(normalize_ws(source[match.start():end + 1]))
    return sorted(literals)


def write_json(path, value):
    text = json.dumps(value, indent=2) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return text


def existing_behavior_sha(path):
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as handle:
            value = json.load(handle).get("reference_behavior_sha256")
    except (ValueError, OSError):
        return None
    if isinstance(value, str) and value != "TBD":
        return value
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table-inc", required=True)
    parser.add_argument("--points-inc", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument(
        "--reference-behavior-sha256", default=None,
        help="bootstrapped fingerprint; preserved from an existing provenance "
             "file when omitted")
    args = parser.parse_args()

    table_inc = read_text(args.table_inc)
    points_inc = read_text(args.points_inc)
    reference = read_text(args.reference)
    reference_bytes = open(args.reference, "rb").read()

    descriptors = parse_descriptors(table_inc)
    opcode_map = parse_opcode_map(table_inc)
    classes = parse_type_classes(table_inc)
    points = parse_points(points_inc)

    queries = build_queries(descriptors, opcode_map, points, classes)
    queries_doc = {
        "schema": SCHEMA_QUERIES,
        "version": 1,
        "comment": ("Hit points are a static approximation of the table's "
                    "Answer::Hit: classifiable types, no non-zero address "
                    "space and no pointer vector.  Boundary picks the first "
                    "candidate with a different type tuple and the widest "
                    "last type; ties keep the earlier point."),
        "count": len(queries),
        "config_default": "C2",
        "queries": queries,
    }
    queries_path = os.path.join(args.out_dir,
                                "x86-legalizer-behavior-queries.json")
    queries_text = write_json(queries_path, queries_doc)

    literals = extract_literals(reference)
    literals_doc = {
        "schema": SCHEMA_LITERALS,
        "version": 1,
        "source": os.path.basename(args.reference),
        "count": len(literals),
        "literals": literals,
    }
    write_json(os.path.join(args.out_dir,
                            "x86-legalizer-llt-literals.json"), literals_doc)

    provenance_path = os.path.join(
        args.out_dir, "x86-legalizer-reference-provenance.json")
    behavior_sha = args.reference_behavior_sha256 or existing_behavior_sha(
        provenance_path) or "TBD"
    provenance_doc = {
        "schema": SCHEMA_PROVENANCE,
        "version": 1,
        "source_commit": SOURCE_COMMIT,
        "source_blob": SOURCE_BLOB,
        "builder_api_commit": BUILDER_API_COMMIT,
        "reference_sha256": sha256_bytes(reference_bytes),
        "builder_count": len(extract_builders(reference)),
        "builders": extract_builders(reference),
        "behavior_query_count": len(queries),
        "behavior_queries_sha256": sha256_text(queries_text),
        "reference_behavior_sha256": behavior_sha,
        "delete_after": DELETE_AFTER,
    }
    write_json(provenance_path, provenance_doc)

    print("wrote %d queries, %d literals, %d builders" %
          (len(queries), len(literals), provenance_doc["builder_count"]))
    print("reference_sha256: %s" % provenance_doc["reference_sha256"])
    print("behavior_queries_sha256: %s" %
          provenance_doc["behavior_queries_sha256"])
    print("reference_behavior_sha256: %s" % behavior_sha)


if __name__ == "__main__":
    main()
