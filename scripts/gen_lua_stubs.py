#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Generates the LuaLS (EmmyLua) stub for the CrossPoint Lua API from the LDoc-style
annotations above each binding in src/util/LuaManager.cpp.

Annotations live in C++ comments; the leading `//` is stripped before parsing:

    // --- Draws an unfilled rectangle.
    // -- @param x int Left edge
    // -- @param color[opt=COLOR_BLACK] int COLOR_* constant
    // -- @return bool ok
    // -- @within gui
    int guiDrawRect(lua_State* state) { ... }

Type words map to LuaLS types: int -> integer, bool -> boolean, anything else passes through.
`@within <table>` groups the function into its namespace; `@global` emits a bare global.

Usage:
    uv run scripts/gen_lua_stubs.py                 # writes data/lua/crosspoint.lua
    uv run scripts/gen_lua_stubs.py --check         # exit 1 if the stub is out of date (CI)
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE = REPO_ROOT / "src" / "util" / "LuaManager.cpp"
OUTPUT = REPO_ROOT / "data" / "lua" / "crosspoint.lua"

FUNCTION_RE = re.compile(r"^int (\w+)\(lua_State\*(?: state)?\)")
ADD_FUNCTION_RE = re.compile(r'addFunction\(state, "(\w+)", (\w+)\)')
PUSH_INT_RE = re.compile(r"lua_pushinteger")
SET_GLOBAL_RE = re.compile(r'lua_setglobal\(state, "(\w+)"\)')
PUSH_LITERAL_RE = re.compile(r'lua_pushliteral\(state, "(.*)"\)')
SET_FIELD_RE = re.compile(r'lua_setfield\(state, -2, "(\w+)"\)')
PARAM_RE = re.compile(r"param (\w+)(?:\[opt(?:=([^\]]*))?\])? (\S+)(?: (.*))?")
RETURN_RE = re.compile(r"return (\S+?)(\|nil)?(\s+\S.*)?$", re.ASCII)
TYPE_MAP = {"int": "integer", "bool": "boolean"}


@dataclass
class Param:
    name: str
    optional: bool
    default: str | None
    lua_type: str
    desc: str


@dataclass
class Binding:
    c_name: str
    namespace: str  # "" = global
    description: list[str] = field(default_factory=list)
    params: list[Param] = field(default_factory=list)
    returns: list[tuple[str, str, bool]] = field(default_factory=list)  # (type, desc, nullable)


def parse_annotations(text: str) -> tuple[dict[str, Binding], dict[str, list[str]]]:
    bindings: dict[str, Binding] = {}
    aliases: dict[str, list[str]] = {}
    description: list[str] = []
    params: list[Param] = []
    returns: list[tuple[str, str, bool]] = []
    pending_alias: list[str] = []
    namespace = ""

    def flush() -> None:
        nonlocal description, params, returns, namespace, pending_alias
        description, params, returns, namespace, pending_alias = [], [], [], "", []

    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("//"):
            stripped = stripped[2:].strip()
        if stripped.startswith("---"):
            description.append(stripped[3:].strip())
            continue
        if stripped.startswith("-- @"):
            tag = stripped[4:].strip()
            if tag.startswith("param "):
                match = PARAM_RE.match(tag)
                if not match:
                    raise ValueError(f"Malformed @param: {line}")
                name, default, raw_type, desc = match.groups()
                optional = default is not None or "[opt]" in tag
                params.append(Param(name, optional, default, TYPE_MAP.get(raw_type, raw_type), (desc or "").strip()))
            elif tag.startswith("return "):
                match = RETURN_RE.match(tag)
                if not match:
                    raise ValueError(f"Malformed @return: {line}")
                raw_type, nullable_marker, desc = match.groups()
                returns.append((TYPE_MAP.get(raw_type, raw_type), (desc or "").strip(), nullable_marker is not None))
            elif tag.startswith("within "):
                namespace = tag[len("within "):].strip()
            elif tag == "global":
                namespace = ""
            elif tag.startswith("alias "):
                alias_name = tag[len("alias "):].strip()
                pending_alias.append(alias_name)
            else:
                raise ValueError(f"Unknown annotation tag: {line}")
            continue

        match = FUNCTION_RE.match(line)
        if match and description:
            c_name = match.group(1)
            if c_name in bindings:
                raise ValueError(f"Duplicate annotation for {c_name}")
            bindings[c_name] = Binding(c_name, namespace, description, params, returns)
            if pending_alias:
                aliases[c_name] = pending_alias
        flush()

    return bindings, aliases


def parse_registration(text: str) -> tuple[dict[str, str], dict[str, str], dict[str, list[tuple[str, str]]]]:
    """Returns ({c_name: lua_name}, {global: kind}, {table: [(name, literal)]}) from registerBindings()."""
    match = re.search(r"void LuaManager::registerBindings\(\) \{(.*?)\n\}", text, re.DOTALL)
    if not match:
        raise ValueError("registerBindings() not found")
    body = match.group(1)

    lua_names: dict[str, str] = {}
    registrations: list[tuple[str, str]] = []
    namespaces: dict[str, str] = {}
    table_fields: dict[str, list[tuple[str, str]]] = {}
    pending_literal: str | None = None
    pending_integer = False
    for line in body.splitlines():
        if match := ADD_FUNCTION_RE.search(line):
            lua_names[match.group(2)] = match.group(1)
            registrations.append((match.group(2), match.group(1)))
        elif match := PUSH_LITERAL_RE.search(line):
            pending_literal = match.group(1)
        elif match := SET_FIELD_RE.search(line):
            if pending_literal is not None:
                table_fields.setdefault("gui", []).append((match.group(1), pending_literal))
                pending_literal = None
        elif match := SET_GLOBAL_RE.search(line):
            namespaces[match.group(1)] = "integer" if pending_integer else "table"
        if PUSH_INT_RE.search(line):
            pending_integer = True
        elif not PUSH_LITERAL_RE.search(line):
            pending_integer = False
    return lua_names, registrations, namespaces, table_fields


def to_lua_type(lua_type: str, nullable: bool) -> str:
    return lua_type + ("?" if nullable else "")


def render_function(lua_name: str, binding: Binding) -> list[str]:
    lines = [f"--- {line}" if line else "---" for line in binding.description]
    for param in binding.params:
        lua_name_part = param.name + ("?" if param.optional else "")
        annotation = f"---@param {lua_name_part} {param.lua_type}"
        if param.desc:
            annotation += f" {param.desc}"
        if param.optional:
            default = param.default
            if default is not None and param.lua_type != "string":
                annotation += f" (default {default})"
            elif default is not None and default != '""':
                annotation += f' (default {default})'
        lines.append(annotation)
    for lua_type, desc, nullable in binding.returns:
        annotation = f"---@return {to_lua_type(lua_type, nullable)}"
        if desc:
            annotation += f" {desc}"
        lines.append(annotation)

    signature = ", ".join(param.name for param in binding.params)
    if binding.namespace:
        lines.append(f"function {binding.namespace}.{lua_name}({signature}) end")
    else:
        lines.append(f"function {lua_name}({signature}) end")
    return lines


def render(bindings: dict[str, Binding], aliases: dict[str, list[str]], registrations: list[tuple[str, str]],
           namespaces: dict[str, str], table_fields: dict[str, list[tuple[str, str]]]) -> str:
    alias_names = {alias for alias_list in aliases.values() for alias in alias_list}
    missing = sorted({c for c, _ in registrations} - set(bindings))
    if missing:
        raise ValueError(f"Registered bindings missing annotations: {', '.join(missing)}")
    registered_names = {lua for _, lua in registrations}
    undeclared = sorted(alias_names - registered_names)
    if undeclared:
        raise ValueError(f"@alias names not registered via addFunction: {', '.join(undeclared)}")

    out = [
        "---@meta",
        "",
        "-- CrossPoint Lua API stub for LuaLS. GENERATED by scripts/gen_lua_stubs.py",
        "-- from the annotations in src/util/LuaManager.cpp; do not edit by hand.",
        "",
        "---@alias CrossPointButton",
        '---| "back"',
        '---| "confirm"',
        '---| "left"',
        '---| "right"',
        '---| "up"',
        '---| "down"',
        '---| "page_back"',
        '---| "page_forward"',
        "",
        "---@alias CrossPointOrientation",
        '---| "portrait"',
        '---| "portrait_inv"',
        '---| "landscape_cw"',
        '---| "landscape_ccw"',
        "",
    ]

    table_names = sorted(name for name, kind in namespaces.items() if kind == "table")
    for name in table_names:
        out.append(f"---@class {name}")
        for field_name, literal in table_fields.get(name, []):
            out.append(f"---@field {field_name} string")
        out.append(f"{name} = {{}}")
        for field_name, literal in table_fields.get(name, []):
            out.append(f'{name}.{field_name} = "{literal}"')
        out.append("")

    constant_names = sorted(name for name, kind in namespaces.items() if kind == "integer")
    for name in constant_names:
        out.append(f"---@type integer")
        out.append(f"{name} = 0")
    if constant_names:
        out.append("")

    ordered = sorted(
        (
            (lua_name, bindings[c_name])
            for c_name, lua_name in registrations
            if c_name in bindings and lua_name not in alias_names
        ),
        key=lambda item: (item[1].namespace, item[0]),
    )
    for c_name, alias_list in aliases.items():
        if c_name in bindings:
            for alias_name in alias_list:
                ordered.append((alias_name, bindings[c_name]))
    ordered.sort(key=lambda item: (item[1].namespace, item[0]))
    current_namespace = None
    for lua_name, binding in ordered:
        if binding.namespace != current_namespace:
            current_namespace = binding.namespace
            out.append(f"-- {current_namespace or 'globals'}")
            out.append("")
        out.extend(render_function(lua_name, binding))
        out.append("")

    out.append(
        "-- Callbacks a runtime app may define: draw(), on_tick(), on_button(name, state), on_timer(id)."
    )
    return "\n".join(out).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if the generated stub differs from %s" % OUTPUT)
    args = parser.parse_args()

    text = SOURCE.read_text()
    bindings, aliases = parse_annotations(text)
    lua_names, registrations, namespaces, table_fields = parse_registration(text)
    stub = render(bindings, aliases, registrations, namespaces, table_fields)

    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != stub:
            print(f"ERROR: {OUTPUT} is out of date; run `uv run scripts/gen_lua_stubs.py`", file=sys.stderr)
            return 1
        print(f"OK: {OUTPUT} is up to date")
        return 0

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(stub)
    print(f"Wrote {OUTPUT} ({len(bindings)} annotated bindings, {len(registrations)} registered functions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
