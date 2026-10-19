#!/usr/bin/env bash
# =============================================================================
# DTVM — Linux 构建与测试上手入口 (onboarding)
#
# 面向新同事的轻量上手脚本：先检查环境，再构建，再运行测试。
#
# 设计约束
#   * 只做只读检查与调用用户已安装的构建工具，绝不自作主张修改系统或
#     自动安装依赖。
#   * 缺失的依赖会打印清晰的操作提示（以 Debian/Ubuntu 的 apt 为例），
#     由使用者自行决定是否安装。
#
# 用法
#   ./scripts/onboarding.sh            等价于 check -> build -> test
#   ./scripts/onboarding.sh check      仅检查环境（不修改任何文件）
#   ./scripts/onboarding.sh build      配置并构建
#   ./scripts/onboarding.sh test       运行构建产物中的 spec 测试
#   ./scripts/onboarding.sh test -- -R <name>  只运行名称匹配的用例（-- 不传给 ctest）
#   ./scripts/onboarding.sh --help     打印本帮助
#
# 可用环境变量（可选）：
#   CMAKE_FLAGS   传给 cmake configure 的额外参数（默认已开启 spec 测试）
#   BUILD_DIR     构建目录（默认 <repo>/build）
#   JOBS          并行编译任务数（默认 nproc）
# =============================================================================

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || printf '2')}"
CMAKE_FLAGS="${CMAKE_FLAGS:--DCMAKE_BUILD_TYPE=Debug -DZEN_ENABLE_SPEC_TEST=ON}"

# 颜色（终端不支持时自动降级）
if [ -t 1 ]; then
  C_FAIL="\033[31m"; C_OK="\033[32m"; C_WARN="\033[33m"
  C_DIM="\033[2m"; C_BOLD="\033[1m"; C_RESET="\033[0m"
else
  C_FAIL=""; C_OK=""; C_WARN=""; C_DIM=""; C_BOLD=""; C_RESET=""
fi

FAILED=0     # 1 = 存在必须修复的问题
WARNED=0     # 1 = 存在建议关注的问题（不阻塞构建）

dashed()  { printf "${C_DIM}%${2:-72}s${C_RESET}\n" "" | tr ' ' "${1:--}"; }
info()    { printf "${C_DIM}%s${C_RESET}\n" "$*"; }
warn()    { printf "${C_WARN}[!]${C_RESET} %s\n" "$*"; }
fail()    { FAILED=1; printf "${C_FAIL}[x]${C_RESET} %s\n" "$*"; }
ok()      { printf "${C_OK}[ok]${C_RESET} %s\n" "$*"; }
headline(){ printf "\n${C_BOLD}== %s ==${C_RESET}\n" "$*"; }

# 点分数字版本比较：$1 >= $2
ver_ge() {
  local i x y a=() b=()
  IFS='.' read -r -a a <<< "$1"
  IFS='.' read -r -a b <<< "$2"
  for i in "${!b[@]}"; do
    x="${a[$i]:-0}"; y="${b[$i]:-0}"
    if (( x > y )); then return 0; fi
    if (( x < y )); then return 1; fi
  done
  return 0
}

# 提取命令输出的第一个点分数字版本号
tool_version() {
  local cmd out
  cmd="$1"
  out="$("$cmd" --version 2>&1 | grep -oE '[0-9]+(\.[0-9]+){0,3}' | head -n1)"
  printf '%s' "${out:-0}"
}

# 检查必备工具（含最低版本），缺失/过旧给出提示
require() { # require <command> <min_version> <apt_pkg_name> <描述>
  local cmd min pkg desc ver
  cmd="$1"; min="$2"; pkg="$3"; desc="$4"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    fail "$desc 未安装($cmd)。请安装：sudo apt-get install -y $pkg"
    return 1
  fi
  ver="$(tool_version "$cmd")"
  if ver_ge "$ver" "$min"; then
    ok "$desc $ver(要求 >= $min)"
  else
    fail "$desc $ver 过旧(要求 >= $min)。请升级：sudo apt-get install -y $pkg"
  fi
}

discover() { # discover <command> <描述>  // 可选工具，仅提示
  local cmd desc
  cmd="$1"; desc="$2"
  if command -v "$cmd" >/dev/null 2>&1; then
    ok "$desc($(tool_version "$cmd"))"
  else
    warn "$desc 未安装($cmd)。可选但推荐：sudo apt-get install -y $(printf '%s' "$cmd" | sed -e 's/^wast2json$/wabt/' -e 's/^ninja$/ninja-build/')"
  fi
}

# 检查 git 子模块是否初始化
check_submodules() {
  dashed
  info "检查 git 子模块（tests/wast/spec、evmc）…"
  if [ ! -e "$REPO_ROOT/.gitmodules" ]; then
    ok "无子模块配置，跳过"
    return 0
  fi
  local missing=0 line st path
  while IFS= read -r line; do
    st="${line:0:1}"
    path="${line#* }"; path="${path%% *}"
    if [ "$st" = "-" ]; then
      warn "子模块未初始化：$path"
      missing=1
    fi
  done < <(git -C "$REPO_ROOT" submodule status 2>/dev/null)
  if [ "$missing" = "1" ]; then
    fail "请先初始化子模块：git submodule update --init --recursive"
  else
    ok "子模块均已初始化"
  fi
}

# 检查 WebAssembly spec.patch 是否已应用
check_spec_patch() {
  dashed
  info "检查 WebAssembly spec.patch …"
  local specdir="$REPO_ROOT/tests/wast/spec"
  local patchfile="$REPO_ROOT/tests/wast/spec.patch"
  # 标准 git 子模块的 .git 是 gitfile（文件，内容为 gitdir: …），不能用
  # -d 判断是否初始化；存在性 + rev-parse 可同时覆盖目录与 gitfile 布局。
  if [ ! -e "$specdir/.git" ] || ! git -C "$specdir" rev-parse --git-dir >/dev/null 2>&1; then
    fail "tests/wast/spec 尚未初始化（先执行 git submodule update --init --recursive），spec.patch 无从应用"
    return 1
  fi
  if [ ! -f "$patchfile" ]; then
    warn "未找到 spec.patch($patchfile)，跳过补丁检查"
    return 0
  fi
  if git -C "$specdir" apply --reverse --check "$patchfile" >/dev/null 2>&1; then
    ok "spec.patch 已应用"
  else
    fail "请应用补丁：cd tests/wast/spec && git apply ../spec.patch"
  fi
}

# 可选系统库（头文件存在性，不阻塞）
check_zlib() {
  if [ -f /usr/include/zlib.h ] || [ -f /usr/local/include/zlib.h ]; then
    ok "zlib 开发头文件存在"
  else
    warn "未找到 zlib.h（zlib1g-dev）。可选：sudo apt-get install -y zlib1g-dev"
  fi
}

check_env() {
  headline "环境检查（只读，不修改系统）"
  dashed
  info "仓库根目录：$REPO_ROOT"

  headline "必备构建工具"
  require git      2.0     git              "git"
  require cmake    3.16.3  cmake            "cmake"
  require gcc      9.4     build-essential  "gcc"
  require g++      9.4     build-essential  "g++"
  require python3  3.8     python3          "python3"
  require wast2json 1.0.10 wabt             "wast2json(wabt)"

  headline "可选 / 强烈建议的工具"
  discover ninja  "ninja（CI 使用，可选）"
  discover make   "make（默认构建器）"
  discover lld    "lld（链接器，可选）"
  check_zlib

  headline "仓库状态"
  check_submodules
  check_spec_patch

  dashed
  if [ "$FAILED" = "1" ]; then
    printf "${C_FAIL}环境未就绪：${C_RESET}请先按上方提示补齐依赖/步骤后重试。\n"
    printf "${C_DIM}提示：缺失的系统包用类似这样的命令安装（不会自动执行）：${C_RESET}\n"
    printf "  sudo apt-get update && sudo apt-get install -y cmake ninja-build build-essential python3 wabt zlib1g-dev\n"
    return 1
  fi
  if [ "$WARNED" = "1" ]; then
    printf "${C_WARN}环境基本就绪，但存在可选优化点（可忽略）。${C_RESET}\n"
  else
    printf "${C_OK}环境就绪，可以开始构建。${C_RESET}\n"
  fi
  return 0
}

build() {
  headline "配置并构建"
  if [ "$FAILED" = "1" ]; then
    fail "环境检查未通过，已跳过构建。请先修复后重试：./scripts/onboarding.sh check"
    return 1
  fi
  info "cmake 配置：-S $REPO_ROOT -B $BUILD_DIR $CMAKE_FLAGS"
  # shellcheck disable=SC2086  # CMAKE_FLAGS 需要按词拆分
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" $CMAKE_FLAGS || { fail "cmake 配置失败"; return 1; }
  info "开始编译（-j $JOBS），首次会下载 FetchContent 依赖，可能较慢…"
  cmake --build "$BUILD_DIR" -j "$JOBS" || { fail "编译失败"; return 1; }
  ok "构建成功，产物在 $BUILD_DIR"
}

run_tests() {
  headline "运行 spec 测试"
  if [ ! -d "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/cmake_install.cmake" ]; then
    fail "尚未构建（找不到 $BUILD_DIR）。请先运行：./scripts/onboarding.sh build"
    return 1
  fi
  if ! command -v ctest >/dev/null 2>&1; then
    fail "ctest 未安装（随 cmake 一起分发）。请安装：sudo apt-get install -y cmake"
    return 1
  fi
  info "运行 ctest（可指定用例：./scripts/onboarding.sh test -- -R <name>）"
  # `test -- -R <name>` 中的 `--` 是脚本自身的参数分隔符，不是 ctest 的
  # 参数；直接透传会让真实 CTest 报 Unknown argument，因此先剥离。
  if [ "${1:-}" = "--" ]; then
    shift
  fi
  local ctest_out ctest_rc
  ctest_out="$(cd "$BUILD_DIR" && ctest --output-on-failure "$@" 2>&1)"
  ctest_rc=$?
  printf '%s\n' "$ctest_out"
  if [ "$ctest_rc" -ne 0 ]; then
    fail "测试失败，请查看上方输出；单用例复现可用 ./build/specUnitTests i32 <0/1/2>"
    return 1
  fi
  # ctest 未发现任何测试时同样以 0 退出，必须显式识别，避免误报“测试通过”
  if printf '%s' "$ctest_out" | grep -q "No tests were found"; then
    fail "ctest 未发现任何测试：请确认已开启 spec 测试（ZEN_ENABLE_SPEC_TEST=ON）且子模块已初始化，然后重新构建"
    return 1
  fi
  ok "测试通过"
}

usage() {
  sed -n '2,18p' "$0" | sed 's/^# \?//'
}

# ---- 入口 ----
cmd="${1:-}"
case "$cmd" in
  --help|-h)
    usage; exit 0 ;;
  check)
    check_env ;;
  build)
    check_env && build ;;
  test)
    shift
    if [ "$#" -gt 0 ]; then run_tests "$@"; else run_tests; fi
    ;;
  "")
    # 默认：check -> build -> test
    check_env && build && run_tests ;;
  *)
    echo "未知子命令：$cmd（运行 $0 --help 查看用法）" >&2; exit 2 ;;
esac

exit $FAILED