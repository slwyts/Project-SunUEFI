# Release kernel 补丁来源

这八枚补丁由原始线性提交范围
`352508459733d3e6d349ea5581a8dd2fd8bb4180..efe5734c24510c3c194f765511b49be9f13b4aa0`
直接 `git format-patch --full-index --binary` 导出，没有改写补丁代码。

[series.json](series.json) 记录公共基线、原提交/父链/日期、每枚补丁 SHA256
与应用后的 tree。最终 tree 固定为
`17d484d4771a140e1eeb18eaf0315028adc5a16a`。

主机端准备入口：

```sh
python3 tools/prepare_release_kernel.py
```

工具从项目内内核仓库取得公共基线；对象缺失时仅向固定公开来源 fetch，
不要求原始 `efe5734` 提交可公开取得。它在
`build/kernel-worktrees/release-kernel` 应用补丁，并把实际提交/tree、
公共基线与原提交映射写入 `build/release-kernel/source-manifest.json`。

提交者使用本地临时身份，日期取自原提交。实际提交哈希可能与原提交不同；
每一步与最终 tree 均须匹配。重复执行验证已有结果，不重复应用补丁。
失败工作树与失败记录保留；检查失败原因后，可显式指定新的 `--worktree`
和 `--source-manifest`，不自动清理或放宽校验。

后续 full builder 使用记录中的 `actual_commit` 配合 `--worktree`/`--commit`。
源码准备状态为 `SOURCE_PREPARED_NOT_BUILT`，不表示内核已编译或实机已验收。
