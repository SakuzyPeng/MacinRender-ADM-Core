#!/usr/bin/env python3
"""Validate GUI localization dictionaries and visible XAML text."""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
GUI_ROOT = REPO_ROOT / "gui" / "MacinRender.Gui"
LOCALIZER_PATH = GUI_ROOT / "I18n" / "Localizer.cs"

DICTIONARY_PATTERN = re.compile(
    r"private static readonly Dictionary<string, string> (?P<lang>Zh|En) = new\(\)\s*"
    r"\{(?P<body>.*?)^\s*\};",
    re.DOTALL | re.MULTILINE,
)
ENTRY_PATTERN = re.compile(
    r'^\s*\["(?P<key>[^"]+)"\]\s*=\s*"(?P<value>(?:\\.|[^"\\])*)",',
    re.MULTILINE,
)
PLACEHOLDER_PATTERN = re.compile(r"(?<!\{)\{(\d+)(?:[^{}]*)\}(?!\})")
VISIBLE_ATTRIBUTE_PATTERN = re.compile(
    r'\b(?P<attribute>Text|Content|Header|Title|ToolTip\.Tip|Watermark)="(?P<value>[^"]*)"'
)
C_SHARP_STRING_PATTERN = re.compile(r'"((?:\\.|[^"\\])*)"')
KEY_PREFIXES = (
    "About",
    "Detail",
    "Error",
    "Log",
    "Nav",
    "Pick",
    "QueueStatus",
    "Sem",
    "Stage",
    "Status",
    "Tip",
)

# Domain labels, channel abbreviations, and symbols are intentionally language-neutral.
ALLOWED_LITERAL_UI = {
    "",
    "Depth",
    "Height",
    "L",
    "R",
    "Width",
    "diffuse",
    "divergence",
    "gain",
    "×",
    "⚠",
}


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def parse_dictionaries(errors: list[str]) -> dict[str, dict[str, str]]:
    text = LOCALIZER_PATH.read_text(encoding="utf-8")
    dictionaries: dict[str, dict[str, str]] = {}
    for match in DICTIONARY_PATTERN.finditer(text):
        lang = match.group("lang")
        entries = [(entry.group("key"), entry.group("value")) for entry in ENTRY_PATTERN.finditer(match.group("body"))]
        counts = Counter(key for key, _ in entries)
        duplicates = sorted(key for key, count in counts.items() if count > 1)
        if duplicates:
            fail(errors, f"{LOCALIZER_PATH.relative_to(REPO_ROOT)}: duplicate {lang} keys: {', '.join(duplicates)}")
        dictionaries[lang] = dict(entries)

    if set(dictionaries) != {"Zh", "En"}:
        fail(errors, f"{LOCALIZER_PATH.relative_to(REPO_ROOT)}: could not parse both Zh and En dictionaries")
    return dictionaries


def check_dictionary_parity(dictionaries: dict[str, dict[str, str]], errors: list[str]) -> set[str]:
    if set(dictionaries) != {"Zh", "En"}:
        return set()

    zh = dictionaries["Zh"]
    en = dictionaries["En"]
    zh_only = sorted(set(zh) - set(en))
    en_only = sorted(set(en) - set(zh))
    if zh_only:
        fail(errors, f"keys missing from En: {', '.join(zh_only)}")
    if en_only:
        fail(errors, f"keys missing from Zh: {', '.join(en_only)}")

    for key in sorted(set(zh) & set(en)):
        zh_args = sorted(PLACEHOLDER_PATTERN.findall(zh[key]))
        en_args = sorted(PLACEHOLDER_PATTERN.findall(en[key]))
        if zh_args != en_args:
            fail(errors, f"{key}: placeholder mismatch (Zh={zh_args}, En={en_args})")
    return set(zh) & set(en)


def check_xaml(keys: set[str], errors: list[str]) -> None:
    for path in sorted(GUI_ROOT.rglob("*.axaml")):
        text = re.sub(r"<!--.*?-->", "", path.read_text(encoding="utf-8"), flags=re.DOTALL)
        for line_number, line in enumerate(text.splitlines(), start=1):
            for match in VISIBLE_ATTRIBUTE_PATTERN.finditer(line):
                value = match.group("value")
                dynamic = re.fullmatch(r"\{DynamicResource\s+([^}]+)\}", value)
                if dynamic:
                    key = dynamic.group(1)
                    if key not in keys:
                        fail(errors, f"{path.relative_to(REPO_ROOT)}:{line_number}: unknown i18n key {key}")
                    continue
                if value.startswith("{"):
                    continue
                if value.strip() not in ALLOWED_LITERAL_UI:
                    fail(
                        errors,
                        f"{path.relative_to(REPO_ROOT)}:{line_number}: visible {match.group('attribute')} "
                        f"must use DynamicResource: {value!r}",
                    )


def check_csharp_key_references(keys: set[str], errors: list[str]) -> None:
    key_identifier = re.compile(r"^[A-Za-z][A-Za-z0-9]*$")
    for path in sorted(GUI_ROOT.rglob("*.cs")):
        if path == LOCALIZER_PATH:
            continue
        text = path.read_text(encoding="utf-8")
        for match in C_SHARP_STRING_PATTERN.finditer(text):
            value = match.group(1)
            probable_key = any(
                value.startswith(prefix) and len(value) > len(prefix) and value[len(prefix)].isupper()
                for prefix in KEY_PREFIXES
            )
            if not key_identifier.fullmatch(value) or not probable_key:
                continue
            if value not in keys:
                line_number = text.count("\n", 0, match.start()) + 1
                fail(errors, f"{path.relative_to(REPO_ROOT)}:{line_number}: unknown probable i18n key {value}")


def main() -> int:
    errors: list[str] = []
    dictionaries = parse_dictionaries(errors)
    keys = check_dictionary_parity(dictionaries, errors)
    check_xaml(keys, errors)
    check_csharp_key_references(keys, errors)
    if errors:
        for error in errors:
            print(f"gui i18n error: {error}", file=sys.stderr)
        return 1

    print(f"GUI localization is valid: {len(keys)} paired keys, no unapproved visible XAML literals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
