#!/usr/bin/env python3
"""Check the hand-written private FFI headers against the Rust `mradm-ffi` exports, and the public
`c_api.h` against the exports `mradm-capi` implements.

cbindgen renders the Rust side into a throwaway C header; both headers are then reduced to
function signatures and struct field sequences and compared structurally. The generated header is
never included by C++ code. Opaque handles are `void*` on the C++ side and any struct pointer on
the Rust side; everything else must match in base type, pointer depth and constness. Typedefs of
scalars, enums (`int`-sized, as `c_api.h` asserts) and callbacks are compared by what they name.

`c_api.h` is checked one way only: every Rust `adm_*` export must be declared there with the same
signature, and the functions still defined in `src/adm_c_api/adm_c_api.cpp` are left to
`check-capi-exports.py`, which checks that the shared library exports all of them.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import shutil
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
RUST_ROOT = REPO_ROOT / "rust"
FFI_CRATE = RUST_ROOT / "crates" / "mradm-ffi"
CBINDGEN_CONFIG = FFI_CRATE / "cbindgen.toml"
CAPI_CRATE = RUST_ROOT / "crates" / "mradm-capi"
CAPI_HEADER = "include/adm/c_api.h"
# The private entries of mradm-capi that the exports still in C++ call (types from c_api.h).
CAPI_FFI_HEADERS = ["src/adm_c_api/capi_ffi.h"]
CBINDGEN_VERSION = "0.29.2"

FFI_HEADERS = [
    "tests/unit/ear_ffi.h",
    "src/adm_dsp/dsp_ffi.h",
    "src/adm_dsp/decimal_ffi.h",
    "src/adm_dsp/timeline_ffi.h",
    "tests/unit/hoa_ffi.h",
    "tests/support/binaural/live_binaural_ffi.h",
    "src/adm_dsp/room_ffi.h",
    "tests/unit/live_vbap_ffi.h",
    "tests/support/dsp/monitor_ffi.h",
    "src/adm_dsp/scene_math_ffi.h",
    "tests/support/dsp/scene_transition_ffi.h",
    "tests/support/post_gain/post_gain_ffi.h",
    "src/adm_dsp/stream_dsp_ffi.h",
    "src/adm_dsp/semantic_policy_ffi.h",
    "tests/support/triple_balance/triple_balance_ffi.h",
    "src/adm_render_common/layout_ffi.h",
    "src/adm_metadata/adm_ffi.h",
    "src/adm_metadata/scene_ffi.h",
    "src/adm_audio/wav_ffi.h",
    "src/adm_realtime/osc_ffi.h",
    "src/adm_core/config_ffi.h",
    "src/adm_render_common/routing_ffi.h",
    "tests/support/render_test_ffi.h",
    "src/adm_render_common/engine_ffi.h",
    "src/adm_apple/apple_plan_ffi.h",
]

# (function, zero-based parameter index, Rust spelling, C spelling) -> required array element/length.
# These pointer spellings are compatible only while the underlying array keeps this exact shape.
KNOWN_PARAMETER_DIFFERENCES = {
    # Rust takes `*mut [f32; 4]` (one filtered frame); C++ passes the first element of float[4].
    ("mradm_dsp_tb_filter_process", 3, "FilteredFrame*", "f32*"): ("f32", 4),
}

SCALARS = {
    "void": "void",
    "bool": "bool",
    "_Bool": "bool",
    "float": "f32",
    "double": "f64",
    "char": "u8",
    "int8_t": "i8",
    "uint8_t": "u8",
    "int16_t": "i16",
    "uint16_t": "u16",
    "int": "i32",
    "int32_t": "i32",
    "uint32_t": "u32",
    "int64_t": "i64",
    "uint64_t": "u64",
    "size_t": "usize",
    "uintptr_t": "usize",
    "intptr_t": "isize",
}


@dataclass(frozen=True)
class CType:
    base: str  # canonical scalar name or struct name
    const: bool  # constness of the base value
    # Constness of each `*` from left to right; the last one is the top-level qualifier, which is not
    # part of the ABI and is dropped.
    pointers: tuple[bool, ...]
    # A function pointer: (return type, parameter types); base is "fn" and pointers is (False,).
    signature: tuple["CType", tuple["CType", ...]] | None = None

    def __str__(self) -> str:
        if self.signature is not None:
            ret, params = self.signature
            return f"{ret} (*)({', '.join(str(p) for p in params)})"
        text = ("const " if self.const else "") + self.base
        for level_const in self.pointers:
            text += "*" + (" const" if level_const else "")
        return text

    @property
    def depth(self) -> int:
        return len(self.pointers)


@dataclass
class Header:
    functions: dict[str, tuple[CType, list[CType], str]]
    structs: dict[str, list[tuple[CType, tuple[int, ...], str]] | None]  # None = opaque
    arrays: dict[str, tuple[str, int]]  # typedef T name[N]
    origin: dict[str, str]
    aliases: dict[str, CType] = None  # typedef of a scalar, an enum or a callback

    def __post_init__(self) -> None:
        if self.aliases is None:
            self.aliases = {}


COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
PREPROCESSOR = re.compile(r"^\s*#.*$", re.M)
EXTERN_OPEN = re.compile(r'extern\s+"C"\s*\{')
STRUCT_BODY = re.compile(r"(typedef\s+)?struct\s+(\w+)\s*\{(.*?)\}\s*(\w*)\s*;", re.S)
OPAQUE = re.compile(r"typedef\s+struct\s+(\w+)\s+(\w+)\s*;")
ARRAY_TYPEDEF = re.compile(r"typedef\s+(\w+)\s+(\w+)\s*\[(\d+)\]\s*;")
FUNCTION_POINTER = re.compile(r"^(.*?)\(\s*\*\s*(\w*)\s*\)\s*\((.*)\)$", re.S)
ENUM_TYPEDEF = re.compile(r"typedef\s+enum\s*\w*\s*\{[^{}]*\}\s*(\w+)\s*;", re.S)
CALLBACK_TYPEDEF = re.compile(r"typedef\s+([\w\s\*]+?)\(\s*\*\s*(\w+)\s*\)\s*\(([^()]*)\)\s*;", re.S)
SCALAR_TYPEDEF = re.compile(r"typedef\s+(\w+)\s+(\w+)\s*;")
# The public header's attribute macros.
PUBLIC_MACROS = re.compile(r'ADM_API_DEPRECATED\s*\(\s*"(?:[^"\\]|\\.)*"\s*\)|\bADM_API_NOEXCEPT\b')


def function_pattern(prefix: str) -> re.Pattern[str]:
    return re.compile(r"([\w\s\*]+?)\b(" + re.escape(prefix) + r"\w+)\s*\(([^;{}]*)\)\s*;", re.S)


def strip(text: str) -> str:
    text = COMMENT.sub(" ", text)
    text = PUBLIC_MACROS.sub(" ", text)
    text = PREPROCESSOR.sub(" ", text)
    text = EXTERN_OPEN.sub(" ", text)
    return text.replace("std::", "")


def split_parameters(text: str) -> list[str]:
    """Split at commas outside callback parameter lists."""
    parameters = []
    depth = 0
    start = 0
    for index, char in enumerate(text):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth < 0:
                raise ValueError(f"unbalanced parameter list: {text!r}")
        elif char == "," and depth == 0:
            parameters.append(text[start:index])
            start = index + 1
    if depth:
        raise ValueError(f"unbalanced parameter list: {text!r}")
    parameters.append(text[start:])
    return parameters


def parse_parameters(text: str, known: set[str]) -> tuple[CType, ...]:
    text = text.strip()
    if not text or text == "void":
        return ()
    parameters = []
    for declaration in split_parameters(text):
        parsed = parse_function_pointer(declaration, known)
        if parsed is None:
            parsed = parse_type(declaration, known)
        parameters.append(parsed[0])
    return tuple(parameters)


def parse_function_pointer(text: str, known: set[str]) -> tuple[CType, str] | None:
    """`RET (*name)(PARAMS)`: a callback field or parameter (optionally unnamed)."""
    match = FUNCTION_POINTER.match(text.strip())
    if match is None:
        return None
    ret, _ = parse_type(match.group(1), known)
    return CType("fn", False, (False,), (ret, parse_parameters(match.group(3), known))), match.group(2)


def parse_type(text: str, known: set[str]) -> tuple[CType, str]:
    """Split one declaration into its type and declared name (empty for unnamed parameters)."""
    tokens = re.findall(r"\w+|\*", text)
    name = ""
    if tokens and tokens[-1] not in ("*", "const", "struct") and tokens[-1] not in SCALARS and tokens[-1] not in known:
        name = tokens.pop()
    tokens = [token for token in tokens if token not in ("struct", "enum")]
    base = None
    base_const = False
    pointers: list[bool] = []
    for token in tokens:
        if token == "const":
            if pointers:
                pointers[-1] = True
            else:
                base_const = True
        elif token == "*":
            pointers.append(False)
        elif token in ("unsigned", "signed", "long", "short"):
            raise ValueError(f"unsupported integer spelling: {text!r}")
        else:
            if base is not None:
                raise ValueError(f"cannot parse declaration: {text!r}")
            base = SCALARS.get(token, token)
    if base is None:
        raise ValueError(f"cannot parse declaration: {text!r}")
    if pointers:
        pointers[-1] = False
    return CType(base, base_const, tuple(pointers)), name


DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)\s+(\d+)\s*$", re.M)


RUST_CONSTANT = re.compile(r"^pub const (\w+): (?:usize|u32|u64) = (\d+);", re.M)


def rust_constants() -> dict[str, str]:
    """cbindgen does not emit constants from dependency crates, so array sizes are read from source."""
    constants: dict[str, str] = {}
    for path in sorted((RUST_ROOT / "crates").glob("*/src/**/*.rs")):
        constants.update(RUST_CONSTANT.findall(path.read_text(encoding="utf-8")))
    return constants


def resolve(ctype: CType, aliases: dict[str, CType]) -> CType:
    """Replace typedef names by what they name (a callback typedef only where used by value)."""
    if ctype.signature is not None:
        ret, params = ctype.signature
        return CType("fn", False, ctype.pointers, (resolve(ret, aliases), tuple(resolve(p, aliases) for p in params)))
    alias = aliases.get(ctype.base)
    if alias is None:
        return ctype
    if alias.signature is not None:
        return alias if not ctype.pointers else ctype
    return CType(alias.base, ctype.const or alias.const, alias.pointers + ctype.pointers)


def parse_header(
    text: str,
    origin: str,
    header: Header,
    constants: dict[str, str] | None = None,
    function_prefix: str = "mradm_",
) -> None:
    defines = dict(constants or {})
    defines.update(DEFINE.findall(text))
    text = strip(text)
    text = re.sub(r"\[(\w+)\]", lambda m: f"[{defines.get(m.group(1), m.group(1))}]", text)
    for match in ARRAY_TYPEDEF.finditer(text):
        header.arrays[match.group(2)] = (SCALARS.get(match.group(1), match.group(1)), int(match.group(3)))
    text = ARRAY_TYPEDEF.sub(" ", text)
    for match in ENUM_TYPEDEF.finditer(text):
        header.aliases[match.group(1)] = CType("i32", False, ())
        header.origin.setdefault(match.group(1), origin)
    text = ENUM_TYPEDEF.sub(" ", text)
    bodies = []
    for match in STRUCT_BODY.finditer(text):
        name = match.group(2)
        bodies.append((name, match.group(3)))
        header.structs.setdefault(name, None)
        header.origin.setdefault(name, origin)
    for match in OPAQUE.finditer(text):
        header.structs.setdefault(match.group(2), None)
        header.origin.setdefault(match.group(2), origin)
    for match in SCALAR_TYPEDEF.finditer(text):
        if match.group(1) in SCALARS:
            header.aliases[match.group(2)] = CType(SCALARS[match.group(1)], False, ())
    text = SCALAR_TYPEDEF.sub(lambda m: " " if m.group(1) in SCALARS else m.group(0), text)
    known = set(header.structs) | set(header.arrays) | set(header.aliases)
    callbacks = list(CALLBACK_TYPEDEF.finditer(text))
    known |= {match.group(2) for match in callbacks}
    for match in callbacks:
        ret, _ = parse_type(match.group(1), known)
        header.aliases[match.group(2)] = CType("fn", False, (False,), (ret, parse_parameters(match.group(3), known)))
        header.origin.setdefault(match.group(2), origin)
    text = CALLBACK_TYPEDEF.sub(" ", text)
    for name, body in bodies:
        fields = []
        for declaration in body.split(";"):
            declaration = declaration.strip()
            if not declaration:
                continue
            callback = parse_function_pointer(declaration, known)
            if callback is not None:
                fields.append((callback[0], (), callback[1]))
                continue
            head, _, rest = declaration.partition(",")
            first_type, first_name = parse_type(re.sub(r"\[\d+\]", "", head), known)
            declarators = [head] + [part.strip() for part in rest.split(",")] if rest else [head]
            prefix = head[: head.rfind(first_name)] if first_name else head
            for index, declarator in enumerate(declarators):
                full = declarator if index == 0 else prefix + declarator
                dims = tuple(int(d) for d in re.findall(r"\[(\d+)\]", full))
                ctype, field_name = parse_type(re.sub(r"\[\d+\]", "", full), known)
                fields.append((ctype, dims, field_name))
        header.structs[name] = fields
    text = STRUCT_BODY.sub(" ", text)
    text = OPAQUE.sub(" ", text)
    declared_names = set(re.findall(r"\b(" + re.escape(function_prefix) + r"\w+)\s*\(", text))
    parsed_names = set()
    for match in function_pattern(function_prefix).finditer(text):
        name = match.group(2)
        if name in header.functions:
            raise ValueError(f"{origin}: duplicate declaration of {name}")
        try:
            ret, _ = parse_type(match.group(1).strip(), known)
            params = list(parse_parameters(match.group(3), known))
        except ValueError as error:
            raise ValueError(f"{origin}: {name}: {error}") from error
        header.functions[name] = (ret, params, origin)
        header.origin[name] = origin
        parsed_names.add(name)
    unparsed = declared_names - parsed_names
    if unparsed:
        raise ValueError(f"{origin}: unsupported function declaration: {', '.join(sorted(unparsed))}")
    aliases = header.aliases
    for name, (ret, params, where) in header.functions.items():
        header.functions[name] = (resolve(ret, aliases), [resolve(p, aliases) for p in params], where)
    for name, fields in header.structs.items():
        if fields is not None:
            header.structs[name] = [(resolve(c, aliases), dims, field) for c, dims, field in fields]


def is_handle(hand: CType, generated: CType, gen: Header) -> bool:
    return hand.base == "void" and hand.depth > 0 and hand.depth == generated.depth and generated.base in gen.structs


class Checker:
    def __init__(self, hand: Header, gen: Header, partial: bool = False, all_structs: bool = True) -> None:
        self.hand = hand
        self.gen = gen
        # The public header: declared functions and structs not (yet) in Rust are not errors.
        self.partial = partial
        # Whether every hand-written struct must be used by a Rust export (not for headers that
        # take their types from c_api.h).
        self.all_structs = all_structs and not partial
        self.errors: list[str] = []
        self.pairs: dict[str, str] = {}  # hand struct -> generated struct
        self.reverse: dict[str, str] = {}

    def error(self, message: str) -> None:
        self.errors.append(message)

    def pair(self, hand_name: str, gen_name: str, where: str) -> bool:
        if self.pairs.get(hand_name, gen_name) != gen_name or self.reverse.get(gen_name, hand_name) != hand_name:
            self.error(
                f"{where}: {hand_name} 对应 Rust {gen_name}，但别处对应 "
                f"{self.pairs.get(hand_name) or self.reverse.get(gen_name)}"
            )
            return False
        self.pairs[hand_name] = gen_name
        self.reverse[gen_name] = hand_name
        return True

    def same(self, hand: CType, generated: CType, where: str) -> bool:
        if hand.signature is not None or generated.signature is not None:
            if hand.signature is None or generated.signature is None:
                return False
            (hand_ret, hand_params), (gen_ret, gen_params) = hand.signature, generated.signature
            return (
                self.same(hand_ret, gen_ret, where)
                and len(hand_params) == len(gen_params)
                and all(self.same(h, g, where) for h, g in zip(hand_params, gen_params))
            )
        if is_handle(hand, generated, self.gen):
            return hand.pointers == generated.pointers and hand.const == generated.const
        if hand.pointers != generated.pointers or hand.const != generated.const:
            return False
        hand_struct = hand.base in self.hand.structs
        gen_struct = generated.base in self.gen.structs
        if hand_struct != gen_struct:
            return False
        if hand_struct:
            hand_opaque = self.hand.structs[hand.base] is None
            gen_opaque = self.gen.structs[generated.base] is None
            if hand_opaque or gen_opaque:
                # Typed opaque handles (e.g. MradmWavReader) only need to be pointers on both sides.
                return hand.depth > 0 and hand_opaque
            return self.pair(hand.base, generated.base, where)
        return hand.base == generated.base

    def run(self) -> None:
        hand_names = set(self.hand.functions)
        gen_names = set(self.gen.functions)
        for name in sorted(gen_names - hand_names):
            self.error(f"{name}: Rust 导出了但手写 FFI 头没有声明")
        for name in sorted(hand_names - gen_names) if not self.partial else ():
            self.error(f"{name}: {self.hand.functions[name][2]} 声明了但 Rust 没有导出")
        for name in sorted(hand_names & gen_names):
            hand_ret, hand_params, origin = self.hand.functions[name]
            gen_ret, gen_params, _ = self.gen.functions[name]
            where = f"{origin}: {name}"
            if not self.same(hand_ret, gen_ret, where):
                self.error(f"{where}: 返回值 C++ `{hand_ret}` / Rust `{gen_ret}`")
            if len(hand_params) != len(gen_params):
                self.error(f"{where}: 参数个数 C++ {len(hand_params)} / Rust {len(gen_params)}")
                continue
            for index, (h, g) in enumerate(zip(hand_params, gen_params)):
                expected_array = KNOWN_PARAMETER_DIFFERENCES.get(
                    (name, index, str(g).replace(" ", ""), str(h).replace(" ", ""))
                )
                if expected_array is not None:
                    actual_array = self.gen.arrays.get(g.base)
                    if actual_array != expected_array:
                        self.error(
                            f"{where}: 第 {index + 1} 个参数的数组别名 {g.base} "
                            f"应为 {expected_array}，实际为 {actual_array}"
                        )
                    continue
                if not self.same(h, g, where):
                    self.error(f"{where}: 第 {index + 1} 个参数 C++ `{h}` / Rust `{g}`")
        checked: set[str] = set()
        while len(checked) < len(self.pairs):
            for hand_name, gen_name in list(self.pairs.items()):
                if hand_name in checked:
                    continue
                checked.add(hand_name)
                self.compare_struct(hand_name, gen_name)
        for name, fields in sorted(self.hand.structs.items()) if self.all_structs else ():
            if fields is not None and name not in self.pairs:
                self.error(f"{self.hand.origin[name]}: 结构体 {name} 没有被任何 Rust 导出签名使用")

    def compare_struct(self, hand_name: str, gen_name: str) -> None:
        hand_fields = self.hand.structs[hand_name] or []
        gen_fields = self.gen.structs[gen_name] or []
        where = f"{self.hand.origin[hand_name]}: {hand_name} ↔ Rust {gen_name}"
        if len(hand_fields) != len(gen_fields):
            self.error(
                f"{where}: 字段数 C++ {len(hand_fields)} {[f[2] for f in hand_fields]} / "
                f"Rust {len(gen_fields)} {[f[2] for f in gen_fields]}"
            )
            return
        for (h, h_dims, h_name), (g, g_dims, g_name) in zip(hand_fields, gen_fields):
            if h_dims != g_dims or not self.same(h, g, where):
                self.error(f"{where}: 字段 C++ `{h} {h_name}{list(h_dims) or ''}` / Rust `{g} {g_name}{list(g_dims) or ''}`")


def run_cbindgen(cbindgen: str, output: Path, crate: Path = FFI_CRATE) -> None:
    version = subprocess.run([cbindgen, "--version"], capture_output=True, text=True, check=True).stdout.split()[-1]
    if version != CBINDGEN_VERSION:
        sys.exit(f"需要 cbindgen {CBINDGEN_VERSION}，当前为 {version}："
                 f"cargo install cbindgen --locked --version {CBINDGEN_VERSION}")
    output.parent.mkdir(parents=True, exist_ok=True)
    config = crate / "cbindgen.toml"
    result = subprocess.run(
        [cbindgen, "--config", str(config), "--crate", crate.name, "--output", str(output), str(crate)],
        cwd=RUST_ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        sys.exit("cbindgen 失败")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cbindgen", default=shutil.which("cbindgen") or "cbindgen")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "build" / "rust" / "ffi-check" / "mradm_ffi.h")
    parser.add_argument("--generated", type=Path, help="比较已有的生成头，不运行 cbindgen")
    parser.add_argument("--generated-capi", type=Path, help="比较已有的 mradm-capi 生成头，不运行 cbindgen")
    args = parser.parse_args()

    generated = args.generated
    generated_capi = args.generated_capi
    if generated is None or generated_capi is None:
        if shutil.which(args.cbindgen) is None:
            sys.exit(f"找不到 cbindgen：cargo install cbindgen --locked --version {CBINDGEN_VERSION}")
    if generated is None:
        run_cbindgen(args.cbindgen, args.output)
        generated = args.output
    if generated_capi is None:
        generated_capi = args.output.with_name("mradm_capi.h")
        run_cbindgen(args.cbindgen, generated_capi, CAPI_CRATE)

    gen = Header({}, {}, {}, {})
    parse_header(generated.read_text(encoding="utf-8"), str(generated), gen, rust_constants())
    hand = Header({}, {}, {}, {})
    for relative in FFI_HEADERS:
        parse_header((REPO_ROOT / relative).read_text(encoding="utf-8"), relative, hand)

    checker = Checker(hand, gen)
    checker.run()

    capi_gen = Header({}, {}, {}, {})
    parse_header(generated_capi.read_text(encoding="utf-8"), str(generated_capi), capi_gen, function_prefix="adm_")
    capi_hand = Header({}, {}, {}, {})
    parse_header((REPO_ROOT / CAPI_HEADER).read_text(encoding="utf-8"), CAPI_HEADER, capi_hand, function_prefix="adm_")
    capi = Checker(capi_hand, capi_gen, partial=True)
    capi.run()

    # mradm-capi's private entries: complete in both directions, types from c_api.h.
    private_gen = Header({}, {}, {}, {})
    parse_header(generated_capi.read_text(encoding="utf-8"), str(generated_capi), private_gen, function_prefix="mradm_")
    private_hand = Header({}, {}, {}, {})
    parse_header((REPO_ROOT / CAPI_HEADER).read_text(encoding="utf-8"), CAPI_HEADER, private_hand, function_prefix="mradm_")
    for relative in CAPI_FFI_HEADERS:
        parse_header((REPO_ROOT / relative).read_text(encoding="utf-8"), relative, private_hand, function_prefix="mradm_")
    private = Checker(private_hand, private_gen, all_structs=False)
    private.run()

    errors = checker.errors + capi.errors + private.errors
    if errors:
        for message in errors:
            print(f"error: {message}", file=sys.stderr)
        print(f"FFI 头校验失败：{len(errors)} 处不一致", file=sys.stderr)
        return 1
    print(f"FFI 头校验通过：{len(hand.functions)} 个函数、{len(checker.pairs)} 个结构体与 Rust 一致；"
          f"c_api.h 的 {len(capi_hand.functions)} 个函数中 {len(capi_gen.functions)} 个由 Rust 实现、"
          f"{len(capi.pairs)} 个结构体与 Rust 一致；mradm-capi 私有入口 {len(private_hand.functions)} 个")
    return 0


if __name__ == "__main__":
    # Windows consoles default to a legacy code page; the messages are Chinese.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")
    sys.exit(main())
