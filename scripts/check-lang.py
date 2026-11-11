"""Keeps assets/lang/*.json honest.

Checks, for every locale file:
  * every key also exists in en.json (no stale keys);
  * every key appears verbatim in src/ (no key that no code can ever emit);
  * every {n} placeholder in en.json appears in the translation.

Run with: python -I scripts/check-lang.py
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANG = ROOT / "assets" / "lang"
LOCALES = ["zh_CN", "zh_TW"]

SOURCE = "\n".join(
    path.read_text(encoding="utf-8")
    for path in (ROOT / "src").rglob("*")
    if path.suffix in (".h", ".cpp")
)

failures = []

base = json.loads((LANG / "en.json").read_text(encoding="utf-8"))

for key in base:
    if key not in SOURCE:
        failures.append(f"en.json: key not used anywhere in src/: {key!r}")

for locale in LOCALES:
    table = json.loads((LANG / f"{locale}.json").read_text(encoding="utf-8"))

    for key in base.keys() - table.keys():
        failures.append(f"{locale}.json: missing key {key!r}")
    for key in table.keys() - base.keys():
        failures.append(f"{locale}.json: stale key {key!r}")

    for key, value in table.items():
        expected = set(re.findall(r"\{\d+\}", base.get(key, "")))
        actual = set(re.findall(r"\{\d+\}", value))
        if not expected <= actual:
            failures.append(f"{locale}.json: {key!r} drops placeholders {sorted(expected - actual)}")

print("\n".join(failures) if failures else "lang files OK")
raise SystemExit(1 if failures else 0)
