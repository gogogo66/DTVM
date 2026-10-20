# DTVM 新同事常见问题排障说明

面向刚接触本仓库的新同事。上手入口是 `scripts/onboarding.sh`（见 `docs/start.md`），
它只做**只读**的环境检查、再驱动构建与测试，绝不自动安装依赖或修改系统。
本文档只覆盖最常见的情况；找不到答案时先跑一次 `./scripts/onboarding.sh --help`。

## 1. 上手入口：基本用法

从仓库根目录执行：

```sh
./scripts/onboarding.sh               # 依次执行 check -> build -> test
./scripts/onboarding.sh check         # 仅检查环境（不修改任何文件）
./scripts/onboarding.sh check --json  # 检查环境并以结构化 JSON 输出（供 CI 解析；人类可读信息写到 stderr，stdout 只含 JSON）
./scripts/onboarding.sh build         # 配置并构建
./scripts/onboarding.sh test          # 运行构建产物中的 spec 测试
./scripts/onboarding.sh test -- -R <name>   # 只跑名称匹配的用例（-- 不传给 ctest）
./scripts/onboarding.sh --help        # 或 -h，打印帮助
```

- 退出码：`0` 为就绪/成功；`1` 表示存在必须修复的环境问题或测试失败；
  `2` 表示子命令写错。
- 可选环境变量：`BUILD_DIR`（构建目录，默认 `<repo>/build`）、`JOBS`（并行数，
  默认 `nproc`）、`CMAKE_FLAGS`（传给 cmake configure 的额外参数，默认已开 spec 测试）。
- 整条链路依赖 git；首次 `build` 会通过 CMake FetchContent 下载外部依赖，可能较慢。

自检回归：`bash scripts/test_onboarding.sh`（本地沙箱内完成，不联网、不改系统）。

### 1.1 check --json：供 CI 解析的结构化输出

`./scripts/onboarding.sh check --json` 执行与 `check` 完全相同的只读检查，但把人类
可读信息写到 **stderr**，**stdout 只输出一份合法 JSON**，可直接交给 CI 解析：

```json
{
  "schema_version": 1,
  "tool": "scripts/onboarding.sh",
  "mode": "check",
  "repo_root": "<仓库根目录>",
  "status": "blocked",
  "failing": 1,
  "warning": 0,
  "checks": [
    { "category": "required_tools", "name": "cmake", "status": "fail",
      "detail": "cmake 未安装(cmake)。请安装：sudo apt-get install -y cmake" }
  ]
}
```

- 顶层字段：`schema_version`、`tool`、`mode`、`repo_root`、`status`、`failing`、
  `warning`、`checks[]`（每项含 `category`/`name`/`status`/`detail`）。
- `status` 与默认 check 的结论一一对应：`blocked`（存在必须修复的问题）、
  `ready_with_warnings`（基本就绪但有可选告警）、`ready`（就绪）。
- 退出码与默认 check 完全一致：`0` 就绪（含可忽略的告警）、`1` 未就绪。
- `--json` 只对 `check` 子命令生效；`build`/`test`/默认入口不接受该标志。
- 人类可读提示（`[x]`/`[!]`/`[ok]`、修复命令等）全部在 stderr，stdout 不含
  终端颜色或控制字符，解析前无需清理。

标志位说明：
- `--help` / `-h`：打印帮助后退出。
- `check --json`：如上，仅 `check` 子命令支持。
- `test -- -R <name>`：`--` 是脚本自身的参数分隔符，不会传给 ctest（见第 4 节）。

## 2. 环境检查失败：现象与解决办法

`check` 阶段会打印 `[x]`（必须修复）、`[!]`（可选优化，不阻塞）和 `[ok]`。
下面按常见的 `[x]` 逐一说明。

### 2.1 必备工具未安装 / 版本过旧

现象：

```
[x] cmake 未安装(cmake)。请安装：sudo apt-get install -y cmake
[x] gcc 13.3.0 过旧(要求 >= 9.4)。请升级：sudo apt-get install -y build-essential
```

检查的必备工具及最低版本：git ≥ 2.0、cmake ≥ 3.16.3、gcc ≥ 9.4、g++ ≥ 9.4、
python3 ≥ 3.8.10、wast2json(wabt) ≥ 1.0.10。

解决办法（Debian/Ubuntu 为例，脚本只给提示、不会自动执行）：

```sh
sudo apt-get update && sudo apt-get install -y cmake ninja-build build-essential python3 wabt zlib1g-dev
```

### 2.2 工具的 `--version` 执行失败

现象：

```
[x] gcc --version 执行失败(gcc)，无法判定版本。请确认 gcc 可正常执行
```

说明该命令存在但无法正常输出版本。先确认命令本身能运行：`command -v gcc && gcc --version`。

### 2.3 子模块未初始化

现象：

```
[!] 子模块未初始化：evmc
[!] 子模块未初始化：tests/wast/spec
[x] 请先初始化子模块：git submodule update --init --recursive
```

解决办法：在仓库根目录执行 `git submodule update --init --recursive`。

### 2.4 WebAssembly spec.patch 未应用

现象：

```
[x] 请应用补丁：cd tests/wast/spec && git apply ../spec.patch
[x] tests/wast/spec 尚未初始化（先执行 git submodule update --init --recursive），spec.patch 无从应用
```

先完成 2.3 初始化，再应用补丁：

```sh
cd tests/wast/spec && git apply ../spec.patch && cd ../..
```

### 2.5 环境未就绪：check 不通过即停止，不会进入构建

现象（默认入口 `./scripts/onboarding.sh` 与显式 `./scripts/onboarding.sh check`
在环境未就绪时，输出最后几行）：

```
------------------------------------------------------------------------
环境未就绪：请先按上方提示补齐依赖/步骤后重试。
提示：缺失的系统包用类似这样的命令安装（不会自动执行）：
  sudo apt-get update && sudo apt-get install -y cmake ninja-build build-essential python3 wabt zlib1g-dev
```

默认入口等价于 check -> build -> test：check 不通过时脚本**立即停止**并以退出码
`1` 结束，build 与 test 根本不会开始，也不会打印“已跳过构建”之类的提示——请以上面
这段真实输出为准。这是结果而非原因：先单独跑 `./scripts/onboarding.sh check`，
按上方 `[x]` 项逐条修复，直到 check 退出码为 `0`，再重新执行默认入口
（或 `./scripts/onboarding.sh build`）。

## 3. 可选项告警（不阻塞构建）

`[!]` 表示可选但推荐，可先忽略：

- `ninja`（CI 使用）→ `sudo apt-get install -y ninja-build`
- `lld`（链接器）→ `sudo apt-get install -y lld`
- `zlib.h` 缺失（zlib1g-dev）→ `sudo apt-get install -y zlib1g-dev`

## 4. test 阶段的常见失败

- **尚未构建**：提示 `尚未构建（找不到 $BUILD_DIR）`。先 `./scripts/onboarding.sh build`。
- **ctest 未安装**：ctest 随 cmake 分发，`sudo apt-get install -y cmake`。
- **未发现任何测试**（输出含 `No tests were found`）：脚本会显式判定为失败并提示，
  确认已开启 spec 测试（`ZEN_ENABLE_SPEC_TEST=ON`）且 2.3 的子模块已初始化，然后重新构建。
- **测试失败**：查看 ctest 输出；单个用例复现：
  `./build/specUnitTests i32 <0/1/2>`（0/1/2 对应 interpreter/singlepass/multipass）。

## 5. 仍然解决不了？

- 重跑一次并保留完整输出：`./scripts/onboarding.sh check 2>&1 | tee /tmp/dtvm-check.log`
- 补充阅读：`docs/start.md`（构建与测试）、`docs/user-guide.md`（用法）。