# 完整版本快照

`full-version.tar.gz` 保存教学版精简之前的全部已跟踪 l4lb 源码、文档、测试和配置（不含构建产物）。

原始提交：`89fd71665e2e0f74887839c4c69a9daabdc3ea41`。创建归档前 l4lb 工作区干净。

恢复到独立目录（从仓库根目录执行）：

```sh
mkdir -p /tmp/l4lb-full-reference
tar -xzf l4lb/legacy/full-version.tar.gz -C /tmp/l4lb-full-reference
cmake -S /tmp/l4lb-full-reference/l4lb -B /tmp/l4lb-full-reference/build -DL4LB_REQUIRE_JSONCPP=ON
cmake --build /tmp/l4lb-full-reference/build -j2
```

完整版本包含 Multi-Reactor、健康检查、LC、JSON、管理端、日志等扩展。当前默认工程不再提供这些功能。归档中的性能报告和面试说明仅适用于归档版本。
