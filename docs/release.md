# 发布流程

版本标签格式为 `vMAJOR.MINOR.PATCH`。CI 会构建原生和交叉配置矩阵，验证架构、链接模式、真实依赖以及已裁剪的符号/字符串；随后把可执行文件与最终 `.config`、`ctunnel.link-report.txt`、示例、README 和许可证打包，生成区分链接模式的体积报告和 SHA-256 校验和，并上传到对应标签的 Release。未打标签的本地构建使用 CMake 项目版本加注入的 Git 提交号；二进制中不嵌入不断变化的构建时间戳。

当前线协议是 v3，与 v2 不兼容且不自动降级。发布说明必须显式标注：server 与 client 需要同时升级，滚动混跑会被版本检查拒绝；任何 transcript、KDF、nonce、AAD 或 work/stream authenticator 变更都必须再次提升 wire version。发布前必须通过严格 warning、ASan/UBSan、协议向量、集成密文捕获和 fuzz smoke gate。

发布归档应区分通用 Linux、musl、OpenWrt 和 Buildroot SDK 构建，并在 artifact 名称或元数据中记录链接模式。架构标签必须与 `file`/`readelf` 结果一致；标记为 Linux 全静态的产物还必须没有 `INTERP` 和 `NEEDED` 项。禁止把宿主机二进制改名冒充目标产物，也禁止把仍依赖系统运行库的产物称为“全静态”。

当前 Release 工作流会额外生成 `linux-armv5-gnueabi-static-mini`。它使用 `arm-linux-gnueabi` 软浮点 EABI 工具链，并以 `-march=armv5te -mfloat-abi=soft` 构建，面向没有 hard-float ABI 的旧 ARM 设备。不要把 `linux-armv7-gnueabihf-*` 产物部署到这类设备上；`gnueabihf` 需要目标系统和 CPU 浮点 ABI 同时匹配。

Release 不发布 ARMv5 glibc Mostly-static 包：公开 GitHub runner 的 `arm-linux-gnueabi` 工具链会引入当前 Ubuntu 的 glibc 版本需求，不能保证兼容 Buildroot/glibc 2.26 设备。需要更小的 ARMv5 glibc 2.26 Mostly-static 包时，必须使用设备 SDK 本地构建，详见 `docs/cross-compilation.md`。

原生桌面/服务器平台不使用 Mini 作为默认发布档位：`linux-x86_64-mostly-static`、`linux-i686-mostly-static`、`macos-x86_64-mostly-static`、`macos-arm64-mostly-static`、`windows-x86_64-static-runtime` 和 `windows-i686-static-runtime` 使用默认功能集，包含 client、server、keygen、fingerprint、configtest 和 build-info 等日常工具。Mini 主要用于 ARM/MIPS 等小体积嵌入式产物；如果某个平台需要同时发布完整工具集和 Mini，可以新增独立 artifact，不能用 Mini 覆盖默认包。
