# SPDX-License-Identifier: GPL-2.0-or-later
"""The player's own defaults: defaults.json beside the launcher's settings.json,
{"launcher": {settings.json keys}, "ini": {bbport.ini keys}}. "Restore my defaults" lays them over
the current settings; without the file, the launcher's built-in ones. No GTK here, so that the
merge is tested without a display (tests/test_defaults.py).
"""

import json
from pathlib import Path


def load(path):
    """The file's {"launcher": {...}, "ini": {...}}, None without a file. A file that is not such
    an object raises ValueError (OSError when it cannot be read)."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    data = json.loads(text)
    if not isinstance(data, dict) or not all(isinstance(data.get(part, {}), dict)
                                             for part in ("launcher", "ini")):
        raise ValueError(f"{path}: expected {{\"launcher\": {{...}}, \"ini\": {{...}}}}")
    # bbport.ini holds text: 4 and "4" are the same; null removes the key (a binding's default).
    ini = {key: None if value is None else str(value) for key, value in data.get("ini", {}).items()}
    return {"launcher": dict(data.get("launcher", {})), "ini": ini}


def save(path, settings, ini):
    """Writes the defaults; private like settings.json (it may hold the trophy key)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.touch(mode=0o600)
    path.chmod(0o600)
    path.write_text(json.dumps({"launcher": settings, "ini": ini}, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def restore(settings, defaults, builtin_settings, builtin_ini, keep=()):
    """(launcher settings, bbport.ini keys to write) for a restore. With `defaults` (load()), its
    keys over the current settings; a value of another type than the built-in one (a hand-edited
    file) is skipped, as the window could not show it. Without, the built-in settings except the
    `keep` keys (folders, the trophy key) and the built-in bbport.ini keys. bbport.ini keys not
    named stay as they are (ini_lines)."""
    restored = dict(settings)
    if defaults is None:
        restored.update({key: value for key, value in builtin_settings.items() if key not in keep})
        return restored, dict(builtin_ini)
    for key, value in defaults["launcher"].items():
        if key in builtin_settings and type(value) is not type(builtin_settings[key]):
            continue
        restored[key] = value
    return restored, dict(defaults["ini"])


def ini_lines(values, lines):
    """bbport.ini's lines with `values` written: edited keys rewritten in place, missing ones
    appended, comments and other keys kept; a key whose value is None is removed (a control
    binding back to its default)."""
    written = set()
    out = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in values:
                if values[key] is not None:
                    out.append(f"{key}={values[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# bbport settings (in-game menu: Insert / L3+R3)")
    for key, value in values.items():
        if key not in written and value is not None:
            out.append(f"{key}={value}")
    return out
