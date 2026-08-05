#!/usr/bin/env python3
"""Static half of the bug hunt: content that is DECLARED but never CONSULTED.

`--audit` finds behaviour that never happens by playing. This finds the cause that playing
cannot distinguish from bad luck: an enum member the engine source never mentions at all.

The motivating case is Skill::Persist. It was declared in content.hpp, assigned to the Knight
in rules.cpp, and asserted in test_content.cpp -- a test that PASSED -- while game.cpp never
referenced the symbol, so Knights could never chain kills. Three files agreed the feature
existed and the one that had to implement it did not know. That is invisible to any dynamic
check, because "Knights never chained" looks exactly like "no Knight got the chance".

Two questions, both answerable with grep:

  1. Every enum member of the gameplay enums must appear somewhere in the engine sources.
  2. Every numeric key in the rules JSON must be parsed by rules.cpp, or it is a knob that
     silently does nothing when you tune it (HANDOFF section 7).

Exit code 1 if anything is unexplained, so this can gate a build.

Usage:  python3 tools/content_audit.py [--quiet]
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Enums whose members are BEHAVIOUR the engine has to act on. Not every enum qualifies --
# Terrain and Tribe are data the engine indexes by rather than branches on, so a member that
# never appears by name in game.cpp is expected and not a finding.
BEHAVIOURAL_ENUMS = ["Skill", "Verb", "CityReward", "Improvement"]

# Unreferenced members we can ACCOUNT FOR. These are reported, but do not fail the build.
#
# An entry here is a claim, and the reason has to be one of two kinds: either the feature is a
# documented gap, or the behaviour genuinely exists under another mechanism. Anything else --
# "probably fine", "seems unused" -- is how Persist survived three files agreeing it existed.
KNOWN_GAPS = {
    # Documented in HANDOFF "Known gaps": Cloak/Dagger and the special tribes.
    "Skill::Hide": "GAP: Cloak/Dagger invisibility, documented as unimplemented",
    "Skill::Infiltrate": "GAP: Cloak infiltrate, documented as unimplemented",
    "Skill::Creep": "GAP: Cloak movement, documented as unimplemented",
    "Skill::Surprise": "GAP: Dagger/Pirate, documented as unimplemented",
    "Skill::Independent": "GAP: Dagger/Pirate, documented as unimplemented",
    "Skill::Stomp": "GAP: Juggernaut stomp damage, documented as unimplemented",
    # Found by this script 2026-08-05 and NOT previously documented anywhere.
    "Skill::Splash": "GAP: Bomber splash damage. Formula is known (wiki Combat, and "
                     "live.json combat.splash_divisor) but nothing implements it, so a Bomber "
                     "is just a weak attacker. Not in any gap list before this script ran.",
    # Real behaviour, different mechanism.
    "Skill::Navigate": "GAP: Polaris tribe, unimplemented like the rest of the special tribes",
    "Skill::Carry": "OK: naval transport is modelled by UnitFlag::Embarked and carried_type "
                    "(R29), not by branching on the skill. The skill is descriptive here.",
}


def read(path):
    try:
        return path.read_text()
    except OSError:
        return ""


def enum_members(header_text, enum_name):
    """Members of `enum class <name>`, minus None/Count sentinels."""
    m = re.search(rf"enum class {enum_name}\s*:[^{{]*\{{(.*?)\}};", header_text, re.S)
    if not m:
        return []
    out = []
    for line in m.group(1).split("\n"):
        line = re.sub(r"//.*", "", line)
        for token in re.findall(r"\b([A-Z][A-Za-z0-9_]*)\s*(?:=|,|$)", line):
            if token in ("None", "Count"):
                continue
            out.append(token)
    return out


def main():
    quiet = "--quiet" in sys.argv
    findings = []
    accounted = []

    headers = "".join(
        read(p) for p in (ROOT / "core" / "include" / "poly").glob("*.hpp")
    )
    # The IMPLEMENTATION is what must consult a behavioural enum, and "implementation" has to
    # be defined carefully or the check is worthless:
    #
    #   - TESTS are excluded. test_content.cpp asserting the Knight has Persist is exactly the
    #     kind of passing test that hid the bug.
    #   - rules.cpp is excluded. It is the data PARSER and contains a name table mentioning
    #     every enum member by construction -- `{"persist", Skill::Persist}` was present the
    #     whole time Persist did nothing. Counting it as a reference is what made the first
    #     version of this script report a clean bill of health on a broken engine.
    engine = "".join(
        read(p)
        for p in (ROOT / "core" / "src").glob("*.cpp")
        if p.name != "rules.cpp"
    )

    for enum_name in BEHAVIOURAL_ENUMS:
        for member in enum_members(headers, enum_name):
            qualified = f"{enum_name}::{member}"
            if qualified in KNOWN_GAPS:
                accounted.append(f"{qualified} -- {KNOWN_GAPS[qualified]}")
                continue
            # Word boundary, so Skill::Persist does not match Skill::PersistSomething.
            if not re.search(rf"\b{re.escape(qualified)}\b", engine):
                findings.append(
                    f"{qualified} is declared but never referenced in core/src/*.cpp "
                    f"-- the engine cannot be acting on it"
                )

    # Rules knobs that are present in the data and never read by the parser.
    rules_parser = read(ROOT / "core" / "src" / "rules.cpp")
    try:
        data = json.loads(read(ROOT / "data" / "rules" / "live.json"))
    except json.JSONDecodeError as e:
        print(f"live.json does not parse: {e}")
        return 1

    def leaves(node, key=""):
        if isinstance(node, dict):
            for k, v in node.items():
                if k.startswith("_"):
                    continue
                yield from leaves(v, k)
        elif isinstance(node, list):
            return
        else:
            if key:
                yield key, node

    numeric = {}
    for key, value in leaves(data):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            continue
        numeric.setdefault(key, value)

    unparsed = sorted(k for k in numeric if k not in rules_parser)
    if unparsed:
        findings.append(
            f"{len(unparsed)} of {len(numeric)} numeric rule keys are never parsed by "
            f"rules.cpp, so editing them in live.json does nothing: "
            + ", ".join(unparsed[:6])
            + (" ..." if len(unparsed) > 6 else "")
        )

    if not quiet and accounted:
        print("=== ACCOUNTED FOR (declared, not consulted, and we know why) ===")
        for a in accounted:
            print(f"  {a}")
        print()
    if not quiet:
        print("=== UNEXPLAINED ===")
    if not findings:
        print("  none")
        return 0
    for f in findings:
        print(f"  {f}")
    print(f"\n{len(findings)} findings.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
