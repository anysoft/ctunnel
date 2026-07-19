# 发布流程

版本标签格式为 `vMAJOR.MINOR.PATCH`。CI 会构建原生和交叉配置矩阵，验证架构、链接模式、真实依赖以及已裁剪的符号/字符串；随后把可执行文件与最终 `.config`、`ctunnel.link-report.txt`、示例、README 和许可证打包，生成区分链接模式的体积报告和 SHA-256 校验和，并上传到对应标签的 Release。未打标签的本地构建使用 CMake 项目版本加注入的 Git 提交号；二进制中不嵌入不断变化的构建时间戳。

发布归档应区分通用 Linux、musl、OpenWrt 和 Buildroot SDK 构建，并在 artifact 名称或元数据中记录链接模式。架构标签必须与 `file`/`readelf` 结果一致；标记为 Linux 全静态的产物还必须没有 `INTERP` 和 `NEEDED` 项。禁止把宿主机二进制改名冒充目标产物，也禁止把仍依赖系统运行库的产物称为“全静态”。
