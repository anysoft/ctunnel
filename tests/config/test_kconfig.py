#!/usr/bin/env python3
"""Regression tests for the offline Kconfig pipeline."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts/kconfig/configure.py"
MERGER = ROOT / "scripts/merge_config.py"


class KconfigTests(unittest.TestCase):
    def run_generator(self, config: pathlib.Path, build: pathlib.Path, *action: str,
                      success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            ["python3", str(GENERATOR), "--config", str(config), "--build-dir", str(build),
             "--header", str(build / "include/generated/autoconf.h"), "--target", "linux",
             *action],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if success and result.returncode:
            self.fail(result.stdout + result.stderr)
        if not success and not result.returncode:
            self.fail("invalid configuration unexpectedly succeeded")
        return result

    def preset(self, name: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            config = temp / ".config"
            self.run_generator(config, temp / "build", "defconfig", f"configs/{name}_defconfig")
            text = config.read_text(encoding="utf-8")
            self.assertIn("CONFIG_CRYPTO_MONOCYPHER=y", text)
            self.assertTrue((temp / "build/generated/config.cmake").is_file())
            self.assertTrue((temp / "build/generated/config.mk").is_file())
            self.assertTrue((temp / "build/include/generated/autoconf.h").is_file())

    def test_mini_defconfig(self) -> None:
        self.preset("mini")

    def test_default_defconfig(self) -> None:
        self.preset("default")

    def test_full_defconfig(self) -> None:
        self.preset("full")

    def invalid(self, body: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            config = temp / "invalid.config"
            config.write_text(body, encoding="utf-8")
            return self.run_generator(config, temp / "build", "defconfig", str(config),
                                      success=False)

    def test_roles_cannot_both_be_disabled(self) -> None:
        result = self.invalid("""\
# CONFIG_CTUNNEL_ROLE_BOTH is not set
# CONFIG_CTUNNEL_ROLE_CLIENT_ONLY is not set
# CONFIG_CTUNNEL_ROLE_SERVER_ONLY is not set
""")
        self.assertIn("resolved", result.stderr)

    def test_work_pool_dependency(self) -> None:
        result = self.invalid("""\
# CONFIG_FEATURE_WORK_CONNECTION is not set
CONFIG_FEATURE_WORK_POOL=y
""")
        self.assertIn("FEATURE_WORK", result.stderr)

    def test_data_encryption_dependency(self) -> None:
        result = self.invalid("""\
# CONFIG_CRYPTO_CHACHA20_POLY1305 is not set
CONFIG_FEATURE_DATA_ENCRYPTION=y
""")
        self.assertIn("DATA_ENCRYPTION", result.stderr)

    def test_out_of_range_integer(self) -> None:
        result = self.invalid("CONFIG_MAX_SERVICES=9999\n")
        self.assertIn("MAX_SERVICES", result.stderr)

    def test_unknown_symbol(self) -> None:
        result = self.invalid("CONFIG_CTUNNEL_DOES_NOT_EXIST=y\n")
        self.assertIn("unknown Kconfig symbol", result.stderr)

    def test_legacy_static_link_symbol_migrates(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            config = temp / ".config"
            config.write_text("CONFIG_STATIC_LINK=y\n", encoding="utf-8")
            self.run_generator(config, temp / "build", "olddefconfig")
            text = config.read_text(encoding="utf-8")
            self.assertIn("CONFIG_CTUNNEL_LINK_STATIC=y", text)
            self.assertNotIn("CONFIG_STATIC_LINK", text)

    def test_noninteractive_backend_can_reload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            config = temp / ".config"
            build = temp / "build"
            self.run_generator(config, build, "defconfig", "configs/default_defconfig")
            self.run_generator(config, build, "olddefconfig")
            header = (build / "include/generated/autoconf.h").read_text(encoding="utf-8")
            self.assertIn("#define CONFIG_CTUNNEL_CLIENT 1", header)

    def test_default_link_mode_is_mostly_static_and_exports_one_choice(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            config = temp / ".config"
            build = temp / "build"
            self.run_generator(config, build, "defconfig", "configs/default_defconfig")
            generated = (build / "generated/config.cmake").read_text(encoding="utf-8")
            self.assertIn("set(CONFIG_CTUNNEL_LINK_DYNAMIC OFF)", generated)
            self.assertIn("set(CONFIG_CTUNNEL_LINK_MOSTLY_STATIC ON)", generated)
            self.assertIn("set(CONFIG_CTUNNEL_LINK_STATIC OFF)", generated)

    def test_profile_role_and_link_fragments_are_independent(self) -> None:
        combinations = (
            ("configs/mini_defconfig", "configs/role/server.config",
             "configs/link/static.config", "CTUNNEL_LINK_STATIC"),
            ("configs/mini_defconfig", "configs/role/server.config",
             "configs/link/mostly-static.config", "CTUNNEL_LINK_MOSTLY_STATIC"),
            ("configs/default_defconfig", "configs/role/client.config",
             "configs/link/dynamic.config", "CTUNNEL_LINK_DYNAMIC"),
            ("configs/full_defconfig", "configs/role/both.config",
             "configs/link/dynamic.config", "CTUNNEL_LINK_DYNAMIC"),
        )
        for profile, role, link, expected_link in combinations:
            with self.subTest(profile=profile, role=role, link=link):
                with tempfile.TemporaryDirectory() as directory:
                    temp = pathlib.Path(directory)
                    result = subprocess.run(
                        ["python3", str(MERGER), "--config", str(temp / ".config"),
                         "--build-dir", str(temp / "build"), "--target", "linux",
                         profile, role, link],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    if result.returncode:
                        self.fail(result.stdout + result.stderr)
                    text = (temp / ".config").read_text(encoding="utf-8")
                    self.assertIn(f"CONFIG_{expected_link}=y", text)
                    if "server.config" in role:
                        self.assertIn("CONFIG_CTUNNEL_ROLE_SERVER_ONLY=y", text)
                    elif "client.config" in role:
                        self.assertIn("CONFIG_CTUNNEL_ROLE_CLIENT_ONLY=y", text)
                    else:
                        self.assertIn("CONFIG_CTUNNEL_ROLE_BOTH=y", text)


if __name__ == "__main__":
    unittest.main()
