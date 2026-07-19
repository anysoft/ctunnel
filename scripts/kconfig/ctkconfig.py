#!/usr/bin/env python3
"""Shared, offline Kconfig support for ctunnel's build entry points."""

from __future__ import annotations

import hashlib
import os
import pathlib
import re
import sys
from typing import Dict, Iterable, Optional

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR / "vendor"))

import kconfiglib  # type: ignore  # vendored, pinned in scripts/kconfig/VERSION

ASSIGNMENT_RE = re.compile(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$")
UNSET_RE = re.compile(r"^# CONFIG_([A-Za-z0-9_]+) is not set$")


class ConfigError(RuntimeError):
    pass


def atomic_write(path: pathlib.Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = data.encode("utf-8")
    if path.exists() and path.read_bytes() == encoded:
        return
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(encoded)
    os.replace(tmp, path)


def new_kconfig() -> kconfiglib.Kconfig:
    os.environ["srctree"] = str(ROOT)
    target = os.environ.get("CTUNNEL_KCONFIG_TARGET", "auto").lower()
    if target == "auto":
        target = "windows" if os.name == "nt" else ("darwin" if sys.platform == "darwin" else "linux")
    os.environ["CTUNNEL_TARGET_OS"] = target
    return kconfiglib.Kconfig(str(ROOT / "Kconfig"), warn_to_stderr=True)


def requested_values(path: pathlib.Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    if not path.exists():
        return values
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        match = ASSIGNMENT_RE.match(line)
        if match:
            values[match.group(1)] = match.group(2).strip().strip('"')
            continue
        match = UNSET_RE.match(line)
        if match:
            values[match.group(1)] = "n"
    return values


def load(kconf: kconfiglib.Kconfig, path: pathlib.Path, check_requests: bool = True) -> None:
    requested = requested_values(path)
    legacy_static = requested.pop("STATIC_LINK", None)
    unknown = sorted(name for name in requested if name not in kconf.syms)
    if unknown:
        raise ConfigError("unknown Kconfig symbol(s): " + ", ".join("CONFIG_" + x for x in unknown))
    kconf.load_config(str(path), replace=True)
    if legacy_static == "y" and not any(
            name in requested for name in (
                "CTUNNEL_LINK_DYNAMIC", "CTUNNEL_LINK_MOSTLY_STATIC", "CTUNNEL_LINK_STATIC")):
        kconf.syms["CTUNNEL_LINK_STATIC"].set_value("y")
    if not check_requests:
        return
    mismatches = []
    for name, requested_value in requested.items():
        actual = kconf.syms[name].str_value
        if requested_value != actual:
            mismatches.append(f"CONFIG_{name} requested {requested_value!r}, resolved to {actual!r}")
    if mismatches:
        raise ConfigError("configuration dependency/range error:\n  " + "\n  ".join(mismatches))


def enabled(kconf: kconfiglib.Kconfig, name: str) -> bool:
    return kconf.syms[name].str_value == "y"


def validate(kconf: kconfiglib.Kconfig, target: str = "auto") -> None:
    required = (
        "FEATURE_TCP",
        "CRYPTO_MONOCYPHER",
        "CRYPTO_CHACHA20_POLY1305",
        "FEATURE_PUBLIC_KEY_AUTH",
        "FEATURE_WORK_CONNECTION",
        "FEATURE_INI_CONFIG",
        "FEATURE_UNKNOWN_CONFIG_ERROR",
    )
    missing = ["CONFIG_" + name for name in required if not enabled(kconf, name)]
    if missing:
        raise ConfigError("phase 1 requires: " + ", ".join(missing))
    if not enabled(kconf, "FEATURE_IPV4") and not enabled(kconf, "FEATURE_IPV6"):
        raise ConfigError("at least one of CONFIG_FEATURE_IPV4/CONFIG_FEATURE_IPV6 must be enabled")
    if not enabled(kconf, "CTUNNEL_CLIENT") and not enabled(kconf, "CTUNNEL_SERVER"):
        raise ConfigError("at least one role must be enabled")
    if enabled(kconf, "FEATURE_DATA_ENCRYPTION") and not enabled(kconf, "CRYPTO_CHACHA20_POLY1305"):
        raise ConfigError("CONFIG_FEATURE_DATA_ENCRYPTION requires the AEAD implementation")
    if enabled(kconf, "FEATURE_WORK_POOL") and not enabled(kconf, "FEATURE_WORK_CONNECTION"):
        raise ConfigError("CONFIG_FEATURE_WORK_POOL requires CONFIG_FEATURE_WORK_CONNECTION")
    if not enabled(kconf, "FEATURE_WORK_POOL"):
        for name in ("DEFAULT_POOL_COUNT", "DEFAULT_POOL_MIN", "DEFAULT_POOL_MAX"):
            if int(kconf.syms[name].str_value) != 0:
                raise ConfigError(f"CONFIG_{name} must be 0 when the work pool is disabled")

    event = next(
        name for name in ("EVENT_EPOLL", "EVENT_KQUEUE", "EVENT_POLL", "EVENT_WSAPOLL") if enabled(kconf, name)
    )
    normalized = target.lower()
    if normalized == "auto":
        normalized = "windows" if os.name == "nt" else ("darwin" if sys.platform == "darwin" else "linux")
    allowed = {
        "linux": {"EVENT_EPOLL", "EVENT_POLL"},
        "darwin": {"EVENT_KQUEUE", "EVENT_POLL"},
        "macos": {"EVENT_KQUEUE", "EVENT_POLL"},
        "windows": {"EVENT_WSAPOLL"},
        "freebsd": {"EVENT_KQUEUE", "EVENT_POLL"},
        "openbsd": {"EVENT_KQUEUE", "EVENT_POLL"},
        "netbsd": {"EVENT_KQUEUE", "EVENT_POLL"},
    }
    if normalized in allowed and event not in allowed[normalized]:
        raise ConfigError(f"CONFIG_{event} is not supported for target {target}")

    for default_name, maximum_name in (
        ("DEFAULT_MAX_STREAMS", "MAX_STREAMS"),
        ("DEFAULT_MAX_PENDING_STREAMS", "MAX_PENDING_STREAMS"),
    ):
        if int(kconf.syms[default_name].str_value) > int(kconf.syms[maximum_name].str_value):
            raise ConfigError(f"CONFIG_{default_name} exceeds CONFIG_{maximum_name}")
    if enabled(kconf, "FEATURE_WORK_POOL"):
        pool_min = int(kconf.syms["DEFAULT_POOL_MIN"].str_value)
        pool_count = int(kconf.syms["DEFAULT_POOL_COUNT"].str_value)
        pool_max = int(kconf.syms["DEFAULT_POOL_MAX"].str_value)
        if not pool_min <= pool_count <= pool_max:
            raise ConfigError("default work-pool limits must satisfy min <= count <= max")
    control_size = int(kconf.syms["CONTROL_BUFFER_SIZE"].str_value)
    required_control_size = max(
        int(kconf.syms["MAX_CLIENT_ID_LENGTH"].str_value) + 192,
        int(kconf.syms["MAX_SERVICE_ID_LENGTH"].str_value)
        + int(kconf.syms["MAX_ADDRESS_LENGTH"].str_value) + 32,
    )
    if control_size < required_control_size:
        raise ConfigError(
            "CONFIG_CONTROL_BUFFER_SIZE is too small for the configured ID/address limits "
            f"(need at least {required_control_size})")


def _cmake_quote(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;")


def _c_quote(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def symbols(kconf: kconfiglib.Kconfig) -> Iterable[kconfiglib.Symbol]:
    # Kconfiglib also exposes anonymous constant/range tokens through syms.
    # Only symbols declared by a config/menuconfig node belong in build output.
    return (kconf.syms[name] for name in sorted(kconf.syms) if kconf.syms[name].nodes)


def render_autoconf(kconf: kconfiglib.Kconfig, config_path: pathlib.Path) -> str:
    lines = [
        "/* Auto-generated from .config by scripts/kconfig/configure.py. */",
        f"/* Source: {config_path} */",
        "#ifndef CTUNNEL_GENERATED_AUTOCONF_H",
        "#define CTUNNEL_GENERATED_AUTOCONF_H",
        "",
    ]
    for sym in symbols(kconf):
        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            if sym.str_value == "y":
                lines.append(f"#define CONFIG_{sym.name} 1")
        elif sym.type == kconfiglib.STRING:
            lines.append(f'#define CONFIG_{sym.name} "{_c_quote(sym.str_value)}"')
        else:
            lines.append(f"#define CONFIG_{sym.name} {sym.str_value}")
    lines.extend(("", "#endif /* CTUNNEL_GENERATED_AUTOCONF_H */", ""))
    return "\n".join(lines)


def render_cmake(kconf: kconfiglib.Kconfig, config_path: pathlib.Path) -> str:
    digest = hashlib.sha256(config_path.read_bytes()).hexdigest()[:16]
    lines = [
        "# Auto-generated; edit .config through make menuconfig or scripts/config.",
        f'set(CTUNNEL_CONFIG_FILE "{_cmake_quote(str(config_path))}")',
        f'set(CTUNNEL_CONFIG_HASH "{digest}")',
    ]
    for sym in symbols(kconf):
        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            value = "ON" if sym.str_value == "y" else "OFF"
            lines.append(f"set(CONFIG_{sym.name} {value})")
        elif sym.type == kconfiglib.STRING:
            lines.append(f'set(CONFIG_{sym.name} "{_cmake_quote(sym.str_value)}")')
        else:
            lines.append(f"set(CONFIG_{sym.name} {sym.str_value})")
    lines.append("")
    return "\n".join(lines)


def render_make(kconf: kconfiglib.Kconfig, config_path: pathlib.Path) -> str:
    lines = ["# Auto-generated; do not edit.", f"CTUNNEL_CONFIG_FILE:={config_path}"]
    for sym in symbols(kconf):
        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            if sym.str_value == "y":
                lines.append(f"CONFIG_{sym.name}:=y")
            else:
                lines.append(f"# CONFIG_{sym.name} is not set")
        else:
            lines.append(f"CONFIG_{sym.name}:={sym.str_value}")
    lines.append("")
    return "\n".join(lines)


def generate(kconf: kconfiglib.Kconfig, config_path: pathlib.Path, build_dir: pathlib.Path,
             header_path: Optional[pathlib.Path] = None) -> None:
    validate(kconf, os.environ.get("CTUNNEL_KCONFIG_TARGET", "auto"))
    if header_path is None:
        header_path = ROOT / "include/generated/autoconf.h"
    atomic_write(header_path, render_autoconf(kconf, config_path))
    generated = build_dir / "generated"
    atomic_write(generated / "config.cmake", render_cmake(kconf, config_path))
    atomic_write(generated / "config.mk", render_make(kconf, config_path))


def save_config(kconf: kconfiglib.Kconfig, path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    kconf.write_config(str(path), header="# Generated by ctunnel's Kconfig frontend.\n")
