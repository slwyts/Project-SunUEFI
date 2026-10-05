# Linux primary-source核对与候选stack，2026-10-05

只新增独立research ref与审计产物，没有move/rebase/cherry-pick实际branch、checkout、gitlink、profile pin或build/device操作。独立repo仍在piano-stable7a33，工作树clean；gitlink和两rescue pin原值保留。

| ref/来源 | 实际commit | 日期/范围 |
| --- | --- | --- |
| piano-stable / blu-sharky:piano-7.2.6 | 7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8 | test71 raw RAM smoke已验 |
| piano-next / 已保存torvalds:master | 7704c4c5bb127673b4f0ead839919db573559e38 | test73 raw RAM smoke已验；Makefile rc5 |
| 官方torvalds HEAD/master / peeled v7.3-rc6 | a90ee4305c4a5df72c11b31dacfdc76e00fcf78a | 发布2026-10-04；独立refs/research/2026-10-05/torvalds-master |
| 官方stable:linux-7.2.y / peeled v7.2.9 | 5fce161649b4d779d1b76d9fcd52dc77779774b8 | 发布2026-10-03；原本本地已保存同SHA |

版本和发布日期来自[kernel.org release feed](https://www.kernel.org/releases.json)。commit来自官方git ls-remote并核对peeled tags；[torvalds固定rc6 commit](https://github.com/torvalds/linux/commit/a90ee4305c4a5df72c11b31dacfdc76e00fcf78a)与本地对象一致。只用primary sources，网页状态不当硬件证据。

当前remotes为blu-sharky、torvalds、stable，未改配置。fetch使用exact SHA+blob:none、no-tags/no-write-fetch-head，仅新research ref；已验证branch和remote-tracking torvalds/master仍7704，未替换成a90。

## rc6是否带来Piano新支持

实际a90的唯一parent就是7704。`git rev-list 7704..a90`仅1commit，完整tree diff仅Makefile一行`-rc5→-rc6`。因此当前pin至rc6没有新增Piano/sm8750/DSI/panel/KTZ/HID支持，也没有吸收任何本地device/debug topic。不要将rc5版本字符串误解为本地pin缺少整周rc6修复；7704本身是`Merge tag i2c-fixes-7.3-rc6`的最后merge。

两tree中sm8750.dtsi blob均dd738d13df8e04b0302c4b4d7ab21e318a9560a0，ktz8866.c均53c1301dbb8c5a58aec33c7f51d527705dc50dba，backlight binding均c914e12769825f9436657c8f9153cbee32ecc5cf。Piano board DTS、NT36532 driver/binding、WN8030 driver路径在两tree中都不存在。

## 最小可移植topic proposal

Root决定后可从a90创建独立`candidate/piano-panel-7.3rc6`，按顺序携带四个canonical提交：

1. a5fdb179b28882df933d6afff0bf5ee4bf32d3ae：NT36532 binding。
2. 438dcb0c8ce2446da0c5896a114794ab1daa5e0f：dual DSI NT36532 driver、Kconfig/Makefile。
3. c1ac1cb0779eb1cba5e1c01f7848168a5a29b9da：paired KTZ binding。
4. 33ca14a5b6673d562606433112c90b97903a8466：paired-secondary KTZ driver。

本轮在临时GIT_INDEX_FILE中read-tree a90，逐commit做apply --check --cached和累计temporary-index apply，四步全部通过；6个最终变更路径的blob均和原panel topic完全一致。没有写真实index/worktree或创建branch。因所有相关API源blob未变，文字移植不需调整；这仍不是重新build或硬件显示证明。

1c2655ef4a94912fae39a14b943ba3d10b6847ce的disabled display topology另保留audit用途，5commit累计temporary-index apply通过；MDSS/SMMU/clock/interconnect和未知supplies的board-provider问题仍须主线适配，不能因overlay文字应用成功启用显示。

094d0b053f61ca20f584e10faa624cd0bc745db0及c4bbf928f335174f8518831797a94597a530c575两diagnostic commit也能累计apply，但固定ramoops/early markers/printk snapshot只用于定位，不进daily最低device stack。

现DSC multiple-slices/MSM DSI、q6v5/PAS IRQ、q6apm/QAIF/TDM八个原始generic canonical commits在rc5和rc6均为可证明祖先，维持上游实现。NT36532原始subsystem提交与GPU DT三项的负祖先查询受shallow/100k scan限制，继续unknown；target路径缺失是tree事实，不能把未知图查询伪装为已上游。18个旧snapshot文件相同也仍不证明整个display/audio squash等价。

stable从vanilla7.2.6至7.2.9的可见64commit、1437files变更，与fork115paths仅4项交集：MAINTAINERS、hci_qca.c、MSM dsi_host.c、pwrseq Kconfig。更新stable需单独审这4项和设备squash/backport，不无条件搬60个历史实验。当前优先rc6 panel候选文字上很小；没有立即移动rescue stable的必要。

## authoritative验证与产物

test71/73记录分别在`artifacts/kernels/stable/ram/ram-validation-test-71.json`与next对应73文件：exact源/config/live DTB、PID1、CPU0-7、180秒自动恢复、26分区匹配。验证限于原pin raw handoff；rc6即使只改版本也未实机验收，完整发行版/display/GPU/network/touch/audio/EFI-runtime目标仍未完成。

机器可复核产物位于`artifacts/kernel-research/2026-10-05/`：upstream-review.json、panel-upstream-audit.json和panel/DTB/memory-debug临时index apply-check JSON。shallow负查询保持unknown，不自动删除patch。下一步候选worktree/build需Root授权，本轮proposal未执行。

## 后续执行：rc6显示候选已建立并编译对象

Root 已在独立仓库建立 `codex/piano-panel-7.3rc6`，工作树位于 meta-repo 的 `build/kernel-worktrees/piano-panel-7.3rc6`。基线为上述官方 rc6 commit，按原顺序 cherry-pick 四个显示/backlight canonical commits，候选 HEAD 为 `ef43c0a879d3a82cf9f865bffee1035775b3b7d5`，工作树 clean。

新提交依次为 `52f4ab5b7ddd0d8086e8c25658503efdabb0bcac`、`fa710d90c233d2ad6df06cea9b745063408d6ffe`、`6b4f65880bdb257b7a84f91e6674776df73c4770`、`ef43c0a879d3a82cf9f865bffee1035775b3b7d5`。没有搬入仅用于诊断的 printk/ramoops topics 或 disabled board overlay。

使用独立 O=`build/kernel-topics/piano-panel-rc6`、实际 LLVM 23.1.1 和原 panel config，经 `olddefconfig` 后真实编译 `panel-novatek-nt36532.o` 与 `ktz8866.o`，进程 exit 0。对象、最终 config 和精确 manifest 保存于 `artifacts/kernel-topics/piano-panel-rc6/`，日志为 `build/logs/piano-panel-rc6-build.log`。这是两个驱动对象的编译验证，尚未构建完整 Image、接入可用板级 DTS 或实机显示；stable/next 已验分支、meta gitlink 和 profile pins 没有改变。
