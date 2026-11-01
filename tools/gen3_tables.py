#!/usr/bin/env python3
"""Generate src/gen3/gen3_data_gen.c and files/gen3_bulk.bin from a pinned pret/pokeemerald clone.

Usage: python3 -I tools/gen3_tables.py <pokeemerald>

The path must be the top of a clean checkout (tracked files) of the pinned commit.
The clone is read as text only; nothing in it is built or run. Standard library only.
Only game facts (ids, base stats, learnsets, names, PP) are extracted. On any failed
check the script names the first failure, exits 1 and writes nothing.

gen3_data_gen.c holds the tables the app keeps in RAM. gen3_bulk.bin holds the bulk tables
it reads rarely, from the SD card: species, move and item names, the name-sorted move and
holdable item lists, and learnsets. Its layout (version 2, 24383 bytes), little-endian:

    0      4         magic "G3BK"
    4      2         version 2
    6      2         species_count 387 (index 0 unused)
    8      4         learnset_entries 3947
    12     4         CRC-32 (IEEE) of the bytes from 16 to the end
    16     11 * 387  species names, NUL-padded, index = National Dex
    4273   2 * 388   learnset start offsets, in entries
    5049   2 * 3947  learnset entries (level << 9) | move
    12943  13 * 355  move names, NUL-padded, index = move id (move 0 is "-")
    17558  2 * 354   move ids 1..354, sorted by name
    18266  15 * 377  item names, NUL-padded, index = item id (all NUL for an id without a name)
    23921  2 * 231   holdable item ids, sorted by name
    24383            end of file

Bytes 16..12942 are the whole payload of version 1.
"""

import binascii
import os
import re
import struct
import subprocess
import sys
from fractions import Fraction

PINNED_COMMIT = "731ad5bfd6e6f265508d0efcca0ba42f9dcf5881"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(REPO_ROOT, "src", "gen3", "gen3_data_gen.c")
BULK_OUTPUT = os.path.join(REPO_ROOT, "files", "gen3_bulk.bin")

NATIONAL_COUNT = 386
INTERNAL_COUNT = 412
MOVE_COUNT = 355
ITEM_COUNT = 377
ABILITY_COUNT = 78
NATURE_COUNT = 25
TYPE_COUNT = 18
LEARNSET_COUNT = 3947
LEARNSET_MAX = 17  # the most entries one species has; GEN3_LEARNSET_MAX
HOLDABLE_COUNT = 231
OLD_UNOWN_FIRST = 252
OLD_UNOWN_LAST = 276
MAX_LEVEL = 100

NAME_MAX = {"species": 10, "move": 12, "item": 14, "ability": 12, "nature": 7, "type": 6}
GENDER_CONSTANTS = {"MON_MALE": 0, "MON_FEMALE": 254, "MON_GENDERLESS": 255}
UNUSED_ITEM_NAME = "????????"

BULK_MAGIC = b"G3BK"
BULK_VERSION = 2
BULK_HEADER_SIZE = 16
BULK_PAYLOAD_SIZE = 24367  # so the file is 24383 bytes
BULK_NAME_SIZE = NAME_MAX["species"] + 1
BULK_MOVE_NAME_SIZE = NAME_MAX["move"] + 1
BULK_ITEM_NAME_SIZE = NAME_MAX["item"] + 1

IDENT_RE = re.compile(r"[A-Za-z_]\w*")
STRING_RE = re.compile(r'_\(\s*"((?:[^"\\\n]|\\.)*)"\s*\)')
TRIGRAPH_RE = re.compile(r"\?\?[=/'()!<>-]")


class GenError(Exception):
    pass


def fail(message):
    raise GenError(message)


# ---------------------------------------------------------------------------
# Reading C source as data


def read_source(clone, rel):
    path = os.path.join(clone, rel)
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        fail(f"cannot read {rel}: {e.strerror}")
    except UnicodeDecodeError:
        fail(f"{rel}: not UTF-8")
    return strip_comments(text)


_COMMENT_OR_LITERAL_RE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S
)


def strip_comments(text):
    def keep_literals(m):
        s = m.group(0)
        return " " if s.startswith("/") else s

    return _COMMENT_OR_LITERAL_RE.sub(keep_literals, text)


def skip_literal(text, i):
    """text[i] is a quote; returns the index just past the closing quote."""
    quote = text[i]
    i += 1
    while i < len(text) and text[i] != quote:
        i += 2 if text[i] == "\\" else 1
    return i + 1


def matching_brace(text, open_pos, where):
    depth = 0
    i = open_pos
    while i < len(text):
        c = text[i]
        if c in "\"'":
            i = skip_literal(text, i)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    fail(f"{where}: unbalanced braces")


def top_level_split(text):
    """Splits on commas outside braces, parentheses, brackets and literals."""
    parts = []
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        c = text[i]
        if c in "\"'":
            i = skip_literal(text, i)
            continue
        if c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
        i += 1
    parts.append(text[start:].strip())
    return [p for p in parts if p]


def array_body(text, name, where):
    m = re.search(r"\b" + re.escape(name) + r"\s*(?:\[[^\]]*\]\s*)*=\s*\{", text)
    if not m:
        fail(f"{where}: {name} not found")
    open_pos = m.end() - 1
    return text[open_pos + 1 : matching_brace(text, open_pos, where)]


def designated(body, prefix, where):
    """[PREFIX_X] = value entries of an initializer body, in source order."""
    out = []
    for part in top_level_split(body):
        m = re.fullmatch(r"\[\s*(" + prefix + r"\w*)\s*\]\s*=\s*(.*)", part, re.S)
        if not m:
            fail(f"{where}: unexpected initializer {part[:40]!r}")
        out.append((m.group(1), m.group(2).strip()))
    return out


def struct_fields(value, where):
    """{ .a = x, .b = {y, z}, } -> {"a": "x", "b": "{y, z}"}."""
    if not (value.startswith("{") and value.endswith("}")):
        fail(f"{where}: expected a {{ ... }} block")
    fields = {}
    for part in top_level_split(value[1:-1]):
        m = re.fullmatch(r"\.(\w+)\s*=\s*(.*)", part, re.S)
        if not m:
            fail(f"{where}: unexpected field {part[:40]!r}")
        fields[m.group(1)] = m.group(2).strip()
    return fields


def field(fields, name, where):
    if name not in fields:
        fail(f"{where}: missing .{name}")
    return fields[name]


def brace_list(value, count, where):
    if not (value.startswith("{") and value.endswith("}")):
        fail(f"{where}: expected a {{ ... }} list")
    items = top_level_split(value[1:-1])
    if len(items) != count:
        fail(f"{where}: expected {count} values, got {len(items)}")
    return items


# ---------------------------------------------------------------------------
# Constants: object-like #defines and enums


class _Expr:
    """Integer constant expressions: literals, identifiers, ( ) unary - and + - * / << >> & |."""

    TOKEN_RE = re.compile(r"\s*(?:(0[xX][0-9a-fA-F]+|\d+)|([A-Za-z_]\w*)|(<<|>>|[-+*/()&|]))")
    LEVELS = (("|",), ("&",), ("<<", ">>"), ("+", "-"), ("*", "/"))

    def __init__(self, text, lookup):
        self.tokens = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = self.TOKEN_RE.match(text, pos)
            if not m or m.end() == pos:
                raise ValueError(text)
            if m.group(1):
                self.tokens.append(("num", int(m.group(1), 0)))
            elif m.group(2):
                self.tokens.append(("num", lookup(m.group(2))))
            else:
                self.tokens.append(("op", m.group(3)))
            pos = m.end()
            while pos < len(text) and text[pos].isspace():
                pos += 1
        self.pos = 0

    def value(self):
        v = self.binary(0)
        if self.pos != len(self.tokens):
            raise ValueError("trailing tokens")
        return v

    def peek_op(self):
        if self.pos < len(self.tokens) and self.tokens[self.pos][0] == "op":
            return self.tokens[self.pos][1]
        return None

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        v = self.binary(level + 1)
        while self.peek_op() in self.LEVELS[level]:
            op = self.tokens[self.pos][1]
            self.pos += 1
            r = self.binary(level + 1)
            if op == "|":
                v |= r
            elif op == "&":
                v &= r
            elif op == "<<":
                v <<= r
            elif op == ">>":
                v >>= r
            elif op == "+":
                v += r
            elif op == "-":
                v -= r
            elif op == "*":
                v *= r
            else:
                if r == 0:
                    raise ValueError("division by zero")
                v //= r
        return v

    def unary(self):
        if self.peek_op() == "-":
            self.pos += 1
            return -self.unary()
        if self.peek_op() == "(":
            self.pos += 1
            v = self.binary(0)
            if self.peek_op() != ")":
                raise ValueError("missing )")
            self.pos += 1
            return v
        if self.pos < len(self.tokens) and self.tokens[self.pos][0] == "num":
            v = self.tokens[self.pos][1]
            self.pos += 1
            return v
        raise ValueError("expected a value")


class Defines:
    """Object-like #defines of one header, evaluated on demand."""

    DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)\b(?!\()[ \t]*(.*)$", re.M)

    def __init__(self, text, where):
        self.where = where
        self.raw = {m.group(1): m.group(2).strip() for m in self.DEFINE_RE.finditer(text)}
        self.values = {}

    def value(self, name, _active=()):
        if name in self.values:
            return self.values[name]
        if name not in self.raw or name in _active:
            raise KeyError(name)
        try:
            v = _Expr(self.raw[name], lambda n: self.value(n, _active + (name,))).value()
        except (KeyError, ValueError):
            fail(f"{self.where}: cannot evaluate {name}")
        self.values[name] = v
        return v

    def with_prefix(self, prefix):
        return {n: self.value(n) for n in self.raw if n.startswith(prefix)}


def first_enum(text, first, where):
    m = re.search(r"\benum\s*\{", text)
    if not m:
        fail(f"{where}: no enum")
    open_pos = m.end() - 1
    entries = top_level_split(text[open_pos + 1 : matching_brace(text, open_pos, where)])
    if not entries or entries[0] != first:
        fail(f"{where}: first enum does not start with {first}")
    values = {}
    for i, entry in enumerate(entries):
        if not IDENT_RE.fullmatch(entry):
            fail(f"{where}: unexpected enum entry {entry[:40]!r}")
        values[entry] = i
    return values


def lookup(table, name, where):
    if name not in table:
        fail(f"{where}: unknown constant {name}")
    return table[name]


def u8(value, where):
    if not 0 <= value <= 0xFF:
        fail(f"{where}: {value} does not fit in a byte")
    return value


def number(value, where, names=None):
    def resolve(n):
        if names is None or n not in names:
            raise KeyError(n)
        return names[n]

    try:
        return _Expr(value, resolve).value()
    except (KeyError, ValueError):
        fail(f"{where}: cannot evaluate {value!r}")


# ---------------------------------------------------------------------------
# Names


def game_string(value, where):
    m = STRING_RE.fullmatch(value.strip())
    if not m:
        fail(f"{where}: expected _(\"...\"), got {value[:40]!r}")
    return m.group(1)


def ascii_name(raw, kind, where):
    """Maps a game string to the app's ASCII form: e-acute -> E, female -> \\200, male -> \\201."""
    text = raw.replace("{POKEBLOCK}", "POKEBLOCK")
    out = []
    for ch in text:
        if ch == "\u00e9":
            out.append("E")
        elif ch == "\u2640":
            out.append("\x80")
        elif ch == "\u2642":
            out.append("\x81")
        elif 0x20 <= ord(ch) <= 0x7E and ch not in "{}\\\"":
            out.append(ch)
        else:
            fail(f"{where}: unsupported character {ch!r} in {raw!r}")
    name = "".join(out)
    if len(name) > NAME_MAX[kind]:
        fail(f"{where}: {kind} name {raw!r} is longer than {NAME_MAX[kind]} characters")
    return name


def name_bytes(name):
    return name.encode("latin-1")


def c_string(name):
    body = name.replace("\x80", "\\200").replace("\x81", "\\201")
    if TRIGRAPH_RE.search(body):
        fail(f"name {body!r} contains a trigraph")
    return '"' + body + '"'


def exact_ids(entries, count, kind):
    """entries: {id: x}. Requires exactly ids 0..count-1."""
    missing = [i for i in range(count) if i not in entries]
    extra = sorted(i for i in entries if not 0 <= i < count)
    if missing or extra or len(entries) != count:
        fail(
            f"expected {count} {kind}, got {len(entries)} "
            f"(missing {missing[:5]}, extra {extra[:5]})"
        )


def gender_ratio(value, where):
    if value in GENDER_CONSTANTS:
        return GENDER_CONSTANTS[value]
    m = re.fullmatch(r"PERCENT_FEMALE\(\s*(\d+(?:\.\d+)?)\s*\)", value)
    if not m:
        fail(f"{where}: unexpected genderRatio {value!r}")
    return min(254, int(Fraction(m.group(1)) * 255 / 100))


# ---------------------------------------------------------------------------
# Extraction


def git(clone, *args):
    """Runs a read-only git query in the clone: no fsmonitor hook, no index refresh writes."""
    command = ["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C", clone, *args]
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except OSError as e:
        fail(f"cannot run git: {e.strerror}")
    return result.returncode, result.stdout.strip()


def check_clone(clone):
    """The path must be the top of a clean checkout of the pinned commit."""
    code, top = git(clone, "rev-parse", "--show-toplevel")
    if code != 0 or os.path.realpath(top) != os.path.realpath(clone):
        fail(f"{clone} is not the top-level directory of a git clone")
    code, head = git(clone, "rev-parse", "HEAD")
    if code != 0 or head != PINNED_COMMIT:
        fail(
            f"{clone} is at commit {head or '(unknown)'}; "
            f"need pret/pokeemerald {PINNED_COMMIT}"
        )
    code, changes = git(clone, "status", "--porcelain", "--untracked-files=no")
    if code != 0 or changes:
        fail(
            f"{clone} has local changes to tracked files; "
            f"need a clean checkout of {PINNED_COMMIT}"
        )


def mail_items(clone, item_ids):
    """Item ids for which ItemIsMail() in src/mail_data.c returns TRUE."""
    where = "src/mail_data.c"
    text = read_source(clone, where)
    m = re.search(r"\bbool8\s+ItemIsMail\s*\(\s*u16\s+\w+\s*\)\s*\{", text)
    if not m:
        fail(f"{where}: ItemIsMail not found")
    open_pos = m.end() - 1
    body = " ".join(text[open_pos + 1 : matching_brace(text, open_pos, where)].split())
    shape = r"switch \(\w+\) \{ ((?:case ITEM_\w+: )+)return TRUE; default: return FALSE; \}"
    m = re.fullmatch(shape, body)
    if not m:
        fail(f"{where}: ItemIsMail is not a plain switch over item ids")
    return {lookup(item_ids, name, where) for name in re.findall(r"ITEM_\w+", m.group(1))}


def extract(clone):
    d = {}

    species_where = "include/constants/species.h"
    species_ids = Defines(read_source(clone, species_where), species_where).with_prefix("SPECIES_")
    national_ids = first_enum(
        read_source(clone, "include/constants/pokedex.h"),
        "NATIONAL_DEX_NONE",
        "include/constants/pokedex.h",
    )
    item_ids = first_enum(
        read_source(clone, "include/constants/items.h"), "ITEM_NONE", "include/constants/items.h"
    )
    move_ids = Defines(
        read_source(clone, "include/constants/moves.h"), "include/constants/moves.h"
    ).with_prefix("MOVE_")
    ability_ids = Defines(
        read_source(clone, "include/constants/abilities.h"), "include/constants/abilities.h"
    ).with_prefix("ABILITY_")
    pokemon_where = "include/constants/pokemon.h"
    pokemon_defines = Defines(read_source(clone, pokemon_where), pokemon_where)
    type_ids = pokemon_defines.with_prefix("TYPE_")
    growth_ids = pokemon_defines.with_prefix("GROWTH_")
    nature_ids = pokemon_defines.with_prefix("NATURE_")
    try:
        friendship_names = {"STANDARD_FRIENDSHIP": pokemon_defines.value("STANDARD_FRIENDSHIP")}
    except KeyError:
        fail(f"{pokemon_where}: STANDARD_FRIENDSHIP not found")

    # Internal species id -> National Dex number.
    where = "src/pokemon.c"
    body = array_body(read_source(clone, where), "sSpeciesToNationalPokedexNum", where)
    internal_to_national = [0] * INTERNAL_COUNT
    species_label = {0: "NONE"}
    seen_internal = set()
    for name in re.findall(r"\bSPECIES_TO_NATIONAL\(\s*(\w+)\s*\)", body):
        internal = lookup(species_ids, "SPECIES_" + name, where)
        national = lookup(national_ids, "NATIONAL_DEX_" + name, where)
        if not 0 < internal < INTERNAL_COUNT or internal in seen_internal:
            fail(f"{where}: bad or repeated internal id for {name}")
        seen_internal.add(internal)
        internal_to_national[internal] = national if 1 <= national <= NATIONAL_COUNT else 0
        species_label[internal] = name
    national_to_internal = [0] * (NATIONAL_COUNT + 1)
    for n in range(1, NATIONAL_COUNT + 1):
        internals = [i for i, v in enumerate(internal_to_national) if v == n]
        if len(internals) != 1:
            fail(f"national {n} has {len(internals)} internal ids")
        national_to_internal[n] = internals[0]
    for i in range(OLD_UNOWN_FIRST, OLD_UNOWN_LAST + 1):
        if internal_to_national[i] != 0:
            fail(f"internal {i} maps to national {internal_to_national[i]}, expected 0")
    d["internal_to_national"] = internal_to_national
    d["national_to_internal"] = national_to_internal
    d["national_label"] = [
        species_label.get(national_to_internal[n], "NONE") for n in range(NATIONAL_COUNT + 1)
    ]
    d["internal_label"] = [species_label.get(i, "?") for i in range(INTERNAL_COUNT)]

    def national_of(species_name, where):
        internal = lookup(species_ids, species_name, where)
        return internal_to_national[internal] if 0 <= internal < INTERNAL_COUNT else 0

    # Species info.
    where = "src/data/pokemon/species_info.h"
    body = array_body(read_source(clone, where), "gSpeciesInfo", where)
    info = {}
    for name, value in designated(body, "SPECIES_", where):
        if value == "OLD_UNOWN_SPECIES_INFO":
            continue
        n = national_of(name, where)
        if n == 0:
            continue
        w = f"{where}: {name}"
        if n in info:
            fail(f"{w}: repeated")
        f = struct_fields(value, w)
        base = [
            u8(number(field(f, k, w), w), w)
            for k in (
                "baseHP",
                "baseAttack",
                "baseDefense",
                "baseSpeed",
                "baseSpAttack",
                "baseSpDefense",
            )
        ]
        types = [u8(lookup(type_ids, t, w), w) for t in brace_list(field(f, "types", w), 2, w)]
        abilities = [
            u8(lookup(ability_ids, a, w), w) for a in brace_list(field(f, "abilities", w), 2, w)
        ]
        growth = lookup(growth_ids, field(f, "growthRate", w), w)
        if not 0 <= growth <= 5:
            fail(f"{w}: growth {growth} out of range")
        friendship = u8(number(field(f, "friendship", w), w, friendship_names), w)
        ratio = gender_ratio(field(f, "genderRatio", w), w)
        info[n] = (base, types, ratio, growth, abilities, friendship)
    for n in range(1, NATIONAL_COUNT + 1):
        if n not in info:
            fail(f"{where}: no species info for national {n}")
    d["info"] = info

    # Species names.
    where = "src/data/text/species_names.h"
    body = array_body(read_source(clone, where), "gSpeciesNames", where)
    species_names = {}
    for name, value in designated(body, "SPECIES_", where):
        n = national_of(name, where)
        if n == 0:
            continue
        if n in species_names:
            fail(f"{where}: {name} repeated")
        species_names[n] = ascii_name(game_string(value, where), "species", f"{where}: {name}")
    for n in range(1, NATIONAL_COUNT + 1):
        if n not in species_names:
            fail(f"{where}: no name for national {n}")
    d["species_names"] = species_names

    # Moves.
    where = "src/data/text/move_names.h"
    body = array_body(read_source(clone, where), "gMoveNames", where)
    move_names = {}
    move_label = {}
    for name, value in designated(body, "MOVE_", where):
        move = lookup(move_ids, name, where)
        if move in move_names:
            fail(f"{where}: {name} repeated")
        move_names[move] = ascii_name(game_string(value, where), "move", f"{where}: {name}")
        move_label[move] = name[len("MOVE_") :]
    exact_ids(move_names, MOVE_COUNT, "move names")
    where = "src/data/battle_moves.h"
    body = array_body(read_source(clone, where), "gBattleMoves", where)
    move_pp = {}
    for name, value in designated(body, "MOVE_", where):
        move = lookup(move_ids, name, where)
        if move in move_pp:
            fail(f"{where}: {name} repeated")
        w = f"{where}: {name}"
        move_pp[move] = u8(number(field(struct_fields(value, w), "pp", w), w), w)
    exact_ids(move_pp, MOVE_COUNT, "move PP values")
    d["move_names"] = move_names
    d["move_label"] = move_label
    d["move_pp"] = move_pp

    # Learnsets.
    where = "src/data/pokemon/level_up_learnsets.h"
    text = read_source(clone, where)
    learnset_arrays = {}
    for m in re.finditer(r"\bstatic\s+const\s+u16\s+(\w+)\s*\[\s*\]\s*=\s*\{", text):
        open_pos = m.end() - 1
        parts = top_level_split(text[open_pos + 1 : matching_brace(text, open_pos, where)])
        w = f"{where}: {m.group(1)}"
        if not parts or parts[-1] != "LEVEL_UP_END":
            fail(f"{w}: does not end with LEVEL_UP_END")
        entries = []
        for part in parts[:-1]:
            mm = re.fullmatch(r"LEVEL_UP_MOVE\(\s*(\d+)\s*,\s*(MOVE_\w+)\s*\)", part)
            if not mm:
                fail(f"{w}: unexpected entry {part[:40]!r}")
            level = int(mm.group(1))
            move = lookup(move_ids, mm.group(2), w)
            if not 1 <= level <= MAX_LEVEL or not 1 <= move < MOVE_COUNT:
                fail(f"{w}: entry {part!r} out of range")
            entries.append((level, move))
        learnset_arrays[m.group(1)] = entries
    where = "src/data/pokemon/level_up_learnset_pointers.h"
    body = array_body(read_source(clone, where), "gLevelUpLearnsets", where)
    learnsets = {}
    for name, value in designated(body, "SPECIES_", where):
        n = national_of(name, where)
        if n == 0:
            continue
        if n in learnsets:
            fail(f"{where}: {name} repeated")
        learnsets[n] = lookup(learnset_arrays, value, where)
    for n in range(1, NATIONAL_COUNT + 1):
        if n not in learnsets:
            fail(f"{where}: no learnset for national {n}")
        if not learnsets[n] or learnsets[n][0][0] != 1:
            fail(f"{where}: national {n} has no level-1 move first")
    total = sum(len(learnsets[n]) for n in range(1, NATIONAL_COUNT + 1))
    if total != LEARNSET_COUNT:
        fail(f"expected {LEARNSET_COUNT} learnset entries, got {total}")
    largest = max(len(learnsets[n]) for n in range(1, NATIONAL_COUNT + 1))
    if largest != LEARNSET_MAX:
        fail(f"expected at most {LEARNSET_MAX} learnset entries per species, got {largest}")
    d["learnsets"] = learnsets

    # Items.
    where = "src/data/items.h"
    body = array_body(read_source(clone, where), "gItems", where)
    items = {}
    holdable = []
    enigma_berry = lookup(item_ids, "ITEM_ENIGMA_BERRY", where)
    # Held mail needs a mail record; the editor sets the mail index to 0xFF, and READ/TAKE on
    # mail[0xFF] writes out of bounds in Ruby and Emerald. So mail is never offered as held.
    mail = mail_items(clone, item_ids)
    for name, value in designated(body, "ITEM_", where):
        w = f"{where}: {name}"
        f = struct_fields(value, w)
        item = item_ids.get(name)
        if item is None:
            item = lookup(item_ids, field(f, "itemId", w), w)
        if item in items:
            fail(f"{w}: item {item} repeated")
        raw = game_string(field(f, "name", w), w)
        item_name = "" if raw == UNUSED_ITEM_NAME else ascii_name(raw, "item", w)
        items[item] = item_name
        importance = number(f["importance"], w) if "importance" in f else 0
        if (
            raw != UNUSED_ITEM_NAME
            and importance != 1
            and f.get("pocket") != "POCKET_KEY_ITEMS"
            and item != enigma_berry
            and item not in mail
        ):
            holdable.append(item)
    exact_ids(items, ITEM_COUNT, "items")
    for item in sorted(mail):
        if not items.get(item):
            fail(f"src/mail_data.c: mail item {item} has no name in {where}")
    if len(holdable) != HOLDABLE_COUNT:
        fail(f"expected {HOLDABLE_COUNT} holdable items, got {len(holdable)}")
    d["items"] = items
    d["holdable"] = sorted(holdable, key=lambda i: (name_bytes(items[i]), i))
    d["move_by_name"] = sorted(range(1, MOVE_COUNT), key=lambda m: (name_bytes(move_names[m]), m))

    # Abilities.
    where = "src/data/text/abilities.h"
    body = array_body(read_source(clone, where), "gAbilityNames", where)
    abilities = {}
    ability_label = {}
    for name, value in designated(body, "ABILITY_", where):
        ability = lookup(ability_ids, name, where)
        if ability in abilities:
            fail(f"{where}: {name} repeated")
        abilities[ability] = ascii_name(game_string(value, where), "ability", f"{where}: {name}")
        ability_label[ability] = name[len("ABILITY_") :]
    exact_ids(abilities, ABILITY_COUNT, "abilities")
    d["abilities"] = abilities
    d["ability_label"] = ability_label

    # Natures.
    where = "src/data/text/nature_names.h"
    text = read_source(clone, where)
    nature_strings = {
        m.group(1): m.group(2)
        for m in re.finditer(r"\b(s\w+NatureName)\s*\[\s*\]\s*=\s*(_\(\s*\"[^\"]*\"\s*\))", text)
    }
    body = array_body(text, "gNatureNamePointers", where)
    natures = {}
    nature_label = {}
    for name, value in designated(body, "NATURE_", where):
        nature = lookup(nature_ids, name, where)
        if nature in natures:
            fail(f"{where}: {name} repeated")
        raw = game_string(lookup(nature_strings, value, where), where)
        natures[nature] = ascii_name(raw, "nature", f"{where}: {name}")
        nature_label[nature] = name[len("NATURE_") :]
    exact_ids(natures, NATURE_COUNT, "natures")
    d["natures"] = natures
    d["nature_label"] = nature_label

    # Types.
    where = "src/battle_main.c"
    body = array_body(read_source(clone, where), "gTypeNames", where)
    types = {}
    type_label = {}
    for name, value in designated(body, "TYPE_", where):
        t = lookup(type_ids, name, where)
        if t in types:
            fail(f"{where}: {name} repeated")
        types[t] = ascii_name(game_string(value, where), "type", f"{where}: {name}")
        type_label[t] = name[len("TYPE_") :]
    exact_ids(types, TYPE_COUNT, "types")
    d["types"] = types
    d["type_label"] = type_label
    return d


# ---------------------------------------------------------------------------
# Output


def render(d):
    out = [
        "/* Generated by tools/gen3_tables.py from pret/pokeemerald "
        f"{PINNED_COMMIT}. Do not edit. Game facts only. */",
        "#include <src/gen3/gen3_data.h>",
        "",
        "/* Species, move and item names, the name-sorted move and holdable lists and learnsets",
        " * are in files/gen3_bulk.bin, written by the same run. */",
        f'_Static_assert(GEN3_LEARNSET_COUNT == {LEARNSET_COUNT}, "rerun tools/gen3_tables.py");',
        f'_Static_assert(GEN3_LEARNSET_MAX == {LEARNSET_MAX}, "rerun tools/gen3_tables.py");',
        f'_Static_assert(GEN3_HOLDABLE_COUNT == {HOLDABLE_COUNT}, "rerun tools/gen3_tables.py");',
    ]

    def table(decl, rows):
        out.append("")
        out.append(f"{decl} = {{")
        out.extend(f"    {value}, /* {comment} */" for value, comment in rows)
        out.append("};")

    labels = d["national_label"]
    rows = [("{{0, 0, 0, 0, 0, 0}, {0, 0}, 0, 0, {0, 0}, 0, 0}", "0 NONE")]
    for n in range(1, NATIONAL_COUNT + 1):
        base, types, ratio, growth, abilities, friendship = d["info"][n]
        value = "{{%s}, {%d, %d}, %d, %d, {%d, %d}, %d, %d}" % (
            ", ".join(str(b) for b in base),
            types[0],
            types[1],
            ratio,
            growth,
            abilities[0],
            abilities[1],
            friendship,
            d["national_to_internal"][n],
        )
        rows.append((value, f"{n} {labels[n]}"))
    table("const Gen3SpeciesInfo gen3_species_info_tab[GEN3_NATIONAL_COUNT + 1]", rows)

    rows = [
        (str(d["internal_to_national"][i]), f"{i} {d['internal_label'][i]}")
        for i in range(INTERNAL_COUNT)
    ]
    table("const uint16_t gen3_internal_to_national_tab[GEN3_INTERNAL_COUNT]", rows)

    move_label = d["move_label"]
    rows = [(str(d["move_pp"][m]), f"{m} {move_label[m]}") for m in range(MOVE_COUNT)]
    table("const uint8_t gen3_move_pp_tab[GEN3_MOVE_COUNT]", rows)

    # Bit (item & 7) of byte (item >> 3) is set for a holdable item.
    bitmap = bytearray((ITEM_COUNT + 7) // 8)
    for item in d["holdable"]:
        bitmap[item >> 3] |= 1 << (item & 7)
    if sum(bin(b).count("1") for b in bitmap) != HOLDABLE_COUNT:
        fail(f"holdable bitmap does not have {HOLDABLE_COUNT} bits set")
    rows = [(f"0x{b:02X}", f"items {8 * k}..{8 * k + 7}") for k, b in enumerate(bitmap)]
    table("const uint8_t gen3_item_holdable_tab[(GEN3_ITEM_COUNT + 7) / 8]", rows)

    small_lists = (
        ("gen3_ability_name_tab[GEN3_ABILITY_COUNT][13]", "abilities", "ability_label"),
        ("gen3_nature_name_tab[GEN3_NATURE_COUNT][8]", "natures", "nature_label"),
        ("gen3_type_name_tab[GEN3_TYPE_COUNT][7]", "types", "type_label"),
    )
    for decl, names_key, label_key in small_lists:
        names, label = d[names_key], d[label_key]
        rows = [(c_string(names[i]), f"{i} {label[i]}") for i in range(len(names))]
        table("const char " + decl, rows)

    return "\n".join(out) + "\n"


def u16_block(values, count, what):
    if len(values) != count:
        fail(f"gen3_bulk.bin: expected {count} {what}, got {len(values)}")
    for v in values:
        if not 0 <= v <= 0xFFFF:
            fail(f"gen3_bulk.bin: {what} value {v} does not fit in 16 bits")
    return struct.pack(f"<{count}H", *values)


def name_block(names, size, what):
    """NUL-padded names of size bytes each; every name must leave room for a NUL."""
    out = bytearray()
    for name in names:
        raw = name_bytes(name)
        if len(raw) >= size or b"\0" in raw:
            fail(f"gen3_bulk.bin: {what} name {raw!r} does not fit in {size} bytes")
        out += raw.ljust(size, b"\0")
    return bytes(out)


def id_block(ids, count, valid, what):
    """count distinct ids, each in valid, as u16s."""
    if len(ids) != count or len(set(ids)) != count or any(i not in valid for i in ids):
        fail(f"gen3_bulk.bin: {what} are not {count} distinct ids in {valid}")
    return u16_block(ids, count, what)


def render_bulk(d):
    """files/gen3_bulk.bin, in the layout of the module docstring."""
    species_names = [d["species_names"][n] if n else "" for n in range(NATIONAL_COUNT + 1)]
    starts = [0, 0]
    entries = []
    for n in range(1, NATIONAL_COUNT + 1):
        entries += [(level << 9) | move for level, move in d["learnsets"][n]]
        starts.append(len(entries))
    payload = (
        name_block(species_names, BULK_NAME_SIZE, "species")
        + u16_block(starts, NATIONAL_COUNT + 2, "learnset starts")
        + u16_block(entries, LEARNSET_COUNT, "learnset entries")
        + name_block([d["move_names"][m] for m in range(MOVE_COUNT)], BULK_MOVE_NAME_SIZE, "move")
        + id_block(d["move_by_name"], MOVE_COUNT - 1, range(1, MOVE_COUNT), "moves by name")
        + name_block([d["items"][i] for i in range(ITEM_COUNT)], BULK_ITEM_NAME_SIZE, "item")
        + id_block(d["holdable"], HOLDABLE_COUNT, range(1, ITEM_COUNT), "holdable items")
    )
    if len(payload) != BULK_PAYLOAD_SIZE:
        fail(f"gen3_bulk.bin: payload is {len(payload)} bytes, expected {BULK_PAYLOAD_SIZE}")
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    header = BULK_MAGIC + struct.pack(
        "<HHII", BULK_VERSION, NATIONAL_COUNT + 1, LEARNSET_COUNT, crc
    )
    if len(header) != BULK_HEADER_SIZE:
        fail(f"gen3_bulk.bin: header is {len(header)} bytes")
    return header + payload


def main(argv):
    if len(argv) != 2:
        print("usage: python3 -I tools/gen3_tables.py <pokeemerald>", file=sys.stderr)
        return 2
    clone = argv[1]
    try:
        check_clone(clone)
        d = extract(clone)
        text = render(d)
        bulk = render_bulk(d)
    except GenError as e:
        print(f"gen3_tables.py: error: {e}", file=sys.stderr)
        return 1
    with open(OUTPUT, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    print(f"wrote {os.path.relpath(OUTPUT, REPO_ROOT)}")
    with open(BULK_OUTPUT, "wb") as f:
        f.write(bulk)
    print(f"wrote {os.path.relpath(BULK_OUTPUT, REPO_ROOT)} ({len(bulk)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
