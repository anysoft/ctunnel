#!/usr/bin/env python3
"""Small scripts/config-compatible editor for automated builds."""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

from ctkconfig import ConfigError, ROOT, generate, load, new_kconfig, save_config, validate


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--file", type=pathlib.Path, default=pathlib.Path(os.environ.get("KCONFIG_CONFIG", ROOT / ".config")))
    p.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build")
    p.add_argument("--header", type=pathlib.Path, default=ROOT / "include/generated/autoconf.h")
    p.add_argument("--target", default="auto")
    p.add_argument("--enable", action="append", default=[], metavar="SYMBOL")
    p.add_argument("--disable", action="append", default=[], metavar="SYMBOL")
    p.add_argument("--set-val", action="append", default=[], nargs=2,
                   metavar=("SYMBOL", "VALUE"))
    p.add_argument("--state", metavar="SYMBOL")
    args = p.parse_args()
    os.chdir(ROOT)
    os.environ["CTUNNEL_KCONFIG_TARGET"] = args.target
    kconf = new_kconfig()
    if args.file.exists():
        load(kconf, args.file)

    if args.state:
        if args.enable or args.disable or args.set_val:
            p.error("--state cannot be combined with modification actions")
        name = args.state[7:] if args.state.startswith("CONFIG_") else args.state
        sym = kconf.syms.get(name)
        if sym is None:
            raise ConfigError(f"unknown Kconfig symbol: CONFIG_{name}")
        print(sym.str_value)
        return 0
    if not args.enable and not args.disable and not args.set_val:
        p.error("at least one --enable, --disable, --set-val, or --state action is required")

    def normalized(raw_name: str) -> str:
        return raw_name[7:] if raw_name.startswith("CONFIG_") else raw_name

    role_client = kconf.syms["CTUNNEL_CLIENT"].str_value == "y"
    role_server = kconf.syms["CTUNNEL_SERVER"].str_value == "y"
    role_changed = False
    modifications = [(normalized(name), "y") for name in args.enable]
    modifications += [(normalized(name), "n") for name in args.disable]
    modifications += [(normalized(name), value) for name, value in args.set_val]
    for name, value in modifications:
        if name == "CTUNNEL_CLIENT":
            role_client = value == "y"
            role_changed = True
            continue
        if name == "CTUNNEL_SERVER":
            role_server = value == "y"
            role_changed = True
            continue
        sym = kconf.syms.get(name)
        if sym is None:
            raise ConfigError(f"unknown Kconfig symbol: CONFIG_{name}")
        if not sym.set_value(value):
            raise ConfigError(f"invalid value {value!r} for CONFIG_{name}")
    if role_changed:
        if not role_client and not role_server:
            raise ConfigError("client and server roles cannot both be disabled")
        role_choice = "CTUNNEL_ROLE_BOTH" if role_client and role_server else (
            "CTUNNEL_ROLE_CLIENT_ONLY" if role_client else "CTUNNEL_ROLE_SERVER_ONLY")
        if not kconf.syms[role_choice].set_value("y"):
            raise ConfigError(f"cannot select CONFIG_{role_choice}")
    if any(name == "FEATURE_WORK_POOL" and value == "n" for name, value in modifications):
        for name in ("DEFAULT_POOL_COUNT", "DEFAULT_POOL_MIN", "DEFAULT_POOL_MAX"):
            kconf.syms[name].set_value("0")
    validate(kconf, args.target)
    save_config(kconf, args.file)
    generate(kconf, args.file.resolve(), args.build_dir.resolve(), args.header.resolve())
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConfigError as exc:
        print(f"configuration error: {exc}", file=sys.stderr)
        raise SystemExit(2)
