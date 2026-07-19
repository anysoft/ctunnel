#!/usr/bin/env python3
"""Merge Kconfig fragments in order and emit a resolved ctunnel .config."""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR / "kconfig"))

from ctkconfig import (  # noqa: E402
    ConfigError,
    generate,
    load,
    new_kconfig,
    requested_values,
    save_config,
    validate,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Merge a base defconfig and later overriding fragments")
    parser.add_argument("--config", type=pathlib.Path, default=ROOT / ".config")
    parser.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build")
    parser.add_argument("--header", type=pathlib.Path,
                        default=ROOT / "include/generated/autoconf.h")
    parser.add_argument("--target", default="auto")
    parser.add_argument("fragments", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    os.chdir(ROOT)
    os.environ["CTUNNEL_KCONFIG_TARGET"] = args.target
    kconf = new_kconfig()
    # The merger reports meaningful value overrides itself. Suppress Kconfiglib's
    # duplicate-assignment warning for intentionally layered, identical fragments.
    kconf.warn_assign_override = False
    kconf.warn_assign_redun = False
    requested_by: dict[str, tuple[str, pathlib.Path]] = {}

    for index, raw_fragment in enumerate(args.fragments):
        fragment = raw_fragment if raw_fragment.is_absolute() else ROOT / raw_fragment
        if not fragment.is_file():
            raise ConfigError(f"configuration fragment not found: {raw_fragment}")
        values = requested_values(fragment)
        unknown = sorted(name for name in values if name not in kconf.syms)
        if unknown:
            raise ConfigError("unknown Kconfig symbol(s) in " + str(raw_fragment) + ": "
                              + ", ".join("CONFIG_" + name for name in unknown))
        for name, value in values.items():
            previous = requested_by.get(name)
            if previous and previous[0] != value:
                print(f"merge_config: {raw_fragment} overrides CONFIG_{name}="
                      f"{previous[0]} from {previous[1]} with {value}", file=sys.stderr)
            requested_by[name] = (value, raw_fragment)

        kconf.load_config(str(fragment), replace=index == 0)
        mismatches = []
        for name, value in values.items():
            actual = kconf.syms[name].str_value
            if value != actual:
                mismatches.append(
                    f"CONFIG_{name} requested {value!r}, resolved to {actual!r}")
        if mismatches:
            raise ConfigError(f"fragment {raw_fragment} has a dependency/range error:\n  "
                              + "\n  ".join(mismatches))

    validate(kconf, args.target)
    save_config(kconf, args.config)
    generate(kconf, args.config.resolve(), args.build_dir.resolve(), args.header.resolve())
    print(f"Merged {len(args.fragments)} fragments into {args.config}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConfigError as exc:
        print(f"merge_config: error: {exc}", file=sys.stderr)
        raise SystemExit(2)
