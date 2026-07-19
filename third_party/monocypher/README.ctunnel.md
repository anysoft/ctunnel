# 内置 Monocypher

- 上游：https://monocypher.org/
- 版本：4.0.3（2026-06-15）
- 官方归档：https://monocypher.org/download/monocypher-4.0.3.tar.gz
- 归档 SHA-256：`8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`
- 上游标签：https://github.com/LoupVaillant/Monocypher/releases/tag/4.0.3
- 许可证：BSD-2-Clause OR CC0-1.0；未经修改的上游许可证位于 `LICENSE`。

项目只内置未经修改的核心 `src/monocypher.c` 和 `src/monocypher.h`。头文件放在 `include/` 下，以明确区分第三方头文件搜索路径。可选的 SHA-512/Ed25519 翻译单元被有意排除：ctunnel Mini 使用核心 EdDSA-BLAKE2b API 和 BLAKE2b KDF，以减小代码体积。

记录的逐文件 SHA-256：

```text
57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be  src/monocypher.c
c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd  include/monocypher.h
5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3  LICENSE
```

CI 会验证 `VERSION`、上述哈希以及不存在外部密码学库依赖。升级固定版本时必须同时更新所有这些值。
