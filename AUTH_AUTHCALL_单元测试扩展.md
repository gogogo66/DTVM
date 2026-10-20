# AUTH/AUTHCALL 单元测试扩展（报告）

节点：并行编写 AUTH(0xf6)/AUTHCALL(0xf7) 解释器单元测试（EIP-3074，OSAKA revision 门控）
状态：✅ 用例已写入工作区（未运行、未 commit/push——按契约真实运行留待验证节点在合并候选上执行）

## 1. 交付内容

### 1.1 语料用例（TEST_P 自动发现，`tests/evm_asm/`）
新增 **16 组** `<name>.evm.hex` + `<name>.expected`（EVMSampleTest 自动扫描 `.hex`，
`stem().stem()` 匹配 `.expected`；运行在 EVMC_OSAKA、InterpMode、默认 ZenMockedEVMHost）：

| 文件（去后缀） | 契约项 | 断言（status / error_code / return） |
|---|---|---|
| `authcall_valid_auth_returns_one` | 有效签名→authorized 设置 | success / 0 / 1 |
| `authcall_auth_then_autcall` | AUTH 后 AUTHCALL 成功 | success / 0 / 1 |
| `authcall_invalid_r_garbage` | r≥n 拒绝 | success / 0 / 0 |
| `authcall_invalid_yparity_2` | yParity∉{0,1} 拒绝 | success / 0 / 0 |
| `authcall_invalid_yparity_3` | 同上 | success / 0 / 0 |
| `authcall_invalid_yparity_255` | 同上 | success / 0 / 0 |
| `authcall_invalid_high_s` | s 高半区（防延展） | success / 0 / 0 |
| `authcall_invalid_commit_tamper` | commit 篡改→恢复失败 | success / 0 / 0 |
| `authcall_length_95_zero_pad` | length<97 越界补 0（补到与原 commit 尾一致→仍有效） | success / 0 / 1 |
| `authcall_length_94_zero_pad_fails` | length 再短 1 破坏 commit→失败 | success / 0 / 0 |
| `authcall_length_128_extra_ignored` | length>97 多余字节忽略 | success / 0 / 1 |
| `authcall_without_auth_invalid` | 无授权 AUTHCALL → invalid execution | invalid instruction / 4 / '' |
| `authcall_auth_failure_resets_authorized` | AUTH 失败复位→AUTHCALL invalid | invalid instruction / 4 / '' |
| `authcall_value_exceeds_authorized_balance` | value>authorized 余额 → invalid | invalid instruction / 4 / '' |
| `authcall_chainid_mismatch` | chainId 参与消息（字节码签 chainId=1，host chain_id=0） | success / 0 / 0 |
| `authcall_nonce_mismatch` | nonce 参与消息（字节码签 nonce=5，host nonce=0） | success / 0 / 0 |

> 注意：`.hex` 为手写字节码（easm 转换器尚无 AUTH/AUTHCALL mnemonic），
> 已修改 `tests/evm_asm/.gitignore` 豁免 `!authcall_*.evm.hex` 以保证可跟踪。
> 这些用例全部按研究契约在真实运行时可独立编译，不引用任何实现新增符号。

### 1.2 C++ 专用测试（`src/tests/evm_interp_tests.cpp` 末尾追加，+846 行，17 个 TEST）
走 host 直连（`RT->loadEVMModule` + `createEVMInstance` + `setRevision` + `callEVMMain`），
覆盖需要 host 定制 / 精确 gas / static / revision 门控的契约项：

| TEST | 契约项 | 覆盖点 |
|---|---|---|
| `ValidSignatureMakesAutcallCallerAuthorized` | #1 | 有效签名→AUTHCALL 子帧 CALLER==authority（子合约返回 CALLER） |
| `AUTHChargesWarmOrColdAuthorityAccess` | #1/#2 | AUTH warm 100 vs cold 2600，GasLeft 差 2500 |
| `AUTHMemoryExpansionChargedForFullLength` | #6 | length 97 vs 200 → 内存扩展差 = 9（按全长计费） |
| `InvalidSignatureResetsAuthorized` | #3 | r 垃圾 / yParity=2 / s 高半 / commit 篡改 → 复位后 AUTHCALL invalid |
| `ChainIdNonceAndInvokerBindToMessage` | #4 | chainId、nonce、invoker 分别不匹配 → AUTH=0 |
| `AuthorityWithCodeRejected` | #5 | EXTCODESIZE(authority)≠0 → 拒绝 |
| `AutcallWithoutAuthIsInvalidExecution` | #7 | 无 AUTH → invalid instruction |
| `AutcallValueAboveAuthorizedBalanceIsInvalid` | #8 | value>authorized 余额 → invalid（invoker 富余额无济于事） |
| `AutcallHasNoStipend` | #9 | value>0 无 2300 stipend：子帧 GAS 输出 = 转发量-2 |
| `AutcallGasZeroForwardsAllButOne64th` | #9 | gas==0 → 精确 all_but_one_64th（R−R/64，注释含逐段 gas 推导） |
| `AutcallDynamicCosts` | #10 | 冷地址 +2500；value 6700（非 9000）；空地址 value +25000（STOP 代码隔离执行开销） |
| `AuthorizedPersistsAcrossMultipleAutcalls` | #11 | authorized 跨多次 AUTHCALL 保持 |
| `AuthorizedDoesNotLeakIntoNestedFrames` | #12 | CALL / DELEGATECALL 不携带 authorized（子帧 AUTHCALL→invalid→0） |
| `AuthUndefinedBeforeOsaka` | #13 | PRAGUE / CANCUN → EVMC_UNDEFINED_INSTRUCTION |
| `StaticContextAllowsAuthButRejectsAutcall` | #14 | static 下 AUTH 允许返回 1；AUTHCALL → EVMC_STATIC_MODE_VIOLATION |
| `AuthOutOfGas` | #15 | AUTH gas 不足 → EVMC_OUT_OF_GAS（无输出残留） |
| `AutcallReturnDataMatchesCallSemantics` | #16 | RETURNDATACOPY 读取 AUTHCALL 子帧返回值 |

测试基础设施：额外增加 `PushBytes`（PUSH1/2/3 变长）、`SigStoreHex`（97B 签名区布局）、
`AuthHex`/`AutcallHex`/`CallStoreResultHex`/`DelegateStoreResultHex` 字节码构造辅助、
`RunAuthProgram` host 直连 runner（仅使用既有 API：RuntimeConfig/ZenMockedEVMHost/
loadEVMModule/createManagedIsolation/createEVMInstance/setRevision/callEVMMain）。

## 2. 签名向量（离线生成，运行时零签名）

- secp256k1 + keccak256 由独立脚本离线生成并硬编码（见 `src/tests/evm_interp_tests.cpp` 常量区）。
- 消息 = `keccak256(0x04 || chainId(32) || nonce(32) || invoker(32) || commit(32))`；
  97B 内存布局 = `yParity(1) || r(32) || s(32) || commit(32)`。
- 向量族：V1（chainId=0, nonce=0, invoker=0）、V2（chainId=1, invoker=0x..f1）、
  V5（chainId=0, nonce=0, invoker=0x..f1）、V8（nonce=5）；另含高 s、yParity≠{0,1}、
  r≥n、commit 篡改等变体。
- 生成脚本位置：`/tmp/gen_auth_vectors.py`、`/tmp/gen2.py`（工作区外临时件，未提交）。

## 3. 运行方式（验证节点执行）

```bash
# 1) 配置并构建（EVM 不支持 singlepass；本仓库默认 SINGLEPASS=ON，需显式关闭）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DZEN_ENABLE_SINGLEPASS_JIT=OFF
cmake --build build --target evmInterpTests -j$(nproc)

# 2) 运行
./build/tests/evmInterpTests --gtest_filter='EVMAuthCall.*:EVMSampleTest.*authcall_*'
#    或全量（含既有回归）
./build/tests/evmInterpTests
```

- TEST_P 语料自动发现：`getAllEvmBytecodeFiles()` 扫描 `tests/evm_asm/*.hex`，
  校验 status 字符串 / error_code / return 三字段（stack/memory/storage/events 为已解析但未比较的 TODO 字段）。
- 需要实现落地：AUTH/AUTHCALL 定义于 OSAKA、secp256k1 恢复、帧内 authorized。
  合并候选跑通即证明本套用例；若实现细节与契约有出入，测试会精确报出失败契约项。

## 4. 如实说明与已执行验证

- **本节点未运行完整测试**：当前运行环境磁盘 100% 满（29G/30G 已用、0 可用）且系统缺 bzip2，
  CMake FetchContent（boost 等）无法解压落地，完整 configure/build 在此环境不可行。
  按契约，真实编译 + 运行由验证节点在合并候选上执行；本节点仅工作区交付、不 commit/push。
- **已在本节点完成的覆盖性验证**（全部通过）：
  1. **字节码构造器自包含校验**：将新增 `ByteHex`/`PushBytes`/`SigStoreHex`/`AuthHex`/
     `AutcallHex`/`StoreTopAndReturnHex`/`ReturndataToOutputHex` 复制为独立 TU，用
     `g++ -std=c++17` 实际编译并运行，断言全部通过（"ALL CHECKS PASSED"）：
     - PushBytes 边界（0/0xFF/0x100/0xFFFF/0x10000/100000 → PUSH1/PUSH2/PUSH3 编码正确）；
     - AUTHCALL 7 参数压栈顺序与 gas==0 分支 hex 精确匹配；
     - RETURNDATACOPY(size, offset, destOffset) 压栈顺序。
  2. **16 个 `.evm.hex` 反汇编核对**：逐条解析 PUSH 序列，确认 AUTH(0xf6)/AUTHCALL(0xf7)
     参数顺序、内存签名区 97B 布局（yParity‖r‖s‖commit）正确。
  3. **语料配对完整性**：16 组 `.evm.hex`↔`.expected` 全部配对、hex 为合法偶数长度、
     `.expected` 均含 status/error_code/return 三字段；返回值为 32B 大写 hex（与
     `zen::utils::toHex` 输出大小写一致，比对样本 `mcopy.expected`）。
  4. **禁止引用检查**：追加代码零引用 `OP_AUTH`/`OP_AUTHCALL` 枚举、`get_nonce`、
     `ec_recover`、`authorized` 成员等尚不存在 API（grep 为空）；全部用原始字节 0xf6/0xf7。
  5. **括号配对**：跳过注释/字符串后 `{}`/`()`/`[]` 计数平衡。
- **遗留差异说明**：C++ 辅助器尾部编码用 PUSH0（`5f`），语料 corpus 用 PUSH1 0x00（`6000`），
  语义完全等价；EVMSampleTest 仅校验 status/error_code/return，不受影响。
- **未触碰实现文件**：`git status` 仅含 `src/tests/evm_interp_tests.cpp`（+846 行测试）、
  `tests/evm_asm/` 新增语料、`tests/evm_asm/.gitignore`（豁免 `authcall_*.evm.hex` 跟踪）、
  本报告。`evm_test_host.hpp`、evmc 头、`src/evm/*`、`src/host/evm/*` 均未改动。

## 5. 边界与交接（@B）

- 本节点未动：evmc 头/表（枚举、metrics、names 仍由实现节点在 osaka 表补齐）、
  `src/evm/*` 解释器与 opcode handlers、`src/host/evm/crypto*`（secp256k1）、VM。
- 需要 @B/实现节点配合的后续项（本次仅记录不越界）：
  1) evmc osaka 表补 `0xf6 AUTH {3100,3,-2}`、`0xf7 AUTHCALL {WARM_STORAGE_READ_COST,7,-6}`，
     低 revision 保持 NULL/UNDEFINED（本套用例会据此逐项断言行为）。
  2) `ec_recover`（`src/host/evm/`）按 yParity∈{0,1}、low-s 校验、恢复地址 == authority。
  3) AUTH nonce 取 host 当前 nonce（本套 nonce 篡改用例按"host nonce 参与消息"设计；
     若实现暂按 0 处理，`authcall_nonce_mismatch` 与 V1 向量(nonce=0) 仍自洽）。
  4) JIT（evm_frontend/singlepass）与 VM 侧：AUTH 帧内状态、AUTHCALL 调用路径、
     fallback-only 降级、SPP 屏障复核、cache 按 revision 失效，按调研契约 §3.2 由 @B 项补充。