#!/usr/bin/env python3
"""Interactive menuconfig wrapper around the vendored Kconfiglib frontend."""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

from ctkconfig import ConfigError, ROOT, generate, load, new_kconfig, validate

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "vendor"))
import menuconfig as kconfig_menu  # type: ignore


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--config", type=pathlib.Path, default=pathlib.Path(os.environ.get("KCONFIG_CONFIG", ROOT / ".config")))
    p.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build")
    p.add_argument("--header", type=pathlib.Path, default=ROOT / "include/generated/autoconf.h")
    p.add_argument("--target", default="auto")
    args = p.parse_args()
    os.chdir(ROOT)
    os.environ["KCONFIG_CONFIG"] = str(args.config)
    os.environ["CTUNNEL_KCONFIG_TARGET"] = args.target
    kconf = new_kconfig()
    if args.config.exists():
        load(kconf, args.config)
    kconfig_menu.menuconfig(kconf)
    load(kconf, args.config)
    validate(kconf, args.target)
    generate(kconf, args.config.resolve(), args.build_dir.resolve(), args.header.resolve())
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConfigError as exc:
        print(f"configuration error: {exc}", file=sys.stderr)
        raise SystemExit(2)
