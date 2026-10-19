#!/usr/bin/env bash
# =============================================================================
# DTVM — scripts/onboarding.sh 回归测试
#
# 覆盖场景（全部在本地沙箱内完成，不修改系统、不联网）：
#   1. 正常子模块：标准 git 子模块布局（.git 是 gitfile 文件，而非目录）
#      应通过检查，不得误报“未初始化”
#   2. 未初始化子模块：应失败并提示 git submodule update --init --recursive
#   3. ctest 零用例：应失败并提示未发现任何测试，不得误报“测试通过”
#   4. ctest 测试通过：应报告“测试通过”
#   5. ctest 测试失败：应报告“测试失败”
#
# 用法：bash scripts/test_onboarding.sh
# 依赖：git；cmake/gcc/python3/wabt/ctest 等构建工具以桩替代。
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ONBOARDING="$SCRIPT_DIR/onboarding.sh"

PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---- 断言辅助 ----------------------------------------------------------------

assert_rc() { # assert_rc <描述> <期望退出码> <实际退出码>
  local name="$1" want="$2" got="$3"
  if [ "$got" -eq "$want" ]; then
    PASS=$((PASS+1)); printf '[ok]   %s（退出码 %s）\n' "$name" "$got"
  else
    FAIL=$((FAIL+1)); printf '[FAIL] %s（期望退出码 %s，实际 %s）\n' "$name" "$want" "$got"
  fi
}

assert_contains() { # assert_contains <描述> <输出> <期望片段>
  local name="$1" out="$2" want="$3"
  if printf '%s' "$out" | grep -qF -- "$want"; then
    PASS=$((PASS+1)); printf '[ok]   %s\n' "$name"
  else
    FAIL=$((FAIL+1)); printf '[FAIL] %s（输出中未找到：%s）\n' "$name" "$want"
    printf '%s\n' "$out" | sed 's/^/       | /'
  fi
}

assert_not_contains() { # assert_not_contains <描述> <输出> <不应出现的片段>
  local name="$1" out="$2" want="$3"
  if printf '%s' "$out" | grep -qF -- "$want"; then
    FAIL=$((FAIL+1)); printf '[FAIL] %s（输出中不应出现：%s）\n' "$name" "$want"
  else
    PASS=$((PASS+1)); printf '[ok]   %s\n' "$name"
  fi
}

# ---- 沙箱构造 ----------------------------------------------------------------

# build_fixture <根目录>：创建真实 git 仓库 + 标准布局子模块
# （tests/wast/spec、evmc，.git 均为 gitfile 文件），并生成已应用的
# tests/wast/spec.patch；随后把当前 onboarding.sh 复制进沙箱仓库。
build_fixture() {
  local root="$1"
  local up_spec="$root/upstream-spec" up_evmc="$root/upstream-evmc" repo="$root/repo"
  local spec="$repo/tests/wast/spec"

  git init -q "$up_spec"
  git -C "$up_spec" config user.name dtvm-test
  git -C "$up_spec" config user.email dtvm-test@example.com
  printf ';; placeholder\n' > "$up_spec/a.wast"
  git -C "$up_spec" add -A
  git -C "$up_spec" commit -qm "init spec"

  git init -q "$up_evmc"
  git -C "$up_evmc" config user.name dtvm-test
  git -C "$up_evmc" config user.email dtvm-test@example.com
  printf '#pragma once\n' > "$up_evmc/evmc.h"
  git -C "$up_evmc" add -A
  git -C "$up_evmc" commit -qm "init evmc"

  git init -q "$repo"
  git -C "$repo" config user.name dtvm-test
  git -C "$repo" config user.email dtvm-test@example.com
  git -C "$repo" -c protocol.file.allow=always submodule add -q "$up_spec" tests/wast/spec
  git -C "$repo" -c protocol.file.allow=always submodule add -q "$up_evmc" evmc

  # 在 spec 子模块工作区制造一处已应用的改动，并据此导出 spec.patch
  printf ';; applied-change\n' >> "$spec/a.wast"
  git -C "$spec" diff -- a.wast > "$repo/tests/wast/spec.patch"

  mkdir -p "$repo/scripts"
  cp "$ONBOARDING" "$repo/scripts/onboarding.sh"
  git -C "$repo" add -A
  git -C "$repo" commit -qm "repo with submodules"
}

# make_fake_bin <目录>：生成构建工具桩，避免依赖宿主是否安装这些工具。
# ctest 桩按 CTEST_MODE 模拟 通过(pass)/失败(fail)/零用例(zero)。
make_fake_bin() {
  local dir="$1" t
  mkdir -p "$dir"
  for t in cmake gcc g++ python3 wast2json ninja make lld; do
    {
      # 注意：不能把命令名（如 python3）带进输出，否则 tool_version
      # 会先匹配到命令名里的数字（python3 -> "3"），误判版本过旧。
      printf '#!/usr/bin/env bash\n'
      printf 'printf "version 12.34.5\\n"\n'
    } > "$dir/$t"
    chmod +x "$dir/$t"
  done
  cat > "$dir/ctest" <<'EOF'
#!/usr/bin/env bash
mode="${CTEST_MODE:-pass}"
case "$mode" in
  zero)
    printf 'No tests were found!!!\n'
    printf 'Please enable at least one test and re-run.\n'
    exit 0
    ;;
  fail)
    printf 'The following tests FAILED:\n'
    printf '\t2/2 Test #2: suite.spec ............***Failed\n'
    exit 1
    ;;
  *)
    printf '100%% tests passed, 5 tests out of 5\n'
    printf 'Total Test time (real) =   0.05 sec\n'
    exit 0
    ;;
esac
EOF
  chmod +x "$dir/ctest"
}

# ---- 场景 1：正常子模块（.git 为 gitfile 文件），check 应通过 ----------------

FIX1="$TMP/fixture-ok/repo"
build_fixture "$TMP/fixture-ok"
fake_bin="$TMP/fake-bin"
make_fake_bin "$fake_bin"
export PATH="$fake_bin:$PATH"

if [ -f "$FIX1/tests/wast/spec/.git" ] && [ ! -d "$FIX1/tests/wast/spec/.git" ]; then
  PASS=$((PASS+1)); printf '[ok]   前置：tests/wast/spec/.git 为标准 gitfile（文件）\n'
else
  FAIL=$((FAIL+1)); printf '[FAIL] 前置：tests/wast/spec/.git 应为 gitfile 文件\n'
fi

out1="$(cd "$FIX1" && bash scripts/onboarding.sh check 2>&1)"
rc1=$?
assert_rc    "场景1 正常子模块 check 退出码" 0 "$rc1"
assert_contains "场景1 子模块均已初始化" "$out1" "子模块均已初始化"
assert_contains "场景1 spec.patch 已应用" "$out1" "spec.patch 已应用"

# ---- 场景 2：未初始化子模块，check 应失败并给出指引 --------------------------

FIX2="$TMP/fixture-deinit/repo"
cp -a "$TMP/fixture-ok" "$TMP/fixture-deinit"
git -C "$FIX2" submodule deinit -f --all >/dev/null 2>&1

out2="$(cd "$FIX2" && bash scripts/onboarding.sh check 2>&1)"
rc2=$?
assert_rc        "场景2 未初始化子模块 check 退出码" 1 "$rc2"
assert_contains  "场景2 提示 submodule update" "$out2" "git submodule update --init --recursive"
assert_contains  "场景2 提示 spec 尚未初始化" "$out2" "tests/wast/spec 尚未初始化"

# ---- 场景 3/4/5：ctest 零用例 / 通过 / 失败 ----------------------------------

mkdir -p "$TMP/build"
: > "$TMP/build/cmake_install.cmake"

out3="$(cd "$FIX1" && BUILD_DIR="$TMP/build" CTEST_MODE=zero bash scripts/onboarding.sh test 2>&1)"
rc3=$?
assert_rc           "场景3 零用例 test 退出码" 1 "$rc3"
assert_contains     "场景3 提示未发现测试" "$out3" "未发现任何测试"
assert_not_contains "场景3 不误报测试通过" "$out3" "测试通过"

out4="$(cd "$FIX1" && BUILD_DIR="$TMP/build" CTEST_MODE=pass bash scripts/onboarding.sh test 2>&1)"
rc4=$?
assert_rc       "场景4 测试通过 test 退出码" 0 "$rc4"
assert_contains "场景4 报告测试通过" "$out4" "测试通过"

out5="$(cd "$FIX1" && BUILD_DIR="$TMP/build" CTEST_MODE=fail bash scripts/onboarding.sh test 2>&1)"
rc5=$?
assert_rc       "场景5 测试失败 test 退出码" 1 "$rc5"
assert_contains "场景5 报告测试失败" "$out5" "测试失败"

# ---- 汇总 -------------------------------------------------------------------

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]