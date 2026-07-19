#!/usr/bin/env python3
"""Non-interactive BusyBox-style configuration frontend."""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

from ctkconfig import ConfigError, ROOT, generate, load, new_kconfig, save_config, validate


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser()
    p.add_argument("--config", type=pathlib.Path, default=pathlib.Path(os.environ.get("KCONFIG_CONFIG", ROOT / ".config")))
    p.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build")
    p.add_argument("--header", type=pathlib.Path, default=ROOT / "include/generated/autoconf.h")
    p.add_argument("--target", default="auto")
    sub = p.add_subparsers(dest="command", required=True)
    d = sub.add_parser("defconfig")
    d.add_argument("input", type=pathlib.Path)
    sub.add_parser("olddefconfig")
    sub.add_parser("oldconfig")
    s = sub.add_parser("savedefconfig")
    s.add_argument("output", nargs="?", type=pathlib.Path, default=ROOT / "defconfig")
    sub.add_parser("generate")
    return p


def main() -> int:
    args = parser().parse_args()
    os.chdir(ROOT)
    os.environ["CTUNNEL_KCONFIG_TARGET"] = args.target
    kconf = new_kconfig()
    if args.command == "defconfig":
        source = args.input if args.input.is_absolute() else ROOT / args.input
        load(kconf, source)
        validate(kconf, args.target)
        save_config(kconf, args.config)
    elif args.command == "olddefconfig":
        if args.config.exists():
            load(kconf, args.config)
        validate(kconf, args.target)
        save_config(kconf, args.config)
    elif args.command == "oldconfig":
        if args.config.exists():
            load(kconf, args.config)
        import oldconfig as interactive_oldconfig  # type: ignore  # vendored
        while True:
            interactive_oldconfig.conf_changed = False
            for node in kconf.node_iter():
                interactive_oldconfig.oldconfig(node)
            if not interactive_oldconfig.conf_changed:
                break
        validate(kconf, args.target)
        save_config(kconf, args.config)
    elif args.command == "savedefconfig":
        load(kconf, args.config)
        validate(kconf, args.target)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        kconf.write_min_config(str(args.output), header="# Minimal ctunnel configuration.\n")
    elif args.command == "generate":
        if not args.config.exists():
            raise ConfigError(f"configuration file not found: {args.config}")
        load(kconf, args.config)
    generate(kconf, args.config.resolve(), args.build_dir.resolve(), args.header.resolve())
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConfigError as exc:
        print(f"configuration error: {exc}", file=sys.stderr)
        raise SystemExit(2)
