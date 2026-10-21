# AUTH/AUTHCALL JIT & VM 侧交接报告

节点：补齐 AUTH(0xf6)/AUTHCALL(0xf7) 的 JIT 与 VM 侧支持（文件边界：JIT 适合度分析、JIT 前端、VM 执行路径）
状态：✅ 实现完成，合并候选（当前工作区）增量构建成功，完整测试 96/96 通过（exit 0）

## 1. 本节点交付内容

### 1.1 evmc fork 表/枚举（构建必需的前置项）
前序实现节点只在子模块工作树留下这些修改、未提交到任何分支；本节点在工作区 evmc 子模块工作树中落地（与实现节点一致，仍未 push——需要 DTVMStack/evmc fork 补提交或由交接链处理）：
- `include/evmc/instructions.h`：新增 `OP_AUTH = 0xf6`、`OP_AUTHCALL = 0xf7`
- `lib/instructions/instruction_metrics.c`：OSAKA 表 `0xf6 {3100,3,-2}`、`0xf7 {WARM_STORAGE_READ_COST,7,-6}`（低 revision 保持 UNDEFINED）
- `lib/instructions/instruction_names.c`：OSAKA 表 `"AUTH"` / `"AUTHCALL"`（低 revision 保持 NULL）

没有这些，`src/evm/*`（引用 `evmc_opcode::OP_AUTH`）与 EVM 解释器 OSAKA 门控都无法编译/工作。

### 1.2 JIT 适合度分析（fallback-only 降级）— `src/compiler/evm_frontend/evm_analyzer.h`
- `JITSuitabilityResult` 新增 `AuthOpcodeCount` 诊断计数。
- `analyzeSuitability`：扫描字节码时，遇到 OP_AUTH/OP_AUTHCALL（0xf6/0xf7）：
  - 计数 `AuthOpcodeCount++`；
  - 若当前 revision 的 names 表定义该 opcode（即 OSAKA+），将最终 verdict 置为 `ShouldFallback=true`——修复了"循环内赋值被函数末尾阈值赋值覆盖"的逻辑错误：现在用局部 `ForceFallbackAuth` 并入最终表达式。
  - pre-OSAKA：opcode 未定义，两引擎一致返回 EVMC_UNDEFINED_INSTRUCTION，不强制 fallback。
  - PUSH 立即数内的 0xf6/0xf7 字节不误判（扫描按 `immediateSize` 跳过 PUSH 数据）。
- 效果：装载含 AUTH/AUTHCALL 的合约时 `EVMModule::ShouldFallbackToInterp=true`，VM `execute()` 与 profile-guided JIT 均路由到解释器 fast path（解释器已完整实现契约）。

### 1.3 JIT 前端防御 — `src/action/evm_bytecode_visitor.h`
- decode() 主 switch 增加 `case OP_AUTH / OP_AUTHCALL`：若模块绕过适合度门控仍被直接编译，抛 `std::logic_error`（fail compilation）→ `EagerEVMJITCompiler::compile` catch 置 `ShouldFallbackToInterp=true`，绝不把已定义 opcode 静默编译成 EVMC_UNDEFINED_INSTRUCTION。

### 1.4 测试
- `src/tests/evm_interp_tests.cpp` 追加 4 个真实 JIT 侧回归测试（`EIP3074Auth.JitSuitability*`）：
  - OSAKA+AUTH / OSAKA+AUTHCALL → `ShouldFallback==true`、`AuthOpcodeCount==1`；
  - PRAGUE+0xf6 → 不强制 fallback（计数仍为 1）；
  - PUSH 立即数内 0xf6/0xf7 → 不误判（计数 0、不 fallback）。
- 修复测试侧两个此前从未运行过的 bug：
  1. `evm_interp_tests.cpp` `AutcallReturnDataMatchesCallSemantics` 处 `"50" + "6020"`（字符串字面量相加，编译错误）→ `std::string("50") + ...`；
  2. `ToUint256Hex` 循环上界 16→8（`uint64_t` 移位 ≥64 为 UB，x86 移位掩码导致值在高半区 ghost 复制，断言全错）。
- 修复语料 `tests/evm_asm/authcall_invalid_commit_tamper.evm.hex`：原语料用**同一个 nonce k 重签名**（r 与 valid 用例完全相同），其签名对篡改后的 commit 实际有效 → AUTH 正确返回 1，与 `.expected`（0）矛盾。已改为保留 valid 的 (r,s,parity)、仅篡改内存 commit，使恢复失败 → 返回 0。用本仓库真实 secp256k1+keccak 独立验证（另见 §3）。

## 2. 验证结果（合并候选，真实运行）

构建配置：`cmake -B build -DCMAKE_BUILD_TYPE=Debug -DZEN_ENABLE_EVM=ON -DZEN_ENABLE_SINGLEPASS_JIT=OFF -DZEN_ENABLE_SPEC_TEST=ON`（MULTIPASS/SINGLEPASS 均 OFF，EVM 仅支持解释器 + multipass，本环境无 LLVM 15）。
- 目标：`evmInterpTests` 增量构建 exit 0。
- 结果：`./build/evmInterpTests` 全量 **96 tests / 96 passed（exit 0，361 ms）**，含：
  - `EIP3074Auth.*`：19 个解释器契约测试 + 4 个新 JIT 适合度测试；
  - `EVMAuthCall.*`：17 个（gas/签名/帧内 authorized/调用路径/return data）；
  - `EVMSamples/EVMSampleTest`：16 组 authcall 语料（EVMC_OSAKA/InterpMode）全部通过；
  - 既有回归（state save/load、regression、host module cache）无破坏。
- 关键契约点经真实执行确认：OSAKA 门控、AUTH/AUTHCALL 动态 gas（2929/内存扩展/6700/25000）、secp256k1 签名恢复与 low-s/parity 校验、commit/chainId/nonce/invoker 消息绑定、AUTH 帧内状态与 AUTHCALL 调用路径、static 模式。

## 3. 证据（非仅 grep）

- 语料修复用本仓库真实 `secp256k1::recoverPublicKey` + `ethash::keccak256` 编写独立校验程序：valid 语料在 chain=0/nonce=0/invoker=0（EVMSampleTest host 实际参数）下恢复为 authority `19e7e376...`；篡改语料改为 commit 篡改后恢复失败（与修复后 `.evm.hex` 一致）。原语料则因同 k 重签实际也恢复成功，证明是语料数据问题而非 VM 问题。
- `git status`：仅 `evmc`（子模块工作树）、`src/action/evm_bytecode_visitor.h`、`src/compiler/evm_frontend/evm_analyzer.h`、`src/tests/evm_interp_tests.cpp`、`tests/evm_asm/authcall_invalid_commit_tamper.evm.hex` 有改动；未触碰其他实现文件。

## 4. 真实剩余限制（如实声明）

1. **生产 VM 宿主 nonce 集成**：`src/vm/wrapped_host.h`（dt_evmc_vm 的 EVMC host 包装）没有也不会调用 `registerAccountNonceProvider`；EVMC 标准 `evmc_host_interface` 无 nonce 查询接口。因此生产 EVMC 宿主路径的 AUTH 以 `getAccountNonce` 的默认值 0 参与消息（`getAccountNonce` 未注册 provider 时返回 0）。测试宿主 `ZenMockedEVMHost` 已注册 provider（构造/析构），`authcall_nonce_mismatch`（nonce=5 vs host 0）等测试证明非零 nonce 契约在可注册 provider 的宿主上正确工作。**不能把"生产默认 nonce=0"说成支持真实非零 nonce**——外部集成方需对 EVMC host 调用 `registerAccountNonceProvider` 才能提供真实 nonce；该约束源于 EVMC ABI 无 nonce 能力，属职责边界外。
2. **JIT 执行路径**：AUTH/AUTHCALL 按研究契约采用 fallback-only 降级（JIT 前端无 lowering，模块级强制解释器），EVMC 构建下 JIT 路径即解释器 fast path。本环境无 LLVM 15，multipass JIT 二进制未实际构建；JIT 侧以 analyzer 回归测试 + 前端防御 + VM 路由代码核对验证。
3. **evmc 子模块**：三个文件改动仅在子模块工作树，未提交/push（fork 提交或 gitlink 更新需交接链/平台权限处理）。
4. 本节点未 commit/push（任务未要求交付提交；开发修改默认保留在工作区）。

## 5. 交接说明

- 完成项：AUTH/AUTHCALL 在 JIT 与 VM 执行路径的支持符合解释器契约（OSAKA 门控、gas、签名恢复），合并候选（当前工作区）实测通过。
- 需 A/后续节点注意：非零 nonce 的生产宿主接入、evmc fork 提交、multipass JIT 在有 LLVM 环境下的构建回归。