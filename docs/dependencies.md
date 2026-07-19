# 依赖与来源

## Monocypher 密码学依赖

- 固定版本：4.0.3；
- 上游项目：`https://monocypher.org/`；
- 源码归档：`https://monocypher.org/download/monocypher-4.0.3.tar.gz`；
- 归档 SHA-256：`8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`；
- 许可证：上游 BSD 2-Clause 文件保留在 `third_party/monocypher/LICENSE`；
- 内置单元：仅包含未修改的核心 `monocypher.c` 和 `monocypher.h`。

逐文件哈希及排除可选 Ed25519/SHA-512 翻译单元的原因记录在 `third_party/monocypher/README.ctunnel.md`。CI 会验证版本、许可证、哈希，以及只有后端适配器可以包含 `monocypher.h` 的规则。

第一期可执行文件没有外部密码学运行时依赖，尤其不会链接 libsodium、OpenSSL 或 mbedTLS。平台运行时依赖仅为目标 C/套接字库；Windows 还会使用系统 Winsock 和 BCrypt 库。

## Kconfiglib 构建前端

- 固定版本：14.1.0；
- 上游：`https://github.com/ulfalizer/Kconfiglib`；
- 内置文件：`kconfiglib.py`、`menuconfig.py` 和位于 `scripts/kconfig/vendor/` 的上游许可证；
- wheel SHA-256 和逐文件哈希：记录在 `scripts/kconfig/VERSION` 中，并由 CI 强制检查。

Kconfiglib 只作为宿主机构建前端使用，绝不会链接或作为目标运行时代码发布。内置副本使 menuconfig、defconfig 生成及非交互配置无需网络或 pip 即可工作。`requirements-build.txt` 记录准确的上游包版本，仅用于审计，不是安装前提。

## 更新规则

不得静默替换内置文件。更新时必须获取官方发布归档、核对公开源码和发布说明、运行上游测试、记录归档与逐文件哈希、保留新许可证、检查每个被调用的 API、运行 ctunnel 向量/Sanitizer/集成测试，并重新生成体积和依赖报告。密码学依赖更新必须经过常规的安全敏感代码审查。
