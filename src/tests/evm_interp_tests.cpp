// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <string_view>
#include <yaml-cpp/yaml.h>

#include "evm/evm.h"
#include "evm/interpreter.h"
#include "evm_test_host.hpp"
#include "compiler/evm_frontend/evm_analyzer.h"
#include "runtime/evm_module.h"
#include "utils/evm.h"
#include "zetaengine.h"

using namespace zen;
using namespace zen::evm;
using namespace zen::runtime;

namespace {

std::filesystem::path getEvmAsmDirPath() {
  return std::filesystem::path(__FILE__).parent_path() /
         std::filesystem::path("../../tests/evm_asm");
}

std::vector<std::string> getAllEvmBytecodeFiles() {
  std::vector<std::string> Files;
  std::filesystem::path DirPath = getEvmAsmDirPath();

  if (!std::filesystem::exists(DirPath)) {
    std::cerr << "tests/evm_asm does not exist: " << DirPath.string()
              << std::endl;
    return Files;
  }

  for (const auto &Entry : std::filesystem::directory_iterator(DirPath)) {
    if (Entry.is_regular_file() && Entry.path().extension() == ".hex") {
      Files.push_back(Entry.path().string());
    }
  }

  std::sort(Files.begin(), Files.end());

  if (Files.empty()) {
    std::cerr << "No EVM hex files found in tests/evm_asm, "
              << "maybe you should convert the asm to hex first" << std::endl;
  }

  return Files;
}

struct ExpectedResult {
  std::string Status;
  uint8_t ErrorCode = 0;
  std::vector<std::string> Stack;
  std::string Memory;
  std::map<std::string, std::string> Storage;
  std::map<std::string, std::string> TransientStorage;
  std::string ReturnValue;
  std::vector<std::string> Events;
};

ExpectedResult readExpectedResult(const std::string &FilePath) {
  std::filesystem::path InputFilePath(FilePath);
  ExpectedResult Result;

  std::filesystem::path ExpectedPath =
      InputFilePath.parent_path() /
      (InputFilePath.stem().stem().string() + ".expected");

  std::ifstream Fin(ExpectedPath);
  if (!Fin) {
    return Result;
  }

  try {
    YAML::Node Doc = YAML::Load(Fin);

    if (Doc["status"]) {
      Result.Status = Doc["status"].as<std::string>();
    }

    if (Doc["error_code"]) {
      Result.ErrorCode = Doc["error_code"].as<uint8_t>();
    }

    if (Doc["stack"] && Doc["stack"].IsSequence()) {
      for (const auto &item : Doc["stack"]) {
        Result.Stack.push_back(item.as<std::string>());
      }
    }

    if (Doc["memory"]) {
      Result.Memory = Doc["memory"].as<std::string>();
    }

    if (Doc["storage"]) {
      if (!Doc["storage"].IsMap()) {
        throw std::runtime_error("Expected 'storage' to be a map type");
      }
      for (const auto &item : Doc["storage"]) {
        Result.Storage[item.first.as<std::string>()] =
            item.second.as<std::string>();
      }
    }

    if (Doc["transient_storage"]) {
      if (!Doc["transient_storage"].IsMap()) {
        throw std::runtime_error(
            "Expected 'transient_storage' to be a map type");
      }
      for (const auto &item : Doc["transient_storage"]) {
        Result.TransientStorage[item.first.as<std::string>()] =
            item.second.as<std::string>();
      }
    }

    if (Doc["return"]) {
      Result.ReturnValue = Doc["return"].as<std::string>();
    }

    if (Doc["events"]) {
      if (!Doc["events"].IsSequence()) {
        throw std::runtime_error("Expected 'events' to be a sequence type");
      }
      for (const auto &item : Doc["events"]) {
        if (!item.IsScalar()) {
          throw std::runtime_error("Expected each event to be a string type");
        }
        Result.Events.push_back(item.as<std::string>());
      }
    }
  } catch (const YAML::Exception &E) {
    std::cerr << "YAML parsing error: " << E.what() << std::endl;
    return Result;
  }

  return Result;
}

#ifdef ZEN_ENABLE_MULTIPASS_JIT
struct EVMExecutionResult {
  evmc_status_code Status = EVMC_INTERNAL_ERROR;
  std::string OutputHex;
  int64_t GasLeft = 0;
  bool JITCompiled = false;
};

EVMExecutionResult executeEvmBytecode(const std::string &ModuleName,
                                      const std::vector<uint8_t> &Bytecode,
                                      common::RunMode Mode,
                                      std::vector<uint8_t> CallData = {},
                                      uint64_t ExecutionGasLimitOverride = 0) {
  EVMExecutionResult Empty;

  RuntimeConfig Config;
  Config.Mode = Mode;
  Config.EnableEvmGasMetering = true;

  auto MockedHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  MockedHost->tx_context.tx_origin = zen::evm::DEFAULT_DEPLOYER_ADDRESS;
  auto RT = Runtime::newEVMRuntime(Config, MockedHost.get());
  EXPECT_TRUE(RT != nullptr) << "Failed to create runtime";
  if (!RT) {
    return Empty;
  }
  MockedHost->setRuntime(RT.get());

  auto ModRet = RT->loadEVMModule(ModuleName, Bytecode.data(), Bytecode.size());
  EXPECT_TRUE(ModRet) << "Failed to load module: " << ModuleName;
  if (!ModRet) {
    return Empty;
  }
  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  EXPECT_TRUE(Iso != nullptr) << "Failed to create isolation: " << ModuleName;
  if (!Iso) {
    return Empty;
  }

  uint64_t ExecutionGasLimit = ExecutionGasLimitOverride;
  if (ExecutionGasLimit == 0) {
    uint64_t GasLimit = 0xFFFF'FFFF'FFFF;
    const uint64_t IntrinsicGas = zen::evm::BASIC_EXECUTION_COST;
    ExecutionGasLimit = GasLimit - IntrinsicGas;
  }

  auto InstRet = Iso->createEVMInstance(*Mod, ExecutionGasLimit);
  EXPECT_TRUE(InstRet) << "Failed to create instance: " << ModuleName;
  if (!InstRet) {
    return Empty;
  }
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(evmc_revision::EVMC_OSAKA);

  evmc_message Msg = {
      .kind = EVMC_CALL,
      .flags = 0u,
      .depth = 0,
      .gas = static_cast<int64_t>(ExecutionGasLimit),
      .recipient = {},
      .sender = zen::evm::DEFAULT_DEPLOYER_ADDRESS,
      .input_data = CallData.empty() ? nullptr : CallData.data(),
      .input_size = CallData.size(),
      .value = {},
      .create2_salt = {},
      .code_address = {},
      .code = reinterpret_cast<const uint8_t *>(Mod->Code),
      .code_size = Mod->CodeSize,
  };

  evmc::Result RawResult;
  EVMExecutionResult Exec;
#ifdef ZEN_ENABLE_JIT
  Exec.JITCompiled = Mod->getJITCode() != nullptr && Mod->getJITCodeSize() > 0;
#endif
  EXPECT_NO_THROW({ RT->callEVMMain(*Inst, Msg, RawResult); });
  Exec.Status = RawResult.status_code;
  Exec.OutputHex =
      zen::utils::toHex(RawResult.output_data, RawResult.output_size);
  Exec.GasLeft = RawResult.gas_left;
  return Exec;
}

EVMExecutionResult executeEvmBytecodeFile(const std::string &FilePath,
                                          common::RunMode Mode,
                                          std::vector<uint8_t> CallData = {}) {
  EVMExecutionResult Empty;

  std::ifstream Fin(FilePath);
  EXPECT_TRUE(Fin.is_open()) << "Failed to open test file: " << FilePath;
  if (!Fin.is_open()) {
    return Empty;
  }

  std::string Hex;
  Fin >> Hex;
  zen::utils::trimString(Hex);
  auto BytecodeBuf = zen::utils::fromHex(Hex);
  EXPECT_TRUE(BytecodeBuf) << "Failed to convert hex to bytecode";
  if (!BytecodeBuf) {
    return Empty;
  }

  return executeEvmBytecode(FilePath, *BytecodeBuf, Mode, std::move(CallData));
}

std::vector<uint8_t> makeUint256Calldata(uint64_t Value) {
  std::vector<uint8_t> Data(32, 0);
  for (size_t I = 0; I < sizeof(Value); ++I) {
    Data[Data.size() - 1 - I] = static_cast<uint8_t>(Value & 0xff);
    Value >>= 8;
  }
  return Data;
}

std::string computeTwoWordKeccakHex(const std::vector<uint8_t> &Word0,
                                    const std::vector<uint8_t> &Word1) {
  EXPECT_EQ(Word0.size(), 32U);
  EXPECT_EQ(Word1.size(), 32U);
  std::vector<uint8_t> Input;
  Input.reserve(64);
  Input.insert(Input.end(), Word0.begin(), Word0.end());
  Input.insert(Input.end(), Word1.begin(), Word1.end());
  const auto Hash = zen::host::evm::crypto::keccak256(Input);
  return zen::utils::toHex(Hash.data(), Hash.size());
}

std::string computeKeccakHex(const std::vector<uint8_t> &Input) {
  const auto Hash = zen::host::evm::crypto::keccak256(Input);
  return zen::utils::toHex(Hash.data(), Hash.size());
}

std::vector<uint8_t> makePaddedAddressWord(const evmc::address &Address) {
  std::vector<uint8_t> Word(32, 0);
  std::memcpy(Word.data() + 12, Address.bytes, sizeof(Address.bytes));
  return Word;
}

std::vector<uint8_t> makeIncrementingBytes(size_t Size) {
  std::vector<uint8_t> Data(Size);
  for (size_t I = 0; I < Data.size(); ++I) {
    Data[I] = static_cast<uint8_t>(I);
  }
  return Data;
}

void expectInterpMatchesMultipass(const std::string &ModuleName,
                                  const std::vector<uint8_t> &Bytecode,
                                  const std::vector<uint8_t> &CallData,
                                  evmc_status_code ExpectedStatus,
                                  const std::string &ExpectedOutputHex = "",
                                  bool CheckGasLeft = true) {
  auto InterpExec = executeEvmBytecode(ModuleName + "_interp", Bytecode,
                                       common::RunMode::InterpMode, CallData);
  auto MultipassExec =
      executeEvmBytecode(ModuleName + "_multipass", Bytecode,
                         common::RunMode::MultipassMode, CallData);

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled)
      << "Multipass JIT should compile " << ModuleName;
#endif

  EXPECT_EQ(InterpExec.Status, ExpectedStatus)
      << "Interpreter status mismatch for " << ModuleName;
  EXPECT_EQ(MultipassExec.Status, ExpectedStatus)
      << "Multipass status mismatch for " << ModuleName;
  EXPECT_EQ(MultipassExec.Status, InterpExec.Status)
      << "Multipass status diverged from interpreter for " << ModuleName;
  EXPECT_EQ(MultipassExec.OutputHex, InterpExec.OutputHex)
      << "Multipass output diverged from interpreter for " << ModuleName;
  if (CheckGasLeft) {
    EXPECT_EQ(MultipassExec.GasLeft, InterpExec.GasLeft)
        << "Multipass gas_left diverged from interpreter for " << ModuleName;
  }

  if (!ExpectedOutputHex.empty()) {
    EXPECT_EQ(InterpExec.OutputHex, ExpectedOutputHex)
        << "Interpreter output mismatch for " << ModuleName;
    EXPECT_EQ(MultipassExec.OutputHex, ExpectedOutputHex)
        << "Multipass output mismatch for " << ModuleName;
  }
}

void expectMemoryLinearMstoreOverlapResult(uint64_t Stride,
                                           const std::string &ExpectedHex) {
  constexpr std::string_view BytecodeHex =
      "600035808080528101808052810180805281018080528151600052805160205260406000"
      "F3";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to build overlap probe bytecode";

  auto Exec = executeEvmBytecode("memory_linear_overlap_probe", *BytecodeBuf,
                                 common::RunMode::MultipassMode,
                                 makeUint256Calldata(Stride));

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(Exec.JITCompiled);
#endif
  EXPECT_EQ(Exec.Status, EVMC_SUCCESS);
  EXPECT_EQ(Exec.OutputHex, ExpectedHex);
}

void expectMultipassJitModuleLoads(const std::string &ModuleName,
                                   const std::vector<uint8_t> &Bytecode) {
  RuntimeConfig Config;
  Config.Mode = common::RunMode::MultipassMode;

  auto MockedHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  MockedHost->tx_context.tx_origin = zen::evm::DEFAULT_DEPLOYER_ADDRESS;
  auto RT = Runtime::newEVMRuntime(Config, MockedHost.get());
  ASSERT_TRUE(RT != nullptr) << "Failed to create runtime";

  MockedHost->setRuntime(RT.get());

  auto ModRet = RT->loadEVMModule(ModuleName, Bytecode.data(), Bytecode.size());
  ASSERT_TRUE(ModRet) << "Failed to load module: " << ModuleName;

#ifdef ZEN_ENABLE_JIT
  EVMModule *Mod = *ModRet;
  EXPECT_TRUE(Mod->getJITCode() != nullptr && Mod->getJITCodeSize() > 0);
#endif
}
#endif

} // namespace

class EVMSampleTest : public ::testing::TestWithParam<std::string> {};

std::string GetTestName(const testing::TestParamInfo<std::string> &Info) {
  std::filesystem::path Path(Info.param);
  return Path.stem().stem().string();
}

TEST_P(EVMSampleTest, ExecuteSample) {
  const std::string &FilePath = GetParam();

  ASSERT_NE(FilePath, "NoEvmHexFiles")
      << "No EVM hex files found, should convert easm to hex first";

  std::ifstream Fin(FilePath);
  ASSERT_TRUE(Fin.is_open()) << "Failed to open test file: " << FilePath;

  std::string Hex;
  Fin >> Hex;
  zen::utils::trimString(Hex);
  auto BytecodeBuf = zen::utils::fromHex(Hex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to convert hex to bytecode";

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;

  auto MockedHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  MockedHost->tx_context.tx_origin = zen::evm::DEFAULT_DEPLOYER_ADDRESS;

  auto RT = Runtime::newEVMRuntime(Config, MockedHost.get());
  ASSERT_TRUE(RT != nullptr) << "Failed to create runtime";

  // Set runtime for ZenMockedEVMHost to enable precompile calls
  MockedHost->setRuntime(RT.get());

  auto ModRet = RT->loadEVMModule(FilePath);
  ASSERT_TRUE(ModRet) << "Failed to load module: " << FilePath;

  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  ASSERT_TRUE(Iso) << "Failed to create Isolation: " << FilePath;

  // same as evm.codes: 0xFFFF'FFFF'FFFF (281,474,976,710,655)
  uint64_t GasLimit = 0xFFFF'FFFF'FFFF;
  const uint64_t IntrinsicGas = zen::evm::BASIC_EXECUTION_COST;
  const uint64_t ExecutionGasLimit = GasLimit - IntrinsicGas;

  auto InstRet = Iso->createEVMInstance(*Mod, ExecutionGasLimit);
  ASSERT_TRUE(Iso) << "Failed to create Instance: " << FilePath;
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(evmc_revision::EVMC_OSAKA);

  InterpreterExecContext Ctx(Inst);

  BaseInterpreter Interpreter(Ctx);

  evmc_message Msg = {
      .kind = EVMC_CALL,
      .flags = 0u,
      .depth = 0,
      .gas = static_cast<int64_t>(ExecutionGasLimit),
      .recipient = {},
      .sender = zen::evm::DEFAULT_DEPLOYER_ADDRESS,
      .input_data = nullptr,
      .input_size = 0,
      .value = {},
      .create2_salt = {},
      .code_address = {},
      .code = reinterpret_cast<const uint8_t *>(Mod->Code),
      .code_size = Mod->CodeSize,
  };
  Ctx.allocTopFrame(&Msg);

  EXPECT_NO_THROW({ Interpreter.interpret(); });

  // Read expected result from .expected file
  ExpectedResult Expected = readExpectedResult(FilePath);
  if (Expected.ReturnValue.empty() && Expected.Status.empty()) {
    ASSERT_TRUE(false) << "No expected file found for: " << FilePath;
  }

  evmc_status_code ActualStatus = Ctx.getStatus();
  std::string ActualStatusStr = evmc::to_string(ActualStatus);

  if (!Expected.Status.empty()) {
    EXPECT_EQ(ActualStatusStr, Expected.Status)
        << "Test: " << std::filesystem::path(FilePath).filename().string()
        << "\nExpected status: " << Expected.Status
        << "\nActual status: " << ActualStatusStr;
  }

  evmc_status_code expectedStatus =
      static_cast<evmc_status_code>(Expected.ErrorCode);
  EXPECT_EQ(ActualStatus, expectedStatus)
      << "Test: " << std::filesystem::path(FilePath).filename().string()
      << "\nExpected error_code: " << Expected.ErrorCode
      << "\nActual status: " << ActualStatus;

  const auto &Ret = Ctx.getReturnData();
  std::string HexRet = zen::utils::toHex(Ret.data(), Ret.size());

  if (!Expected.ReturnValue.empty()) {
    EXPECT_EQ(HexRet, Expected.ReturnValue)
        << "Test: " << std::filesystem::path(FilePath).filename().string()
        << "\nExpected return: " << Expected.ReturnValue
        << "\nActual return: " << HexRet;
  }

  // TODO: frame has been freed and can't check stack and memory values
  // TODO: storage, transient storage, and events check

  EXPECT_EQ(Ctx.getCurFrame(), nullptr)
      << "Frame should be deallocated after execution";
}

TEST(ZenMockedEVMHostModuleCacheTest, ReusesInternalCallModuleByCodeIdentity) {
  RuntimeConfig Config;
#ifdef ZEN_ENABLE_MULTIPASS_JIT
  Config.Mode = common::RunMode::MultipassMode;
#else
  Config.Mode = common::RunMode::InterpMode;
#endif

  auto MockedHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  auto RT = Runtime::newEVMRuntime(Config, MockedHost.get());
  ASSERT_TRUE(RT != nullptr) << "Failed to create runtime";
  MockedHost->setRuntime(RT.get());

  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "00000000000000000000000000000000000000c1");

  evmc::MockedAccount SenderAccount;
  SenderAccount.set_balance(1000);
  MockedHost->accounts[SenderAddr] = SenderAccount;

  evmc::MockedAccount ContractAccount;
  ContractAccount.code = {0x60, 0x00, 0x50, 0x00}; // PUSH1 0; POP; STOP
  ContractAccount.codehash.bytes[31] = 0xaa;
  MockedHost->accounts[ContractAddr] = ContractAccount;

  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = 100000;
  Msg.recipient = ContractAddr;
  Msg.sender = SenderAddr;
  Msg.code_address = ContractAddr;

  evmc::Result First = MockedHost->call(Msg);
  ASSERT_EQ(First.status_code, EVMC_SUCCESS);
  EXPECT_EQ(MockedHost->getInternalCallModuleCacheSize(), 1U);

  evmc::Result Second = MockedHost->call(Msg);
  ASSERT_EQ(Second.status_code, EVMC_SUCCESS);
  EXPECT_EQ(MockedHost->getInternalCallModuleCacheSize(), 1U);

  // Same address and codehash but different bytecode must not reuse the cached
  // module. This protects replay tests from stale code when state changes.
  MockedHost->accounts[ContractAddr].code = {0x60, 0x01, 0x50, 0x00};
  evmc::Result Third = MockedHost->call(Msg);
  ASSERT_EQ(Third.status_code, EVMC_SUCCESS);
  EXPECT_EQ(MockedHost->getInternalCallModuleCacheSize(), 2U);
}

// if there is no evm files, we add a special string to make the test run and
// handle it in the test case
auto EvmFiles = getAllEvmBytecodeFiles();
INSTANTIATE_TEST_SUITE_P(
    EVMSamples, EVMSampleTest,
    ::testing::ValuesIn(EvmFiles.empty()
                            ? std::vector<std::string>{"NoEvmHexFiles"}
                            : EvmFiles),
    GetTestName);

// ============================================================================
// EIP-3074 AUTH (0xf6) / AUTHCALL (0xf7) interpreter tests (protocol
// extension).  The signature vectors are pre-generated by
// tools/eip3074_vector_gen.py: pure-Python secp256k1 signing cross-verified
// with OpenSSL 3.0.13 (`pkeyutl -verify`) and the y-parity determined by
// matching the recovered public key against the OpenSSL-derived one.  No
// runtime signature generation.
// ============================================================================
namespace {

constexpr uint64_t AuthChainId = 1;

struct AuthVector {
  const char *Authority; // 20-byte signer address (hex)
  const char *R;         // 32-byte signature component r (hex)
  const char *S;         // 32-byte low-s component (hex)
  const char *SHigh;     // N - s (high-s, must be rejected)
  int V;                 // recovery parity
  uint64_t Nonce;        // signer's host account nonce used when signing
  const char *Commit;    // 32-byte commit (hex)
};

constexpr AuthVector kAuthVecA = {
    "74dab2ae9787539aeb785c0aed16033ce926b384",
    "222450f59d8d339e6dcd3b17b5b0563099e409d8bd0c80d9357f50453216b90c",
    "6cc1257cd312d8e52fcb51bb923df84c10a6aecc701414158dc88e84867b01b5",
    "933eda832ced271ad034ae446dc207b2aa082e1a3f348c263209d00849bb3f8c",
    1,
    0,
    "112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00",
};

constexpr AuthVector kAuthVecB = {
    "74dab2ae9787539aeb785c0aed16033ce926b384",
    "2124788944c783e5daa28086ee3325c4a92a6637c06ce278e7d58d3fdc8098f6",
    "5353e1a25b28be653889c9b53ff67d9594a7f5bffe38be73e04c4f147ca564e3",
    "acac1e5da4d7419ac776364ac00982692606e726b10fe1c7df860f785390dc5e",
    1,
    1,
    "112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00",
};

// The contract executing AUTH (the "invoker" address) the vectors were signed
// for.  Vectors were generated with bytes(12) || 0101...0101 as the invoker.
constexpr evmc::address kAuthInvoker =
    evmc::literals::operator""_address(
        "0101010101010101010101010101010101010101");

const std::string kAuthDigestA =
    "bc655563c4d17f5f7c1257dc413434bd6d2c85f178312aa00c7fe242b550ffc0";

const std::string kAuthCalleeCaller = "3360005260206000f3";
const std::string kAuthCalleeCallValue = "3460005260206000f3";

std::vector<uint8_t> authFromHex(const std::string &Hex) {
  const auto Bytes = zen::utils::fromHex(Hex);
  return Bytes ? *Bytes : std::vector<uint8_t>{};
}

// Append Src to Dst (std::vector has no operator+=).
void authAppend(std::vector<uint8_t> &Dst, const std::vector<uint8_t> &Src) {
  Dst.insert(Dst.end(), Src.begin(), Src.end());
}

// Minimal PUSHn (PUSH1..PUSH32) for exact bytes.
std::vector<uint8_t> authPush(const std::vector<uint8_t> &Data) {
  assert(Data.size() >= 1 && Data.size() <= 32);
  std::vector<uint8_t> Code;
  Code.push_back(static_cast<uint8_t>(0x5f + Data.size()));
  Code.insert(Code.end(), Data.begin(), Data.end());
  return Code;
}

enum class AuthSigMutation {
  None,
  FlipR,
  FlipCommit,
  HighS,
  Parity2,
  ZeroR,
  ZeroS,
};

// Layout: yParity(1) || r(32) || s(32) || commit(32) = 97 bytes.
std::vector<uint8_t> authSigBlock(const AuthVector &V, AuthSigMutation Mut) {
  std::vector<uint8_t> Sig(97, 0);
  Sig[0] = static_cast<uint8_t>(V.V);
  const auto R = authFromHex(V.R);
  const auto S = authFromHex(V.S);
  const auto C = authFromHex(V.Commit);
  std::memcpy(Sig.data() + 1, R.data(), 32);
  std::memcpy(Sig.data() + 33, S.data(), 32);
  std::memcpy(Sig.data() + 65, C.data(), 32);
  switch (Mut) {
  case AuthSigMutation::None:
    break;
  case AuthSigMutation::FlipR:
    Sig[2] ^= 0x01;
    break;
  case AuthSigMutation::FlipCommit:
    Sig[70] ^= 0x01;
    break;
  case AuthSigMutation::HighS: {
    const auto SH = authFromHex(V.SHigh);
    std::memcpy(Sig.data() + 33, SH.data(), 32);
    break;
  }
  case AuthSigMutation::Parity2:
    Sig[0] = 2;
    break;
  case AuthSigMutation::ZeroR:
    std::memset(Sig.data() + 1, 0, 32);
    break;
  case AuthSigMutation::ZeroS:
    std::memset(Sig.data() + 33, 0, 32);
    break;
  }
  return Sig;
}

// MSTORE the 97-byte signature block at memory offset 0 and return the code.
std::vector<uint8_t> authSigStore(const std::vector<uint8_t> &Sig97) {
  std::vector<uint8_t> Code;
  std::vector<uint8_t> W96(Sig97.begin() + 96, Sig97.begin() + 97);
  W96.resize(32, 0);
  authAppend(Code, authPush(W96));
  authAppend(Code, authPush({0x60}));
  authAppend(Code, authFromHex("52")); // MSTORE @96
  authAppend(Code, authPush(std::vector<uint8_t>(Sig97.begin() + 64, Sig97.begin() + 96)));
  authAppend(Code, authPush({0x40}));
  authAppend(Code, authFromHex("52")); // MSTORE @64
  authAppend(Code, authPush(std::vector<uint8_t>(Sig97.begin() + 32, Sig97.begin() + 64)));
  authAppend(Code, authPush({0x20}));
  authAppend(Code, authFromHex("52")); // MSTORE @32
  authAppend(Code, authPush(std::vector<uint8_t>(Sig97.begin(), Sig97.begin() + 32)));
  authAppend(Code, authFromHex("600052")); // PUSH1 0 MSTORE
  return Code;
}

// PUSH1 97 (length) PUSH1 0 (offset) PUSH20 authority AUTH
std::vector<uint8_t> authOp(const std::string &AuthorityHex) {
  std::vector<uint8_t> Code = authFromHex("6061");
  authAppend(Code, authPush({0x00}));
  authAppend(Code, authPush(authFromHex(AuthorityHex)));
  Code.push_back(0xf6);
  return Code;
}

// PUSH1 0 MSTORE PUSH1 32 PUSH1 0 RETURN -> return the top stack word.
std::vector<uint8_t> authReturnWord() { return authFromHex("60005260206000f3"); }

// PUSH1 32 PUSH1 Offset RETURN -> return memory[Offset..Offset+32).
std::vector<uint8_t> authReturnFrom(uint8_t Offset) {
  std::vector<uint8_t> Code = authFromHex("6020");
  authAppend(Code, authPush({Offset}));
  authAppend(Code, authFromHex("f3"));
  return Code;
}

// Contract that writes the signature block, runs AUTH, and returns the AUTH
// success word (0 or 1).
std::vector<uint8_t> authOnlyBytecode(const AuthVector &V, AuthSigMutation Mut,
                                      const std::string &AuthorityHex = "") {
  auto Code = authSigStore(authSigBlock(V, Mut));
  authAppend(Code, authOp(AuthorityHex.empty() ? V.Authority : AuthorityHex));
  authAppend(Code, authReturnWord());
  return Code;
}

// Contract that writes the signature block, runs AUTH, then AUTHCALLs the
// target with the given value (args/ret regions at 0/0, generous gas) and
// returns memory[0..32) (where the callee's output is copied).
std::vector<uint8_t> authCallBytecode(const AuthVector &V, AuthSigMutation Mut,
                                      const std::string &TargetHex,
                                      uint64_t Value) {
  auto Code = authSigStore(authSigBlock(V, Mut));
  authAppend(Code, authOp(V.Authority));
  authAppend(Code, authPush({0x20})); // retSize
  authAppend(Code, authPush({0x80})); // retOffset (away from the sig block)
  authAppend(Code, authPush({0x00})); // argsSize
  authAppend(Code, authPush({0x00})); // argsOffset
  std::vector<uint8_t> ValueBytes(32, 0);
  const auto ValueWord = intx::be::store<evmc::bytes32>(intx::uint256(Value));
  std::memcpy(ValueBytes.data(), ValueWord.bytes, 32);
  authAppend(Code, authPush(ValueBytes)); // value
  authAppend(Code, authPush(authFromHex(TargetHex)));
  authAppend(Code, authPush({0x0F, 0xFF, 0xFF})); // gas
  Code.push_back(0xf7);
  authAppend(Code, authFromHex("50")); // POP the success flag
  authAppend(Code, authReturnFrom(0x80));
  return Code;
}

evmc::address authAddr(const std::string &Hex) {
  const auto B = authFromHex(Hex);
  evmc::address Addr{};
  std::memcpy(Addr.bytes, B.data(), std::min<size_t>(B.size(), 20));
  return Addr;
}

std::string authWordHex(const std::string &Hex20) {
  std::string Result = std::string(24, '0') + Hex20;
  for (auto &C : Result) {
    C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
  }
  return Result;
}

std::string authWord64(uint64_t Value) {
  std::vector<uint8_t> W(32, 0);
  const auto ValueWord = intx::be::store<evmc::bytes32>(intx::uint256(Value));
  std::memcpy(W.data(), ValueWord.bytes, 32);
  return zen::utils::toHex(W.data(), W.size());
}

struct AuthRunResult {
  evmc_status_code Status = EVMC_INTERNAL_ERROR;
  std::string OutputHex;
  int64_t GasLeft = 0;
};

// Runs ContractCode through the interpreter (InterpMode) with the given
// message flags/revision.  HostSetup can install accounts, balances, nonces
// and prewarm accessed accounts before execution.
AuthRunResult runAuthVm(
    const std::vector<uint8_t> &ContractCode, const evmc::address &Recipient,
    const evmc::address &Sender = zen::evm::DEFAULT_DEPLOYER_ADDRESS,
    uint32_t Flags = 0, evmc_revision Revision = zen::evm::DEFAULT_REVISION,
    uint64_t GasLimit = 0,
    const std::function<void(zen::evm::ZenMockedEVMHost *)> &HostSetup = {}) {
  AuthRunResult Res;
  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;
  Config.EnableEvmGasMetering = true;

  auto MockedHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  MockedHost->tx_context.tx_origin = zen::evm::DEFAULT_DEPLOYER_ADDRESS;
  const auto ChainId =
      intx::be::store<evmc::bytes32>(intx::uint256(AuthChainId));
  std::memcpy(MockedHost->tx_context.chain_id.bytes, ChainId.bytes, 32);
  if (HostSetup) {
    HostSetup(MockedHost.get());
  }

  auto RT = Runtime::newEVMRuntime(Config, MockedHost.get());
  if (!RT) {
    return Res;
  }
  MockedHost->setRuntime(RT.get());

  auto ModRet = RT->loadEVMModule("eip3074_auth_test", ContractCode.data(),
                                  ContractCode.size(), Revision);
  if (!ModRet) {
    return Res;
  }
  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  if (!Iso) {
    return Res;
  }

  if (GasLimit == 0) {
    GasLimit = 0xFFFF'FFFF'FFFF;
  }
  const uint64_t ExecutionGas = GasLimit - zen::evm::BASIC_EXECUTION_COST;
  auto InstRet = Iso->createEVMInstance(*Mod, ExecutionGas);
  if (!InstRet) {
    return Res;
  }
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(Revision);

  evmc_message Msg = {
      .kind = EVMC_CALL,
      .flags = Flags,
      .depth = 0,
      .gas = static_cast<int64_t>(ExecutionGas),
      .recipient = Recipient,
      .sender = Sender,
      .input_data = nullptr,
      .input_size = 0,
      .value = {},
      .create2_salt = {},
      .code_address = {},
      .code = reinterpret_cast<const uint8_t *>(Mod->Code),
      .code_size = Mod->CodeSize,
  };

  evmc::Result RawResult{};
  try {
    RT->callEVMMain(*Inst, Msg, RawResult);
  } catch (const std::exception &E) {
    std::cerr << "[runAuthVm] exception during execution: " << E.what()
              << "\n";
    Res.Status = EVMC_INTERNAL_ERROR;
    Res.OutputHex = std::string("exception: ") + E.what();
    return Res;
  }
  Res.Status = RawResult.status_code;
  if (RawResult.output_size > 0 && RawResult.output_data != nullptr) {
    Res.OutputHex =
        zen::utils::toHex(RawResult.output_data, RawResult.output_size);
  }
  Res.GasLeft = RawResult.gas_left;
  return Res;
}

} // namespace

TEST(EIP3074Auth, RevisionNamesAndMetricsTables) {
  const auto *OsakaNames = evmc_get_instruction_names_table(EVMC_OSAKA);
  const auto *PragueNames = evmc_get_instruction_names_table(EVMC_PRAGUE);
  ASSERT_NE(OsakaNames[0xf6], nullptr);
  ASSERT_NE(OsakaNames[0xf7], nullptr);
  EXPECT_STREQ(OsakaNames[0xf6], "AUTH");
  EXPECT_STREQ(OsakaNames[0xf7], "AUTHCALL");
  EXPECT_EQ(PragueNames[0xf6], nullptr);
  EXPECT_EQ(PragueNames[0xf7], nullptr);

  const auto *OsakaMetrics = evmc_get_instruction_metrics_table(EVMC_OSAKA);
  const auto *PragueMetrics = evmc_get_instruction_metrics_table(EVMC_PRAGUE);
  EXPECT_EQ(OsakaMetrics[0xf6].gas_cost, 3100);
  EXPECT_EQ(OsakaMetrics[0xf6].stack_height_required, 3);
  EXPECT_EQ(OsakaMetrics[0xf6].stack_height_change, -2);
  // AUTHCALL mirrors the CALL metrics row: base = warm-storage-read 100.
  EXPECT_EQ(OsakaMetrics[0xf7].gas_cost, 100);
  EXPECT_EQ(OsakaMetrics[0xf7].stack_height_required, 7);
  EXPECT_EQ(OsakaMetrics[0xf7].stack_height_change, -6);
  // Pre-Osaka revisions keep the opcodes undefined.
  EXPECT_EQ(PragueMetrics[0xf6].gas_cost, 0);
  EXPECT_EQ(PragueMetrics[0xf7].gas_cost, 0);
}

TEST(EIP3074Auth, CryptoRecoveryPrimitiveVectors) {
  const auto &V = kAuthVecA;
  const auto Digest = authFromHex(kAuthDigestA);
  const auto R = authFromHex(V.R);
  const auto S = authFromHex(V.S);
  const auto SHigh = authFromHex(V.SHigh);
  std::vector<uint8_t> Zero(32, 0);
  uint8_t Addr[20] = {0};

  ASSERT_TRUE(zen::host::evm::crypto::ec_recover(
      Digest.data(), R.data(), S.data(), static_cast<uint8_t>(V.V), Addr));
  EXPECT_EQ(zen::utils::toHex(Addr, sizeof(Addr)),
            "74DAB2AE9787539AEB785C0AED16033CE926B384");

  // Wrong parity / high-s / zero components must be rejected.
  uint8_t Other[20] = {0};
  EXPECT_FALSE(zen::host::evm::crypto::ec_recover(
      Digest.data(), R.data(), SHigh.data(), static_cast<uint8_t>(V.V), Other));
  EXPECT_FALSE(zen::host::evm::crypto::ec_recover(
      Digest.data(), R.data(), S.data(), 2, Other));
  EXPECT_FALSE(zen::host::evm::crypto::ec_recover(
      Digest.data(), Zero.data(), S.data(), 0, Other));
  EXPECT_FALSE(zen::host::evm::crypto::ec_recover(
      Digest.data(), R.data(), Zero.data(), 0, Other));
}

TEST(EIP3074Auth, HappyPathAuthCallSetsCallerToAuthorized) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  const auto Result = runAuthVm(
      authCallBytecode(V, AuthSigMutation::None, Target, 0), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
        {
          const auto CalleeCode = authFromHex(kAuthCalleeCaller);
          Host->accounts[authAddr(Target)].code.assign(
              CalleeCode.begin(), CalleeCode.end());
        }
      });
  EXPECT_EQ(Result.Status, EVMC_SUCCESS);
  // The callee returned CALLER, which AUTHCALL must have set to the
  // authorized address.
  EXPECT_EQ(Result.OutputHex, authWordHex(V.Authority));
  EXPECT_GT(Result.GasLeft, 0);
}

TEST(EIP3074Auth, AuthColdAccessChargesAdditionalGas) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Code = authOnlyBytecode(V, AuthSigMutation::None);
  // Warm: prewarm the authority account, then execute the same bytecode.
  auto Warm = runAuthVm(
      Code, kAuthInvoker, zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
      zen::evm::DEFAULT_REVISION, 0, [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->access_account(Authority);
      });
  auto Cold = runAuthVm(
      Code, kAuthInvoker, zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
      zen::evm::DEFAULT_REVISION, 0, [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
      });
  ASSERT_EQ(Warm.Status, EVMC_SUCCESS);
  ASSERT_EQ(Cold.Status, EVMC_SUCCESS);
  EXPECT_EQ(Warm.OutputHex, authWord64(1));
  EXPECT_EQ(Cold.OutputHex, authWord64(1));
  // Cold access costs 2600 vs warm 100 -> 2500 extra.
  EXPECT_EQ(Cold.GasLeft, Warm.GasLeft - 2500);
}

TEST(EIP3074Auth, InvalidSignatureClearsAuthorization) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  // AUTH with a tampered r must push 0...
  const auto Only = runAuthVm(
      authOnlyBytecode(V, AuthSigMutation::FlipR), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
      });
  EXPECT_EQ(Only.Status, EVMC_SUCCESS);
  EXPECT_EQ(Only.OutputHex, authWord64(0));
  // ... and the subsequent AUTHCALL must be an invalid execution.
  const auto Flow = runAuthVm(
      authCallBytecode(V, AuthSigMutation::FlipR, Target, 0), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
        {
          const auto CalleeCode = authFromHex(kAuthCalleeCaller);
          Host->accounts[authAddr(Target)].code.assign(
              CalleeCode.begin(), CalleeCode.end());
        }
      });
  EXPECT_EQ(Flow.Status, EVMC_INVALID_INSTRUCTION);
  EXPECT_EQ(Flow.GasLeft, 0);
}

TEST(EIP3074Auth, CommitTamperFails) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Result = runAuthVm(
      authOnlyBytecode(V, AuthSigMutation::FlipCommit), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, authWord64(0));
}

TEST(EIP3074Auth, ChainIdTamperFails) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  // Vector A is signed for chain id 1; run with chain id 2.
  const auto Result = runAuthVm(
      authOnlyBytecode(V, AuthSigMutation::None), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        const auto WrongChain =
            intx::be::store<evmc::bytes32>(intx::uint256(2));
        std::memcpy(Host->tx_context.chain_id.bytes, WrongChain.bytes, 32);
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, authWord64(0));
}

TEST(EIP3074Auth, HostNonceIsPartOfSignedMessage) {
  const auto Authority = authAddr(kAuthVecB.Authority);
  // Vector B is signed for nonce 1: succeeds when the host reports nonce 1...
  const auto Ok = runAuthVm(
      authOnlyBytecode(kAuthVecB, AuthSigMutation::None), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = 1;
      });
  ASSERT_EQ(Ok.Status, EVMC_SUCCESS);
  EXPECT_EQ(Ok.OutputHex, authWord64(1));
  // ... and fails when the host reports a different nonce.
  const auto Wrong = runAuthVm(
      authOnlyBytecode(kAuthVecB, AuthSigMutation::None), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = 2;
      });
  ASSERT_EQ(Wrong.Status, EVMC_SUCCESS);
  EXPECT_EQ(Wrong.OutputHex, authWord64(0));
}

TEST(EIP3074Auth, AuthorityWithCodeFails) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Result = runAuthVm(
      authOnlyBytecode(V, AuthSigMutation::None), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[Authority].code = {0x00};
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, authWord64(0));
}

TEST(EIP3074Auth, WrongAuthorityFails) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  // Pass a different address as the authority: the recovered signer differs.
  const std::string Other = "3333333333333333333333333333333333333333";
  const auto Only = runAuthVm(
      authOnlyBytecode(V, AuthSigMutation::None, Other), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
      });
  ASSERT_EQ(Only.Status, EVMC_SUCCESS);
  EXPECT_EQ(Only.OutputHex, authWord64(0));
}

TEST(EIP3074Auth, HighSParityAndZeroComponentsRejected) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Setup = [&](zen::evm::ZenMockedEVMHost *Host) {
    Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
  };
  auto Run = [&](AuthSigMutation M) {
    return runAuthVm(authOnlyBytecode(V, M), kAuthInvoker,
                     zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
                     zen::evm::DEFAULT_REVISION, 0, Setup);
  };
  for (const auto M : {AuthSigMutation::HighS, AuthSigMutation::Parity2,
                       AuthSigMutation::ZeroR, AuthSigMutation::ZeroS}) {
    const auto R = Run(M);
    ASSERT_EQ(R.Status, EVMC_SUCCESS) << static_cast<int>(M);
    EXPECT_EQ(R.OutputHex, authWord64(0)) << static_cast<int>(M);
  }
}

TEST(EIP3074Auth, AuthCallWithoutAuthIsInvalidExecution) {
  // Build the AUTHCALL flow without the AUTH prefix: only write the sig block
  // and jump straight into the AUTHCALL section.
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  auto Code = authSigStore(authSigBlock(V, AuthSigMutation::None));
  authAppend(Code, authPush({0x20})); // retSize
  authAppend(Code, authPush({0x00})); // retOffset
  authAppend(Code, authPush({0x00})); // argsSize
  authAppend(Code, authPush({0x00})); // argsOffset
  authAppend(Code, authPush({0x0F, 0xFF, 0xFF}));
  authAppend(Code, authPush({0x00})); // value
  authAppend(Code, authPush(authFromHex(Target)));
  authAppend(Code, authPush({0x0F, 0xFF, 0xFF})); // gas
  Code.push_back(0xf7);
  authAppend(Code, authFromHex("50"));
  authAppend(Code, authReturnWord());

  const auto Result = runAuthVm(
      Code, kAuthInvoker, zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
      zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
        {
          const auto CalleeCode = authFromHex(kAuthCalleeCaller);
          Host->accounts[authAddr(Target)].code.assign(
              CalleeCode.begin(), CalleeCode.end());
        }
      });
  EXPECT_EQ(Result.Status, EVMC_INVALID_INSTRUCTION);
  EXPECT_EQ(Result.GasLeft, 0);
}

TEST(EIP3074Auth, StaticModeAllowsAuthButRejectsAuthCall) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Setup = [&](zen::evm::ZenMockedEVMHost *Host) {
    Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
  };
  // AUTH itself is allowed in a static context and still authorizes.
  const auto Only = runAuthVm(authOnlyBytecode(V, AuthSigMutation::None),
                              kAuthInvoker, zen::evm::DEFAULT_DEPLOYER_ADDRESS,
                              EVMC_STATIC, zen::evm::DEFAULT_REVISION, 0,
                              Setup);
  ASSERT_EQ(Only.Status, EVMC_SUCCESS);
  EXPECT_EQ(Only.OutputHex, authWord64(1));

  // AUTHCALL in a static context is rejected (acceptance criterion).
  const std::string Target = "2222222222222222222222222222222222222222";
  const auto Flow = runAuthVm(
      authCallBytecode(V, AuthSigMutation::None, Target, 0), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, EVMC_STATIC,
      zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
        {
          const auto CalleeCode = authFromHex(kAuthCalleeCaller);
          Host->accounts[authAddr(Target)].code.assign(
              CalleeCode.begin(), CalleeCode.end());
        }
      });
  EXPECT_EQ(Flow.Status, EVMC_STATIC_MODE_VIOLATION);
}

TEST(EIP3074Auth, PreOsakaRevisionRejectsOpcode) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Result = runAuthVm(
      authOnlyBytecode(V, AuthSigMutation::None), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, EVMC_PRAGUE, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
      });
  EXPECT_EQ(Result.Status, EVMC_UNDEFINED_INSTRUCTION);
}

TEST(EIP3074Auth, SecondAuthResetsAndRecommitsAuthorization) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  const auto Setup = [&](zen::evm::ZenMockedEVMHost *Host) {
    Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
    Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
    {
          const auto CalleeCode = authFromHex(kAuthCalleeCaller);
          Host->accounts[authAddr(Target)].code.assign(
              CalleeCode.begin(), CalleeCode.end());
        }
  };

  // Good AUTH, then a failing AUTH (tampered commit) must clear the
  // authorization: the following AUTHCALL becomes an invalid execution.
  auto GoodThenBad = authSigStore(authSigBlock(V, AuthSigMutation::None));
  authAppend(GoodThenBad, authOp(V.Authority));
  authAppend(GoodThenBad, authSigStore(authSigBlock(V, AuthSigMutation::FlipCommit)));
  authAppend(GoodThenBad, authOp(V.Authority));
  authAppend(GoodThenBad, authPush({0x20}));
  authAppend(GoodThenBad, authPush({0x80}));
  authAppend(GoodThenBad, authPush({0x00}));
  authAppend(GoodThenBad, authPush({0x00}));
  authAppend(GoodThenBad, authPush({0x00})); // value = 0
  authAppend(GoodThenBad, authPush(authFromHex(Target)));
  authAppend(GoodThenBad, authPush({0x0F, 0xFF, 0xFF}));
  GoodThenBad.push_back(0xf7);
  authAppend(GoodThenBad, authFromHex("50"));
  authAppend(GoodThenBad, authReturnFrom(0x80));
  const auto FailCase = runAuthVm(GoodThenBad, kAuthInvoker,
                                  zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
                                  zen::evm::DEFAULT_REVISION, 0, Setup);
  EXPECT_EQ(FailCase.Status, EVMC_INVALID_INSTRUCTION)
      << "a failed second AUTH must clear the authorization";

  // A second successful AUTH re-authorizes and AUTHCALL succeeds again
  // (AUTHCALL itself does not reset the authorization).
  auto GoodThenGood = authSigStore(authSigBlock(V, AuthSigMutation::None));
  authAppend(GoodThenGood, authOp(V.Authority));
  // First AUTHCALL.
  authAppend(GoodThenGood, authPush({0x20}));
  authAppend(GoodThenGood, authPush({0x80}));
  authAppend(GoodThenGood, authPush({0x00}));
  authAppend(GoodThenGood, authPush({0x00}));
  authAppend(GoodThenGood, authPush({0x00})); // value = 0
  authAppend(GoodThenGood, authPush(authFromHex(Target)));
  authAppend(GoodThenGood, authPush({0x0F, 0xFF, 0xFF}));
  GoodThenGood.push_back(0xf7);
  authAppend(GoodThenGood, authFromHex("50"));
  // Second AUTH (same vector) + second AUTHCALL.
  authAppend(GoodThenGood, authSigStore(authSigBlock(V, AuthSigMutation::None)));
  authAppend(GoodThenGood, authOp(V.Authority));
  authAppend(GoodThenGood, authPush({0x20}));
  authAppend(GoodThenGood, authPush({0x80}));
  authAppend(GoodThenGood, authPush({0x00}));
  authAppend(GoodThenGood, authPush({0x00}));
  authAppend(GoodThenGood, authPush({0x00})); // value = 0
  authAppend(GoodThenGood, authPush(authFromHex(Target)));
  authAppend(GoodThenGood, authPush({0x0F, 0xFF, 0xFF}));
  GoodThenGood.push_back(0xf7);
  authAppend(GoodThenGood, authFromHex("50"));
  authAppend(GoodThenGood, authReturnFrom(0x80));
  const auto GoodCase = runAuthVm(GoodThenGood, kAuthInvoker,
                                  zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
                                  zen::evm::DEFAULT_REVISION, 0, Setup);
  ASSERT_EQ(GoodCase.Status, EVMC_SUCCESS);
  EXPECT_EQ(GoodCase.OutputHex, authWordHex(V.Authority));
}

TEST(EIP3074Auth, AuthCallValueTransferFromAuthorized) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  // Callee returns CALLVALUE: proves the value was forwarded (with the
  // authorized account as the sender).
  const auto Result = runAuthVm(
      authCallBytecode(V, AuthSigMutation::None, Target, 1), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[Authority].balance =
            intx::be::store<evmc::bytes32>(intx::uint256(5));
        Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
        {
          const auto Callee = authFromHex(kAuthCalleeCallValue);
          Host->accounts[authAddr(Target)].code.assign(Callee.begin(),
                                                       Callee.end());
        }
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, authWord64(1));
}

TEST(EIP3074Auth, AuthCallValueExceedsAuthorizedBalanceIsInvalid) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  // The invoker has plenty of balance but the authorized account has none:
  // AUTHCALL must charge the value against the authorized account and fail.
  const auto Result = runAuthVm(
      authCallBytecode(V, AuthSigMutation::None, Target, 1), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[Authority].balance =
            intx::be::store<evmc::bytes32>(intx::uint256(0));
        Host->accounts[zen::evm::DEFAULT_DEPLOYER_ADDRESS].balance =
            intx::be::store<evmc::bytes32>(intx::uint256(1000));
        Host->accounts[authAddr(Target)] = evmc::MockedAccount{};
        {
          const auto Callee = authFromHex(kAuthCalleeCallValue);
          Host->accounts[authAddr(Target)].code.assign(Callee.begin(),
                                                       Callee.end());
        }
      });
  EXPECT_EQ(Result.Status, EVMC_INVALID_INSTRUCTION);
  EXPECT_EQ(Result.GasLeft, 0);
}

TEST(EIP3074Auth, EmptyTargetPaysCreationCost) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const std::string Target = "2222222222222222222222222222222222222222";
  // Both runs target an EMPTY (absent) account and therefore pay the same
  // 25000 account-creation-cost contribution (is_empty semantics).  The only
  // gas difference between them is the EIP-3074 value-transfer cost (6700).
  const auto NoValue = runAuthVm(
      authCallBytecode(V, AuthSigMutation::None, Target, 0), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[Authority].balance =
            intx::be::store<evmc::bytes32>(intx::uint256(5));
      });
  const auto WithValue = runAuthVm(
      authCallBytecode(V, AuthSigMutation::None, Target, 1), kAuthInvoker,
      zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0, zen::evm::DEFAULT_REVISION, 0,
      [&](zen::evm::ZenMockedEVMHost *Host) {
        Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
        Host->accounts[Authority].balance =
            intx::be::store<evmc::bytes32>(intx::uint256(5));
      });
  ASSERT_EQ(NoValue.Status, EVMC_SUCCESS);
  ASSERT_EQ(WithValue.Status, EVMC_SUCCESS);
  EXPECT_EQ(WithValue.GasLeft, NoValue.GasLeft - AUTHCALL_VALUE_COST);
}

TEST(EIP3074Auth, AuthLengthBoundaries) {
  const auto &V = kAuthVecA;
  const auto Authority = authAddr(V.Authority);
  const auto Setup = [&](zen::evm::ZenMockedEVMHost *Host) {
    Host->accounts[Authority].nonce = static_cast<int>(V.Nonce);
  };
  // length = 128 (> 97): extra bytes ignored but still charged; the
  // signature is at offset 0 so the message is identical -> success.
  auto Long = authSigStore(authSigBlock(V, AuthSigMutation::None));
  authAppend(Long, authFromHex("6080")); // PUSH1 128 (length)
  authAppend(Long, authPush({0x00}));    // offset
  authAppend(Long, authPush(authFromHex(V.Authority)));
  Long.push_back(0xf6);
  authAppend(Long, authReturnWord());
  const auto LongResult = runAuthVm(Long, kAuthInvoker,
                                    zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
                                    zen::evm::DEFAULT_REVISION, 0, Setup);
  ASSERT_EQ(LongResult.Status, EVMC_SUCCESS);
  EXPECT_EQ(LongResult.OutputHex, authWord64(1));

  // length = 90 (< 97): the trailing commit bytes are zero-padded, making
  // the signed message different -> AUTH fails.
  auto Short = authSigStore(authSigBlock(V, AuthSigMutation::None));
  authAppend(Short, authFromHex("605a")); // PUSH1 90 (length)
  authAppend(Short, authPush({0x00}));
  authAppend(Short, authPush(authFromHex(V.Authority)));
  Short.push_back(0xf6);
  authAppend(Short, authReturnWord());
  const auto ShortResult = runAuthVm(Short, kAuthInvoker,
                                     zen::evm::DEFAULT_DEPLOYER_ADDRESS, 0,
                                     zen::evm::DEFAULT_REVISION, 0, Setup);
  ASSERT_EQ(ShortResult.Status, EVMC_SUCCESS);
  EXPECT_EQ(ShortResult.OutputHex, authWord64(0));
}

#ifdef ZEN_ENABLE_MULTIPASS_JIT
TEST(EVMMultipassLinearPrecheckTest, MemoryLinearMloadStepUsesNonZeroStride) {
  const auto FilePath =
      (getEvmAsmDirPath() / "memory_linear_mload_step.evm.hex").string();
  auto Exec = executeEvmBytecodeFile(FilePath, common::RunMode::MultipassMode,
                                     makeUint256Calldata(0x20));

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(Exec.JITCompiled);
#endif
  EXPECT_EQ(Exec.Status, EVMC_SUCCESS);
  EXPECT_EQ(Exec.OutputHex,
            "0000000000000000000000000000000000000000000000000000000000000080");
}

TEST(EVMMultipassLinearPrecheckTest, MemoryLinearMstoreStepUsesNonZeroStride) {
  const auto FilePath =
      (getEvmAsmDirPath() / "memory_linear_mstore_step.evm.hex").string();
  auto Exec = executeEvmBytecodeFile(FilePath, common::RunMode::MultipassMode,
                                     makeUint256Calldata(0x20));

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(Exec.JITCompiled);
#endif
  EXPECT_EQ(Exec.Status, EVMC_SUCCESS);
  EXPECT_EQ(Exec.OutputHex,
            "0000000000000000000000000000000000000000000000000000000000000080");
}

TEST(EVMMultipassLinearPrecheckTest,
     MemoryLinearMstoreOverlapStride8PreservesSemantics) {
  expectMemoryLinearMstoreOverlapResult(
      0x08, "0000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000020");
}

TEST(EVMMultipassLinearPrecheckTest,
     MemoryLinearMstoreOverlapStride16PreservesSemantics) {
  expectMemoryLinearMstoreOverlapResult(
      0x10, "0000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000040");
}

TEST(EVMMultipassLinearPrecheckTest,
     MemoryLinearMstoreOverlapStride24DisablesElisionButPreservesSemantics) {
  expectMemoryLinearMstoreOverlapResult(
      0x18, "0000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000060");
}

TEST(EVMMultipassDisplacedBytes32Test,
     MemoryConstMloadAboveI32DisplacementLimitCompiles) {
  const std::vector<uint8_t> Bytecode = {0x63, 0x7f, 0xff, 0xff,
                                         0xe8, 0x51, 0x00};
  expectMultipassJitModuleLoads("memory_const_mload_i32_disp_limit", Bytecode);
}

TEST(EVMMultipassDisplacedBytes32Test,
     MemoryConstMstoreAboveI32DisplacementLimitCompiles) {
  const std::vector<uint8_t> Bytecode = {0x60, 0x01, 0x63, 0x7f, 0xff,
                                         0xff, 0xe8, 0x52, 0x00};
  expectMultipassJitModuleLoads("memory_const_mstore_i32_disp_limit", Bytecode);
}

TEST(EVMMultipassDisplacedBytes32Test,
     MemoryConstMloadAboveI32DisplacementLimitReturnsOutOfGas) {
  const std::vector<uint8_t> Bytecode = {0x63, 0x7f, 0xff, 0xff,
                                         0xe8, 0x51, 0x00};
  auto Exec =
      executeEvmBytecode("memory_const_mload_i32_disp_limit_oog", Bytecode,
                         common::RunMode::MultipassMode, {}, 1'000'000);

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(Exec.JITCompiled);
#endif
  EXPECT_EQ(Exec.Status, EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassDisplacedBytes32Test,
     MemoryConstMstoreAboveI32DisplacementLimitReturnsOutOfGas) {
  const std::vector<uint8_t> Bytecode = {0x60, 0x01, 0x63, 0x7f, 0xff,
                                         0xff, 0xe8, 0x52, 0x00};
  auto Exec =
      executeEvmBytecode("memory_const_mstore_i32_disp_limit_oog", Bytecode,
                         common::RunMode::MultipassMode, {}, 1'000'000);

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(Exec.JITCompiled);
#endif
  EXPECT_EQ(Exec.Status, EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassCopyHelperTest,
     CallDataCopyMatchesInterpreterAfterMemoryPreExpand) {
  auto BytecodeBuf = zen::utils::fromHex("6030601060203760406020f3");
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse calldata-copy bytecode";

  const std::vector<uint8_t> CallData = makeIncrementingBytes(80);
  std::vector<uint8_t> ExpectedOutput;
  ExpectedOutput.insert(ExpectedOutput.end(), CallData.begin() + 16,
                        CallData.begin() + 64);
  ExpectedOutput.resize(64, 0);

  expectInterpMatchesMultipass(
      "calldatacopy_preexpand", *BytecodeBuf, CallData, EVMC_SUCCESS,
      zen::utils::toHex(ExpectedOutput.data(), ExpectedOutput.size()));
}

TEST(EVMMultipassCopyHelperTest,
     CallDataCopyZeroSizeWithOversizedDestRemainsNoOp) {
  auto BytecodeBuf = zen::utils::fromHex(
      "60006000"
      "7f0100000000000000000000000000000000000000000000000000000000000000"
      "3760006000f3");
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse zero-size calldata-copy bytecode";

  expectInterpMatchesMultipass("calldatacopy_zero_size_oversized_dest",
                               *BytecodeBuf, {}, EVMC_SUCCESS);
}

TEST(EVMMultipassCopyHelperTest,
     CallDataCopyNonZeroSizeWithOversizedDestReturnsOutOfGas) {
  auto BytecodeBuf = zen::utils::fromHex(
      "60016000"
      "7f0100000000000000000000000000000000000000000000000000000000000000"
      "3700");
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse oversized calldata-copy bytecode";

  expectInterpMatchesMultipass("calldatacopy_nonzero_size_oversized_dest",
                               *BytecodeBuf, {}, EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassCopyHelperTest,
     CodeCopyMatchesInterpreterAfterMemoryPreExpand) {
  auto BytecodeBuf = zen::utils::fromHex("6008600060003960206000f3");
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse code-copy bytecode";

  std::vector<uint8_t> ExpectedOutput = {0x60, 0x08, 0x60, 0x00,
                                         0x60, 0x00, 0x39, 0x60};
  ExpectedOutput.resize(32, 0);

  expectInterpMatchesMultipass(
      "codecopy_preexpand", *BytecodeBuf, {}, EVMC_SUCCESS,
      zen::utils::toHex(ExpectedOutput.data(), ExpectedOutput.size()));
}

TEST(EVMMultipassCopyHelperTest,
     CodeCopyNonZeroSizeWithOversizedDestReturnsOutOfGas) {
  auto BytecodeBuf = zen::utils::fromHex(
      "60016000"
      "7f0100000000000000000000000000000000000000000000000000000000000000"
      "3900");
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse oversized code-copy bytecode";

  expectInterpMatchesMultipass("codecopy_nonzero_size_oversized_dest",
                               *BytecodeBuf, {}, EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassKeccakHelperTest,
     CallerConstSlotHelperMatchesInterpreterAndExpectedDigest) {
  auto BytecodeBuf =
      zen::utils::fromHex("336000526005602052604060002060005260206000f3");
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse caller-slot helper bytecode";

  const std::string ExpectedDigest = computeTwoWordKeccakHex(
      makePaddedAddressWord(DEFAULT_DEPLOYER_ADDRESS), makeUint256Calldata(5));

  expectInterpMatchesMultipass("keccak_caller_const_slot", *BytecodeBuf, {},
                               EVMC_SUCCESS, ExpectedDigest);
}

TEST(EVMMultipassKeccakHelperTest,
     CallDataConstSlotHelperMatchesInterpreterAndExpectedDigest) {
  auto BytecodeBuf =
      zen::utils::fromHex("6000356000526007602052604060002060005260206000f3");
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse calldata-slot helper bytecode";

  const std::vector<uint8_t> CallData = makeUint256Calldata(0x1234);
  const std::string ExpectedDigest =
      computeTwoWordKeccakHex(CallData, makeUint256Calldata(7));

  expectInterpMatchesMultipass("keccak_calldata_const_slot", *BytecodeBuf,
                               CallData, EVMC_SUCCESS, ExpectedDigest);
}

TEST(EVMMultipassKeccakHelperTest,
     CallDataConstSlotHelperMatchesInterpreterWithNonZeroStagingBase) {
  auto BytecodeBuf =
      zen::utils::fromHex("6000356040526007606052604060402060005260206000f3");
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse calldata-slot nonzero-base bytecode";

  const std::vector<uint8_t> CallData = makeUint256Calldata(0x1234);
  const std::string ExpectedDigest =
      computeTwoWordKeccakHex(CallData, makeUint256Calldata(7));

  expectInterpMatchesMultipass("keccak_calldata_const_slot_nonzero_base",
                               *BytecodeBuf, CallData, EVMC_SUCCESS,
                               ExpectedDigest);
}

TEST(EVMMultipassKeccakHelperTest,
     CallerConstSlotHelperPreservesMemoryExpansionFailureSemantics) {
  auto BytecodeBuf = zen::utils::fromHex(
      "3362ffffe0526005630100000052604062ffffe02060005260206000f3");
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse caller-slot memory edge bytecode";

  expectInterpMatchesMultipass("keccak_caller_const_slot_mem_oog", *BytecodeBuf,
                               {}, EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassKeccakHelperTest,
     CallDataConstSlotHelperPreservesMemoryExpansionFailureSemantics) {
  auto BytecodeBuf = zen::utils::fromHex(
      "60003562ffffe0526005630100000052604062ffffe02060005260206000f3");
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse calldata-slot memory edge bytecode";

  expectInterpMatchesMultipass("keccak_calldata_const_slot_mem_oog",
                               *BytecodeBuf, makeUint256Calldata(0x1234),
                               EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassKeccakHelperTest,
     GenericKeccakPreExpandMatchesInterpreterAndExpectedDigest) {
  std::vector<uint8_t> Word(32);
  for (size_t I = 0; I < Word.size(); ++I) {
    Word[I] = static_cast<uint8_t>(I);
  }

  std::vector<uint8_t> Bytecode = {0x7f};
  Bytecode.insert(Bytecode.end(), Word.begin(), Word.end());
  const std::vector<uint8_t> Suffix = {
      0x60, 0x00, 0x52,       // MSTORE word at memory offset 0.
      0x60, 0x30, 0x60, 0x00, // KECCAK256 memory[0:48].
      0x20, 0x60, 0x00, 0x52, // Store digest at memory offset 0.
      0x60, 0x20, 0x60, 0x00, 0xf3};
  Bytecode.insert(Bytecode.end(), Suffix.begin(), Suffix.end());

  std::vector<uint8_t> KeccakInput = Word;
  KeccakInput.insert(KeccakInput.end(), 16, 0);
  const std::string ExpectedDigest = computeKeccakHex(KeccakInput);

  expectInterpMatchesMultipass("keccak_generic_preexpand_48", Bytecode, {},
                               EVMC_SUCCESS, ExpectedDigest);
}

TEST(EVMMultipassKeccakHelperTest,
     GenericKeccakZeroLengthAllowsOversizedOffset) {
  std::vector<uint8_t> Bytecode = {0x60, 0x00, 0x7f, 0x01};
  Bytecode.insert(Bytecode.end(), 31, 0x00);
  const std::vector<uint8_t> Suffix = {0x20, 0x60, 0x00, 0x52,
                                       0x60, 0x20, 0x60, 0x00};
  Bytecode.insert(Bytecode.end(), Suffix.begin(), Suffix.end());
  Bytecode.push_back(0xf3);

  const std::vector<uint8_t> EmptyInput;
  const std::string ExpectedDigest = computeKeccakHex(EmptyInput);

  expectInterpMatchesMultipass("keccak_zero_size_oversized_offset", Bytecode,
                               {}, EVMC_SUCCESS, ExpectedDigest);
}

TEST(EVMMultipassKeccakHelperTest,
     GenericKeccakNonZeroLengthRejectsOversizedOffset) {
  std::vector<uint8_t> Bytecode = {0x60, 0x01, 0x7f, 0x01};
  Bytecode.insert(Bytecode.end(), 31, 0x00);
  Bytecode.push_back(0x20);
  Bytecode.push_back(0x00);

  expectInterpMatchesMultipass("keccak_nonzero_oversized_offset", Bytecode, {},
                               EVMC_OUT_OF_GAS);
}

TEST(EVMMultipassJumpRegressionTest, InvalidJumpDestStillMatchesInterpreter) {
  const std::vector<uint8_t> Bytecode = {0x60, 0x04, 0x56, 0x00, 0x00};

  expectInterpMatchesMultipass("invalid_jumpdest_regression", Bytecode, {},
                               EVMC_BAD_JUMP_DESTINATION);
}

TEST(EVMMultipassJumpRegressionTest,
     HighLimbNonZeroJumpTargetStillRejectsOtherwiseValidLowDest) {
  std::vector<uint8_t> Bytecode = {0x7f, 0x01};
  Bytecode.insert(Bytecode.end(), 30, 0x00);
  Bytecode.push_back(0x22);
  Bytecode.push_back(0x56);
  Bytecode.push_back(0x5b);
  Bytecode.push_back(0x00);

  expectInterpMatchesMultipass("high_limb_jump_target_regression", Bytecode, {},
                               EVMC_BAD_JUMP_DESTINATION);
}

// Regression test for issue #487: multipass JIT corrupted high limbs of U256
// values written via SSTORE. Shared zero-constant MInstructions caused the
// register allocator to spill them across long live ranges; stale stack slots
// produced garbage in limbs 1-3.
TEST(EVMMultipassSstoreTest, Issue487_U256HighLimbsNotCorrupted) {
  const std::string BytecodeHex =
      "60005047585c816e0000000000000000000000000000125c6d000000000000000000"
      "000000a3485179000000000000000000000000000000000000000000a68804c0cf0a"
      "680000000000000000ef31841a097000000000000000000000000000000000c7911a"
      "1c08760000000000000000000000000000000000000000000014355f0860e3337600"
      "0000000000000000000000000000000000000000626e541c05053d6c000000000000"
      "000000000000c4720000000000000000000000000000000000006e3d770000000000"
      "000000000000000000000000000000000000a06300006f913d145d1a900330"
      "7a0000000000000000000000000000000000000000000000005f6f5c3f7c00000000"
      "000000000000000000000000000000000000000000000000b9620000ab5808634200"
      "0000556342000001556342000002556342000003556c00000000000000000000004115"
      "553d385f5f5f0a3979000000000000000000000000000000000000000000000000f6"
      "925e6168e65842453650387900000000000000000000000000000000000000000000"
      "000000db6e0000000000000000000000000000aa1005493649845e47906342000000"
      "556342000001556342000002557e00000000000000000000000000000000000000000"
      "00000000000000000018b5c79000000000000000000000000000000000000000000000"
      "00000d307634200000055634200000155634200000255";

  const std::string CalldataHex =
      "0dba1bece48614fcdabf80dc0a3d1d180b641b5a9fe0a3092ad29c772b066210"
      "e553242e7e1ad9bf1bde48e1cce998dfe1aeebf268ec679f3ca10ade95016a8d"
      "527bdf705a729d7616799a1f5806";

  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse bytecode hex";
  auto CalldataBuf = zen::utils::fromHex(CalldataHex);
  ASSERT_TRUE(CalldataBuf) << "Failed to parse calldata hex";

  RuntimeConfig Config;
  Config.Mode = common::RunMode::MultipassMode;
  Config.EnableEvmGasMetering = true;

  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "00000000000000000000000000000000000000f1");
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();

  evmc::MockedAccount ContractAccount;
  ContractAccount.code = {0x60, 0x00, 0x50};

  evmc::MockedAccount SenderAccount;
  SenderAccount.set_balance(0xFFFFFFFFFF);

  HostPtr->accounts[ContractAddr] = ContractAccount;
  HostPtr->accounts[SenderAddr] = SenderAccount;

  evmc_tx_context TxCtx{};
  TxCtx.tx_origin = SenderAddr;
  HostPtr->tx_context = TxCtx;

  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  ASSERT_TRUE(RT != nullptr) << "Failed to create runtime";
  HostPtr->setRuntime(RT.get());

  const std::string ModuleName = "issue487_reproducer";
  auto ModRet =
      RT->loadEVMModule(ModuleName, BytecodeBuf->data(), BytecodeBuf->size());
  ASSERT_TRUE(ModRet) << "Failed to load module";
  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  ASSERT_TRUE(Iso != nullptr) << "Failed to create isolation";

  constexpr uint64_t GasLimit = 8000000;
  const uint64_t IntrinsicGas = zen::evm::BASIC_EXECUTION_COST;
  const uint64_t ExecutionGasLimit = GasLimit - IntrinsicGas;

  auto InstRet = Iso->createEVMInstance(*Mod, ExecutionGasLimit);
  ASSERT_TRUE(InstRet) << "Failed to create instance";
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(EVMC_CANCUN);

  evmc_message Msg = {
      .kind = EVMC_CALL,
      .flags = 0u,
      .depth = 0,
      .gas = static_cast<int64_t>(ExecutionGasLimit),
      .recipient = ContractAddr,
      .sender = SenderAddr,
      .input_data = CalldataBuf->data(),
      .input_size = CalldataBuf->size(),
      .value = {},
      .create2_salt = {},
      .code_address = ContractAddr,
      .code = reinterpret_cast<const uint8_t *>(Mod->Code),
      .code_size = Mod->CodeSize,
  };

  evmc::Result RawResult;
  EXPECT_NO_THROW({ RT->callEVMMain(*Inst, Msg, RawResult); });
  ASSERT_EQ(RawResult.status_code, EVMC_SUCCESS)
      << "EVM execution failed with status code "
      << static_cast<int>(RawResult.status_code);

  // Verify SSTORE wrote correct U256 values with clean high limbs.
  auto makeKey = [](uint64_t low) {
    evmc::bytes32 Key{};
    for (int I = 0; I < 8; ++I) {
      Key.bytes[31 - I] = static_cast<uint8_t>(low & 0xFF);
      low >>= 8;
    }
    return Key;
  };

  auto checkStorageValue = [&](uint64_t KeyLow, uint64_t ExpectedLow,
                               const std::string &Label) {
    const evmc::bytes32 Key = makeKey(KeyLow);
    const auto &Storage = HostPtr->accounts[ContractAddr].storage;
    auto It = Storage.find(Key);
    ASSERT_NE(It, Storage.end()) << Label << ": key not found in storage";
    const evmc::bytes32 &Value = It->second.current;
    // All high bytes (0..23) must be zero - no garbage in upper limbs.
    for (int I = 0; I < 24; ++I) {
      EXPECT_EQ(Value.bytes[I], 0)
          << Label << ": non-zero byte at position " << I;
    }
    uint64_t ActualLow = 0;
    for (int I = 24; I < 32; ++I) {
      ActualLow = (ActualLow << 8) | Value.bytes[I];
    }
    EXPECT_EQ(ActualLow, ExpectedLow) << Label << ": low value mismatch";
  };

  checkStorageValue(0x42000001, 0x179, "slot_0x42000001");
  checkStorageValue(0x42000002, 0x68E6, "slot_0x42000002");
  checkStorageValue(0x42000003, 0xC4, "slot_0x42000003");
}

// Regression test for issue #488.
//
// Before the fix, EVMMirBuilder::handlePC produced a raw `const i64`
// MInstruction whose result virtual register was reused across basic blocks
// through the x86 lowering's expression cache (_expr_reg_map). When PC was
// later consumed by the slow path of ADDMOD (which spills the U256 augend
// through setInstanceElement in a different basic block), the cached vreg had
// been clobbered in between, so the runtime helper evmGetAddMod read a stale
// heap pointer instead of the PC value. The result was a divergence between
// the interpreter (correct) and the multipass JIT (incorrect, often throwing
// Unreachable).
//
// The fix spills the PC constant through a temporary variable in handlePC so
// each consumer re-reads it via dread.
TEST(EVMRegressionTest, Issue488_PCAsAddmodAugend_InterpMatchesMultipass) {
  const auto FilePath =
      (getEvmAsmDirPath() / "addmod_pc_augend.evm.hex").string();

  auto InterpExec =
      executeEvmBytecodeFile(FilePath, common::RunMode::InterpMode);
  auto MultipassExec =
      executeEvmBytecodeFile(FilePath, common::RunMode::MultipassMode);

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled)
      << "Multipass JIT should compile addmod_pc_augend";
#endif

  EXPECT_EQ(InterpExec.Status, EVMC_SUCCESS);
  EXPECT_EQ(MultipassExec.Status, InterpExec.Status)
      << "Multipass status diverged from interpreter for issue #488 "
         "regression";
  EXPECT_EQ(MultipassExec.OutputHex, InterpExec.OutputHex)
      << "Multipass output diverged from interpreter for issue #488 "
         "regression";

  // (PC=4) + 0x10 = 20, 20 % 7 = 6, returned as a 32-byte big-endian word.
  EXPECT_EQ(InterpExec.OutputHex,
            "0000000000000000000000000000000000000000000000000000000000000006");
}

// Regression test for issue #541 (and #542): multipass JIT carry chain
// corruption when the last ADC in handleAddU64Const is not
// protectUnsafeValue'd.
//
// When ADD produces a U256 result via the handleAddU64Const fast path
// (ADD limb[0] + 3×ADC for carry propagation), the last ADC (limb[3]) was
// intentionally left as an un-materialized tree-IR expression because the
// carry flag is "dead" within the carry chain after that instruction.
// However, if the ADD result is later consumed by a CMP instruction (e.g.
// from GT/LT comparison), the CMP lowers before the last ADC, clobbering
// x86 EFLAGS (including CF). The ADC then reads the wrong CF from CMP
// instead of the correct CF from the preceding ADC, corrupting the carry
// chain.
//
// The test uses a minimal EVM program that triggers the bug:
//   PUSH24 big_val  -- a 192-bit value with non-zero limbs
//   PUSH1 0xff      -- a small value (U64 range)
//   RETURNDATASIZE  -- pushes 0 (U64 range)
//   ADD             -- ADD(0, 0xff) via handleAddU64Const
//   GT              -- GT(0xff, big_val) should return 0
//   PUSH0           -- offset for MSTORE
//   MSTORE          -- store GT result to memory
//   PUSH1 32        -- size for RETURN
//   PUSH0           -- offset for RETURN
//   RETURN
//
// Before the fix: CMP from GT clobbered CF before the last ADC was lowered,
// ADC computed wrong limb[3], GT incorrectly returned 1 instead of 0.
// For issue #542: the same root cause also caused a crash.
TEST(EVMRegressionTest, Issue541_AddU64ConstLastAdcCarryChainPreserved) {
  // Bytecode: PUSH24 big_val, PUSH1 0xff, RETURNDATASIZE, ADD, GT,
  //           PUSH0, MSTORE, PUSH1 32, PUSH0, RETURN
  //
  // big_val = 0x0000_0000_0000_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF
  // (3 non-zero limbs to force CMP in GT to set CF=1)
  const std::string BytecodeHex =
      "77000000000000ffffffffffffffffffffffffffffffffffff"
      "60ff3d01115f5260205ff3";

  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse bytecode hex";

  // Run interpreter (reference) and multipass JIT, compare outputs.
  auto InterpExec = executeEvmBytecode("issue541_interp", *BytecodeBuf,
                                       common::RunMode::InterpMode);
  ASSERT_EQ(InterpExec.Status, EVMC_SUCCESS) << "Interpreter execution failed";

  auto MultipassExec = executeEvmBytecode("issue541_multipass", *BytecodeBuf,
                                          common::RunMode::MultipassMode);
  ASSERT_EQ(MultipassExec.Status, EVMC_SUCCESS)
      << "Multipass JIT execution failed (crash = issue #542)";

#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled)
      << "Multipass JIT should compile issue541 reproducer";
#endif

  // GT(0xff, big_val) should return 0. Before the fix, multipass returned 1.
  EXPECT_EQ(MultipassExec.OutputHex, InterpExec.OutputHex)
      << "Multipass output diverged from interpreter for issue #541 "
         "regression";

  // Explicitly verify: GT(0xff, big_val) = 0 (0xff is NOT greater than
  // a 192-bit value).
  EXPECT_EQ(InterpExec.OutputHex,
            "0000000000000000000000000000000000000000000000000000000000000000");
}
#endif

// Test that chain_id and blob_base_fee can be saved and loaded via state
TEST(EVMStateSaveLoad, ChainIdAndBlobBaseFee) {
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();

  // Set chain_id and blob_base_fee to non-zero values
  const std::string ChainIdHex =
      "0000000000000000000000000000000000000000000000000000000000000007";
  const std::string BlobBaseFeeHex =
      "0000000000000000000000000000000000000000000000000000000000000001";

  Host->tx_context.chain_id = zen::utils::parseUint256(ChainIdHex);
  Host->tx_context.blob_base_fee = zen::utils::parseUint256(BlobBaseFeeHex);

  // Set other tx_context fields to make the state complete
  Host->tx_context.tx_gas_price = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000000");
  Host->tx_context.block_number = 1;
  Host->tx_context.block_timestamp = 1;
  Host->tx_context.block_gas_limit = 10000000;
  Host->tx_context.tx_origin = evmc::address{};
  Host->tx_context.block_coinbase = evmc::address{};
  Host->tx_context.block_prev_randao = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000000");
  Host->tx_context.block_base_fee = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000000");

  // Add a simple account
  evmc::address Addr = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  evmc::MockedAccount Account;
  Account.set_balance(100);
  Account.code = {0x60, 0x00, 0x50};
  Host->accounts[Addr] = Account;

  const std::string StateFilePath = "/tmp/dtvm_test_chainid_state.json";

  // Save state
  ASSERT_TRUE(zen::utils::saveState(*Host, StateFilePath));

  // Load state into a new host
  auto NewHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*NewHost, StateFilePath));

  // Verify chain_id was loaded correctly
  auto ExpectedChainId = zen::utils::parseUint256(ChainIdHex);
  EXPECT_EQ(std::memcmp(NewHost->tx_context.chain_id.bytes,
                        ExpectedChainId.bytes, 32),
            0)
      << "chain_id not loaded correctly from state file";

  // Verify blob_base_fee was loaded correctly
  auto ExpectedBlobBaseFee = zen::utils::parseUint256(BlobBaseFeeHex);
  EXPECT_EQ(std::memcmp(NewHost->tx_context.blob_base_fee.bytes,
                        ExpectedBlobBaseFee.bytes, 32),
            0)
      << "blob_base_fee not loaded correctly from state file";

  // Cleanup
  std::filesystem::remove(StateFilePath);
}

// Test that loadState handles missing chain_id and blob_base_fee gracefully
// (backward compatibility: old state.json without these fields should still
// work)
TEST(EVMStateSaveLoad, MissingChainIdAndBlobBaseFee) {
  const std::string StateFilePath = "/tmp/dtvm_test_missing_chainid_state.json";

  // Use saveState to produce a valid complete JSON, then remove chain_id
  // and blob_base_fee lines to simulate an old-format state file.
  {
    auto SaveHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
    SaveHost->tx_context.tx_gas_price = evmc::uint256be{};
    SaveHost->tx_context.block_number = 1;
    SaveHost->tx_context.block_timestamp = 1;
    SaveHost->tx_context.block_gas_limit = 10000000;
    SaveHost->tx_context.tx_origin = evmc::address{};
    SaveHost->tx_context.block_coinbase = evmc::address{};
    SaveHost->tx_context.block_prev_randao = evmc::uint256be{};
    SaveHost->tx_context.block_base_fee = evmc::uint256be{};
    ASSERT_TRUE(zen::utils::saveState(*SaveHost, StateFilePath));

    // Read file, remove chain_id and blob_base_fee lines, rewrite
    std::ifstream InFile(StateFilePath);
    std::string Line;
    std::string Result;
    while (std::getline(InFile, Line)) {
      if (Line.find("\"chain_id\"") != std::string::npos ||
          Line.find("\"blob_base_fee\"") != std::string::npos) {
        continue;
      }
      // Remove trailing comma from tx_origin line (now last field)
      if (Line.find("\"tx_origin\"") != std::string::npos) {
        auto CommaPos = Line.rfind(",");
        if (CommaPos != std::string::npos) {
          Line.erase(CommaPos, 1);
        }
      }
      Result += Line + "\n";
    }
    InFile.close();

    std::ofstream OutFile(StateFilePath);
    OutFile << Result;
  }

  // Load state into a new host
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, StateFilePath));

  // Verify chain_id and blob_base_fee remain default (zero)
  evmc::uint256be ZeroValue{};
  EXPECT_EQ(std::memcmp(Host->tx_context.chain_id.bytes, ZeroValue.bytes, 32),
            0)
      << "Missing chain_id should default to zero";
  EXPECT_EQ(
      std::memcmp(Host->tx_context.blob_base_fee.bytes, ZeroValue.bytes, 32), 0)
      << "Missing blob_base_fee should default to zero";

  // Cleanup
  std::filesystem::remove(StateFilePath);
}

// ---------------------------------------------------------------------------
// Regression tests: loadState() must preserve the caller's existing
// tx_context when the state file has no tx_context member (or omits fields),
// only overlaying the fields that are actually present in the JSON. The CLI
// sets tx_origin from --sender before loadState() and relies on the state file
// to override it only when tx_origin is present; minimal/old state files must
// not silently zero the transaction context.
// ---------------------------------------------------------------------------
TEST(EVMStateSaveLoad, MissingTxContextPreservesCallerTxContext) {
  const std::string FilePath = "/tmp/dtvm_test_missing_tx_context_state.json";

  // Minimal old-format state file: accounts only, no tx_context member.
  const std::string StateJson = R"({
  "accounts": {
    "a94f5374fce5edbc8e2a8697c15331677e6ebf0b": {
      "balance": "0000000000000000000000000000000000000000000000000000000000000001",
      "nonce": 0, "code": "", "codehash": "0000000000000000000000000000000000000000000000000000000000000000", "storage": {}
    }
  }
})";
  {
    std::ofstream OutFile(FilePath);
    OutFile << StateJson;
  }

  // Pre-set the transaction context the same way the CLI does with --sender.
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  const evmc::address Sender = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::address Coinbase = evmc::literals::operator""_address(
      "b94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::uint256be GasPrice = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000010");
  const evmc::uint256be PrevRandao = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000200000");
  const evmc::uint256be BaseFee = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000007");
  const evmc::uint256be ChainId = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000005");
  const evmc::uint256be BlobBaseFee = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000003");

  Host->tx_context.tx_origin = Sender;
  Host->tx_context.tx_gas_price = GasPrice;
  Host->tx_context.block_number = 42;
  Host->tx_context.block_timestamp = 1700000000;
  Host->tx_context.block_gas_limit = 30000000;
  Host->tx_context.block_coinbase = Coinbase;
  Host->tx_context.block_prev_randao = PrevRandao;
  Host->tx_context.block_base_fee = BaseFee;
  Host->tx_context.chain_id = ChainId;
  Host->tx_context.blob_base_fee = BlobBaseFee;

  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath))
      << "A state file without tx_context must still load";

  // The caller's whole transaction context must survive untouched.
  EXPECT_EQ(std::memcmp(Host->tx_context.tx_origin.bytes, Sender.bytes, 20), 0)
      << "tx_origin from the caller must be preserved when the state file has "
         "no tx_context";
  EXPECT_EQ(
      std::memcmp(Host->tx_context.tx_gas_price.bytes, GasPrice.bytes, 32), 0)
      << "gas_price from the caller must be preserved";
  EXPECT_EQ(Host->tx_context.block_number, 42);
  EXPECT_EQ(Host->tx_context.block_timestamp, 1700000000);
  EXPECT_EQ(Host->tx_context.block_gas_limit, 30000000);
  EXPECT_EQ(
      std::memcmp(Host->tx_context.block_coinbase.bytes, Coinbase.bytes, 20),
      0);
  EXPECT_EQ(std::memcmp(Host->tx_context.block_prev_randao.bytes,
                        PrevRandao.bytes, 32),
            0);
  EXPECT_EQ(
      std::memcmp(Host->tx_context.block_base_fee.bytes, BaseFee.bytes, 32), 0);
  EXPECT_EQ(std::memcmp(Host->tx_context.chain_id.bytes, ChainId.bytes, 32), 0);
  EXPECT_EQ(
      std::memcmp(Host->tx_context.blob_base_fee.bytes, BlobBaseFee.bytes, 32),
      0);

  // The account from the state file must still be loaded.
  EXPECT_NE(Host->accounts.find(Sender), Host->accounts.end());

  std::filesystem::remove(FilePath);
}

TEST(EVMStateSaveLoad, PartialTxContextOverlaysOnlyPresentFields) {
  const std::string FilePath = "/tmp/dtvm_test_partial_tx_context_state.json";

  // tx_context object with only tx_origin present; every other field absent.
  const std::string StateJson = R"({
  "accounts": {},
  "tx_context": {
    "tx_origin": "b94f5374fce5edbc8e2a8697c15331677e6ebf0b"
  }
})";
  {
    std::ofstream OutFile(FilePath);
    OutFile << StateJson;
  }

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  const evmc::address CallerOrigin = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::address StateOrigin = evmc::literals::operator""_address(
      "b94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::uint256be GasPrice = zen::utils::parseUint256(
      "0000000000000000000000000000000000000000000000000000000000000010");

  Host->tx_context.tx_origin = CallerOrigin;
  Host->tx_context.tx_gas_price = GasPrice;
  Host->tx_context.block_number = 42;
  Host->tx_context.block_timestamp = 1700000000;
  Host->tx_context.block_gas_limit = 30000000;

  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));

  // tx_origin is present in the state file and must override the caller.
  EXPECT_EQ(
      std::memcmp(Host->tx_context.tx_origin.bytes, StateOrigin.bytes, 20), 0)
      << "tx_origin present in the state file must override the caller value";
  // Every absent field keeps the caller's value.
  EXPECT_EQ(
      std::memcmp(Host->tx_context.tx_gas_price.bytes, GasPrice.bytes, 32), 0)
      << "Absent tx_context fields must keep the caller's value";
  EXPECT_EQ(Host->tx_context.block_number, 42);
  EXPECT_EQ(Host->tx_context.block_timestamp, 1700000000);
  EXPECT_EQ(Host->tx_context.block_gas_limit, 30000000);

  std::filesystem::remove(FilePath);
}

// ---------------------------------------------------------------------------
// Regression test: loadState() must pre-warm the EIP-2930 access list on the
// caller's host.  Account warmth lives in the host access journal (not in the
// accounts map), so any prewarming done on a temporary parsing host would be
// discarded together with it and execution would charge cold-access gas for
// access-list entries.
// ---------------------------------------------------------------------------
TEST(EVMStateSaveLoad, AccessListPrewarmSurvivesLoadState) {
  const std::string FilePath = "/tmp/dtvm_test_access_list_state.json";

  // One entry with storage keys, one plain address entry, and one malformed
  // entry that must be skipped without failing the load.
  const std::string StateJson = R"({
  "accounts": {},
  "access_list": [
    {
      "address": "00000000000000000000000000000000000000aa",
      "storage_keys": [
        "0000000000000000000000000000000000000000000000000000000000000001",
        "not-hex"
      ]
    },
    {"address": "00000000000000000000000000000000000000bb"},
    {"address": "zzz"}
  ]
})";
  {
    std::ofstream OutFile(FilePath);
    OutFile << StateJson;
  }

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  const evmc::address WarmAddr = evmc::literals::operator""_address(
      "00000000000000000000000000000000000000aa");
  const evmc::address WarmAddrNoKeys = evmc::literals::operator""_address(
      "00000000000000000000000000000000000000bb");
  const evmc::bytes32 WarmKey = zen::utils::parseBytes32(
      "0000000000000000000000000000000000000000000000000000000000000001");

  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));

  // Account warmth must survive loadState (EIP-2930).
  EXPECT_EQ(Host->access_account(WarmAddr), EVMC_ACCESS_WARM)
      << "Access-list account must be warm after loadState";
  EXPECT_EQ(Host->access_account(WarmAddrNoKeys), EVMC_ACCESS_WARM)
      << "Access-list entry without storage keys must also be warm";

  // Storage key warmth must survive loadState.
  EXPECT_EQ(Host->access_storage(WarmAddr, WarmKey), EVMC_ACCESS_WARM)
      << "Access-list storage key must be warm after loadState";

  // The access-list-only account must stay materialized so keys of accounts
  // created later in the transaction remain observable.
  EXPECT_NE(Host->accounts.find(WarmAddr), Host->accounts.end());

  std::filesystem::remove(FilePath);
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/589.
// A storage value loaded from prestate is both current and original for the
// new transaction. If original remains zero, a non-zero prestate slot written
// to zero is misclassified as a dirty clear instead of a reset.
TEST(EVMStateSaveLoad, LoadedStorageInitializesOriginalForNewTransaction) {
  const std::string StateFilePath = "/tmp/dtvm_issue589_state.json";
  const std::string ContractAddr = "00000000000000000000000000000000000000f1";
  const std::string SenderAddr = "a94f5374fce5edbc8e2a8697c15331677e6ebf0b";
  const std::vector<uint8_t> Bytecode = {0x60, 0x00, 0x60, 0x00, 0x55, 0x00};

  {
    std::ofstream StateFile(StateFilePath);
    ASSERT_TRUE(StateFile) << "Failed to create issue #589 state file";
    StateFile << R"({
      "accounts": {
        ")" << ContractAddr
              << R"(": {
          "balance": "0000000000000000000000000000000000000000000000000000000000000000",
          "code": "0x600060005500",
          "nonce": 0,
          "storage": {
            "0000000000000000000000000000000000000000000000000000000000000000": {
              "value": "0000000000000000000000000000000000000000000000000000000000000005"
            }
          }
        },
        ")" << SenderAddr
              << R"(": {
          "balance": "0000000000000000000000000000000000000000000000000de0b6b3a7640000",
          "code": "0x",
          "nonce": 0,
          "storage": {}
        }
      },
      "tx_context": {
        "gas_price": "0000000000000000000000000000000000000000000000000000000000000010",
        "block_number": 1,
        "block_timestamp": 1000,
        "block_coinbase": "b94f5374fce5edbc8e2a8697c15331677e6ebf0b",
        "block_prev_randao": "0000000000000000000000000000000000000000000000000000000000000020",
        "block_gas_limit": 10000000,
        "block_base_fee": "0000000000000000000000000000000000000000000000000000000000000010",
        "tx_origin": ")"
              << SenderAddr << R"("
      }
    })";
  }

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*HostPtr, StateFilePath));

  const evmc::address ContractAddress = zen::utils::parseAddress(ContractAddr);
  const evmc::address SenderAddress = zen::utils::parseAddress(SenderAddr);
  const evmc::bytes32 StorageKey{};
  const auto &Slot = HostPtr->accounts[ContractAddress].storage.at(StorageKey);
  const auto PrestateValue = zen::utils::parseBytes32(
      "0000000000000000000000000000000000000000000000000000000000000005");
  EXPECT_EQ(std::memcmp(Slot.current.bytes, PrestateValue.bytes, 32), 0)
      << "Prestate current storage value was not loaded";
  EXPECT_EQ(std::memcmp(Slot.original.bytes, PrestateValue.bytes, 32), 0)
      << "Prestate current value must initialize original for a new tx";

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;
  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  ASSERT_TRUE(RT);
  HostPtr->setRuntime(RT.get());

  zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
  ExecConfig.ModuleName = "issue589";
  ExecConfig.Bytecode = Bytecode.data();
  ExecConfig.BytecodeSize = Bytecode.size();
  ExecConfig.Revision = EVMC_CANCUN;
  ExecConfig.GasLimit = 100000;
  ExecConfig.IntrinsicGas = 21000;
  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = 100000;
  Msg.sender = SenderAddress;
  Msg.recipient = ContractAddress;
  Msg.code_address = ContractAddress;
  ExecConfig.Message = Msg;

  auto Result = HostPtr->executeTransaction(ExecConfig);
  ASSERT_TRUE(Result.Success) << Result.ErrorMessage;
  EXPECT_EQ(Result.Status, EVMC_SUCCESS);
  // Execution gas: 2 PUSH1 + cold SLOAD + reset SSTORE = 5006.
  // Intrinsic gas: 21000. Raw refund: 4800; cap floor(26006 / 5) = 5201.
  EXPECT_EQ(Result.GasUsed, 26006u)
      << "SSTORE(5 -> 0) must charge cold access + reset gas";
  EXPECT_EQ(Result.GasRefund, 4800u)
      << "Clearing a non-zero prestate slot refunds R_clear";
  EXPECT_EQ(Result.GasCharged, 21206u)
      << "Host-path net SSTORE gas is reset cost after refund";

  std::filesystem::remove(StateFilePath);
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/590.
// Warm/cold access is transaction execution state, not world state. A legacy
// prestate's access_status must not cold-skip the next transaction's first
// access, and freshly saved states must not carry the field.
TEST(EVMStateSaveLoad, AccessStatusIsNotPrestateState) {
  const std::string StateFilePath = "/tmp/dtvm_issue590_state.json";
  const std::string SavedStateFilePath = "/tmp/dtvm_issue590_saved_state.json";
  const std::string ContractAddr = "00000000000000000000000000000000000000f1";
  const std::string SenderAddr = "a94f5374fce5edbc8e2a8697c15331677e6ebf0b";
  const std::vector<uint8_t> Bytecode = {0x60, 0x03, 0x60, 0x00, 0x55, 0x00};

  {
    std::ofstream StateFile(StateFilePath);
    ASSERT_TRUE(StateFile) << "Failed to create issue #590 state file";
    StateFile << R"({
      "accounts": {
        ")" << ContractAddr
              << R"(": {
          "balance": "0000000000000000000000000000000000000000000000000000000000000000",
          "code": "0x600360005500",
          "nonce": 0,
          "storage": {
            "0000000000000000000000000000000000000000000000000000000000000000": {
              "value": "0000000000000000000000000000000000000000000000000000000000000000",
              "access_status": 1
            }
          }
        },
        ")" << SenderAddr
              << R"(": {
          "balance": "0000000000000000000000000000000000000000000000000de0b6b3a7640000",
          "code": "0x",
          "nonce": 0,
          "storage": {}
        }
      },
      "tx_context": {
        "gas_price": "0000000000000000000000000000000000000000000000000000000000000010",
        "block_number": 1,
        "block_timestamp": 1000,
        "block_coinbase": "b94f5374fce5edbc8e2a8697c15331677e6ebf0b",
        "block_prev_randao": "0000000000000000000000000000000000000000000000000000000000000020",
        "block_gas_limit": 10000000,
        "block_base_fee": "0000000000000000000000000000000000000000000000000000000000000010",
        "tx_origin": ")"
              << SenderAddr << R"("
      }
    })";
  }

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*HostPtr, StateFilePath));

  const evmc::address ContractAddress = zen::utils::parseAddress(ContractAddr);
  const evmc::address SenderAddress = zen::utils::parseAddress(SenderAddr);
  const evmc::bytes32 StorageKey{};
  const auto &Slot = HostPtr->accounts[ContractAddress].storage.at(StorageKey);
  EXPECT_EQ(Slot.access_status, EVMC_ACCESS_COLD)
      << "Prestate access_status must not warm storage for a new transaction";

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;
  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  ASSERT_TRUE(RT);
  HostPtr->setRuntime(RT.get());

  zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
  ExecConfig.ModuleName = "issue590";
  ExecConfig.Bytecode = Bytecode.data();
  ExecConfig.BytecodeSize = Bytecode.size();
  ExecConfig.Revision = EVMC_CANCUN;
  ExecConfig.GasLimit = 100000;
  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = 100000;
  Msg.sender = SenderAddress;
  Msg.recipient = ContractAddress;
  Msg.code_address = ContractAddress;
  ExecConfig.Message = Msg;

  auto Result = HostPtr->executeTransaction(ExecConfig);
  ASSERT_TRUE(Result.Success) << Result.ErrorMessage;
  EXPECT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.GasUsed, 22106u)
      << "The first SSTORE must pay cold access plus set cost";

  ASSERT_TRUE(zen::utils::saveState(*HostPtr, SavedStateFilePath));
  std::ifstream SavedStateFile(SavedStateFilePath);
  std::string SavedState((std::istreambuf_iterator<char>(SavedStateFile)),
                         std::istreambuf_iterator<char>());
  ASSERT_TRUE(SavedStateFile) << "Failed to read saved state";
  EXPECT_EQ(SavedState.find("access_status"), std::string::npos)
      << "Saved state must not contain transaction access status";
  auto SavedHost = std::make_unique<zen::evm::ZenMockedEVMHost>();
  EXPECT_TRUE(zen::utils::loadState(*SavedHost, SavedStateFilePath))
      << "Saved state must remain valid JSON";

  std::filesystem::remove(StateFilePath);
  std::filesystem::remove(SavedStateFilePath);
}

namespace {

// Helper that builds and returns the final sender balance for a transaction
// against a contract with the given Bytecode. The caller is expected to set
// initial balances, storage, and tx_context before calling this.
struct SettlementResult {
  intx::uint256 InitialSenderBalance;
  intx::uint256 FinalSenderBalance;
  uint64_t GasUsed = 0;
  uint64_t GasRefund = 0;
  uint64_t GasCharged = 0;
  bool Success = false;
};

const evmc::address SettlementContractAddr = evmc::literals::operator""_address(
    "00000000000000000000000000000000000000f1");
const evmc::address SettlementSenderAddr = evmc::literals::operator""_address(
    "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
constexpr uint64_t SettlementGasLimit = 8000000;
constexpr uint64_t SettlementSenderBalance = 0xffffffffffff;
constexpr intx::uint256 SettlementGasPrice = intx::uint256(16);

// Creates the common account/tx-context state used by settlement tests.
// SeedStorage is used by the REVERT test to ensure the first SSTORE is a
// paid reset rather than a free set.
void prepareSettlementHost(zen::evm::ZenMockedEVMHost &Host,
                           const std::vector<uint8_t> &Bytecode,
                           const evmc::address &SenderAddr,
                           const evmc::address &ContractAddr,
                           bool SeedStorage = false) {
  evmc::MockedAccount ContractAccount;
  ContractAccount.code = evmc::bytes(Bytecode.data(), Bytecode.size());
  if (SeedStorage) {
    evmc::bytes32 Key{};
    evmc::bytes32 Val{};
    Val.bytes[31] = 1;
    ContractAccount.storage[Key].current = Val;
    ContractAccount.storage[Key].original = Val;
  }
  Host.accounts[ContractAddr] = ContractAccount;

  evmc::MockedAccount SenderAccount;
  SenderAccount.set_balance(SettlementSenderBalance);
  Host.accounts[SenderAddr] = SenderAccount;

  evmc_tx_context TxCtx{};
  TxCtx.tx_origin = SenderAddr;
  TxCtx.tx_gas_price = intx::be::store<evmc::uint256be>(SettlementGasPrice);
  TxCtx.block_base_fee = intx::be::store<evmc::uint256be>(SettlementGasPrice);
  Host.tx_context = TxCtx;
}

template <typename PrepareHostT>
SettlementResult runSettlementTransaction(const evmc::address &ContractAddr,
                                          const evmc::address &SenderAddr,
                                          const std::vector<uint8_t> &Bytecode,
                                          evmc_revision Revision,
                                          uint64_t GasLimit,
                                          PrepareHostT &&PrepareHost) {
  SettlementResult Result;

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();
  std::forward<PrepareHostT>(PrepareHost)(*HostPtr);

  auto &SenderAcc = HostPtr->accounts[SenderAddr];
  Result.InitialSenderBalance =
      intx::be::load<intx::uint256>(SenderAcc.balance);

  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  if (!RT) {
    return Result;
  }
  HostPtr->setRuntime(RT.get());

  zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
  ExecConfig.ModuleName = "settlement";
  ExecConfig.Bytecode = Bytecode.data();
  ExecConfig.BytecodeSize = Bytecode.size();
  ExecConfig.Revision = Revision;
  ExecConfig.GasLimit = GasLimit;

  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = static_cast<int64_t>(GasLimit);
  Msg.sender = SenderAddr;
  Msg.recipient = ContractAddr;
  Msg.code_address = ContractAddr;
  ExecConfig.Message = Msg;

  auto ExecResult = HostPtr->executeTransaction(ExecConfig);
  Result.Success = ExecResult.Success;
  Result.GasUsed = ExecResult.GasUsed;
  Result.GasRefund = ExecResult.GasRefund;
  Result.GasCharged = ExecResult.GasCharged;
  Result.FinalSenderBalance =
      intx::be::load<intx::uint256>(HostPtr->accounts[SenderAddr].balance);
  return Result;
}

// Helper that exercises the same settlement code used by the dtvm CLI:
// applyEvmUpfrontGas, callEVMMain, applyEvmPostExecutionSettlement.
template <typename PrepareHostT>
SettlementResult runDtvmCliSettlementTransaction(
    const evmc::address &ContractAddr, const evmc::address &SenderAddr,
    const std::vector<uint8_t> &Bytecode, evmc_revision Revision,
    uint64_t GasLimit, PrepareHostT &&PrepareHost) {
  SettlementResult Result;

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();
  // The CLI selects the Host revision before loading state and pre-warming.
  // Keep this helper aligned with src/cli/dtvm.cpp.
  HostPtr->setRevision(Revision);
  std::forward<PrepareHostT>(PrepareHost)(*HostPtr);

  auto &SenderAcc = HostPtr->accounts[SenderAddr];
  Result.InitialSenderBalance =
      intx::be::load<intx::uint256>(SenderAcc.balance);

  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  if (!RT) {
    return Result;
  }
  HostPtr->setRuntime(RT.get());

  auto ModRet =
      RT->loadEVMModule("cli_settlement", Bytecode.data(), Bytecode.size());
  if (!ModRet) {
    return Result;
  }
  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  if (!Iso) {
    return Result;
  }

  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = static_cast<int64_t>(GasLimit);
  Msg.sender = SenderAddr;
  Msg.recipient = ContractAddr;
  Msg.code_address = ContractAddr;

  const auto UpfrontResult =
      zen::utils::applyEvmUpfrontGas(*HostPtr, Msg, GasLimit, Revision);
  if (UpfrontResult != zen::utils::EvmUpfrontGasResult::Success) {
    return Result;
  }

  auto InstRet = Iso->createEVMInstance(*Mod, static_cast<uint64_t>(Msg.gas));
  if (!InstRet) {
    return Result;
  }
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(Revision);

  evmc::Result ExecResult{};
  RT->callEVMMain(*Inst, Msg, ExecResult);

  zen::utils::applyEvmPostExecutionSettlement(*HostPtr, Msg, GasLimit,
                                              ExecResult, Revision);

  Result.Success = ExecResult.status_code == EVMC_SUCCESS;
  Result.GasUsed = static_cast<uint64_t>(
      GasLimit - std::max<int64_t>(0, ExecResult.gas_left));
  const uint64_t RefundLimit =
      zen::utils::computeRefundCap(Revision, Result.GasUsed);
  Result.GasRefund = std::min(
      static_cast<uint64_t>(std::max<int64_t>(0, ExecResult.gas_refund)),
      RefundLimit);
  Result.GasCharged =
      Result.GasUsed > Result.GasRefund ? Result.GasUsed - Result.GasRefund : 0;
  Result.FinalSenderBalance =
      intx::be::load<intx::uint256>(HostPtr->accounts[SenderAddr].balance);
  return Result;
}

} // namespace

// Regression tests for https://github.com/DTVMStack/DTVM/issues/588.
// Unlike the CLI helper below, executeTransaction models the state-test path
// where intrinsic gas has already been consumed before this host method.
// These tests therefore validate the host's refund-cap behavior on execution
// gas only; the CLI-specific tests cover intrinsic gas and total settlement.
TEST(EVMRegressionTest, Issue588_CappedRefundDoesNotReduceChargesOnCancun) {
  // SSTORE set slot 0 to 5, then clear it back to 0. Cancun refunds are capped
  // at 1/5 of GasUsed.
  const std::string BytecodeHex = "6005600055600060005500";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse issue #588 bytecode";

  auto Result = runSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, *BytecodeBuf, EVMC_CANCUN,
      SettlementGasLimit, [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, *BytecodeBuf, SettlementSenderAddr,
                              SettlementContractAddr);
      });

  ASSERT_TRUE(Result.Success) << "Issue #588 transaction should succeed";

  // Expected host-path gas accounting:
  //   ExecutionGasUsed = 22212; intrinsic is consumed before executeTransaction
  //   refund counter = 19900, capped to 22212/5 = 4442
  //   GasCharged = 22212 - 4442 = 17770
  // Net sender cost = GasCharged * gas_price.
  EXPECT_EQ(Result.GasUsed, 22212u)
      << "ExecutionGasUsed should exclude intrinsic gas on the host path";
  EXPECT_EQ(Result.GasCharged, 17770u)
      << "Refund cap should produce the expected Cancun gas charge";

  intx::uint256 ExpectedCost = SettlementGasPrice * Result.GasCharged;
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            ExpectedCost)
      << "Sender should be charged GasCharged * gas_price on issue #588";
}

// On the host path, ensure that a REVERT discards the refund accumulator so
// it does not reduce the sender's net gas charge.
TEST(EVMRegressionTest, Issue588_RevertResetsRefundAccumulator) {
  // SSTORE slot 0 to 5 (which would create a refund if the slot is later
  // cleared), then REVERT. The refund counter is reset on revert, so the gas
  // refunded to the sender must be (GasLimit - GasUsed) only, not more.
  const std::string BytecodeHex = "600560005560006000fd";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse revert bytecode";

  auto Result = runSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, *BytecodeBuf, EVMC_CANCUN,
      SettlementGasLimit, [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, *BytecodeBuf, SettlementSenderAddr,
                              SettlementContractAddr, /*SeedStorage=*/true);
      });

  ASSERT_TRUE(Result.Success) << "Revert transaction should succeed";
  EXPECT_EQ(Result.GasCharged, Result.GasUsed)
      << "REVERT must reset refund counter so no refund is applied";

  intx::uint256 ExpectedCost = SettlementGasPrice * Result.GasCharged;
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            ExpectedCost)
      << "Sender cost must equal GasUsed * gas_price after revert";
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/588.
// On the host path, pre-London forks (Byzantium) cap refunds at
// ExecutionGasUsed / 2 instead of ExecutionGasUsed / 5 (EIP-3529). The same
// SSTORE bytecode that yields a large raw refund produces a different cap and
// therefore different GasCharged.
TEST(EVMRegressionTest, Issue588_HalfRefundCapOnByzantium) {
  // Byzantium is pre-London, pre-Berlin. Storage costs use the legacy
  // schedule: Clear refund = 15000. No cold access surcharge.
  // SSTORE set slot 0 → 5 (ADDED), then clear slot 0 → 0 (ADDED_DELETED,
  // which maps to DELETED in the legacy table → ReSet=5000, Clear=15000).
  const std::string BytecodeHex = "6005600055600060005500";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse issue #588 pre-London bytecode";

  auto Result = runSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, *BytecodeBuf,
      EVMC_BYZANTIUM, SettlementGasLimit,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, *BytecodeBuf, SettlementSenderAddr,
                              SettlementContractAddr);
      });

  ASSERT_TRUE(Result.Success)
      << "Issue #588 pre-London transaction should succeed";

  // On Byzantium:
  //   4×PUSH1(base=3) = 12
  //   SSTORE#1 (cold skip, ADDED): cold=0, warm=20000 (Set), refund=0 → 20000
  //   SSTORE#2 (warm, ADDED_DELETED→DELETED): cold=0, warm=5000 (ReSet),
  //     refund=15000 (Clear) → 5000
  //   GasUsed = 12 + 20000 + 5000 = 25012
  //   Raw refund = 15000
  //   Refund cap (pre-London) = GasUsed / 2 = 12506
  //   Applied refund = min(15000, 12506) = 12506
  //   GasCharged = 25012 - 12506 = 12996
  EXPECT_EQ(Result.GasUsed, 25012u)
      << "GasUsed should equal PUSH base + SSTORE costs on Byzantium";

  uint64_t HalfCap = Result.GasUsed / 2; // = 12506
  EXPECT_EQ(Result.GasRefund, HalfCap)
      << "Pre-London refund must be capped at GasUsed/2, not GasUsed/5";

  // Raw refund (15000) must exceed the half cap, confirming the cap is active.
  EXPECT_LT(Result.GasRefund, 15000u)
      << "Raw EIP-2200 Clear refund (15000) should be capped by /2 rule";

  EXPECT_EQ(Result.GasCharged, Result.GasUsed - HalfCap)
      << "GasCharged = GasUsed - GasUsed/2 after refund cap";

  intx::uint256 ExpectedCost = SettlementGasPrice * Result.GasCharged;
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            ExpectedCost)
      << "Sender should be charged GasCharged * gas_price on issue #588";
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/588
// SELFDESTRUCT on a pre-London revision adds EXTRA_REFUND_BEFORE_LONDON (24000)
// to the refund accumulator. The /2 refund cap must still apply.
TEST(EVMRegressionTest, Issue588_SELFDESTRUCTPreLondonRefundCapped) {
  // Bytecode: PUSH1 0x00 SELFDESTRUCT — destroys contract, transfers balance
  // to address(0). On pre-London this adds 24000 refund, capped at GasUsed/2.
  const std::string BytecodeHex = "6000FF";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse SELFDESTRUCT bytecode hex";

  auto Result = runSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, *BytecodeBuf,
      EVMC_BYZANTIUM, SettlementGasLimit,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, *BytecodeBuf, SettlementSenderAddr,
                              SettlementContractAddr);
      });

  ASSERT_TRUE(Result.Success)
      << "SELFDESTRUCT must succeed on Byzantium without cold-access costs";

  // On Byzantium (pre-Berlin):
  //   PUSH1(base=3) + SELFDESTRUCT(base=5000) = 5003
  //   No cold access surcharge, no account creation cost (recipient balance=0)
  //   EXTRA_REFUND_BEFORE_LONDON = 24000 added to refund counter
  //   Refund cap = GasUsed / 2 = 5003 / 2 = 2501
  //   Applied refund = min(24000, 2501) = 2501
  //   GasCharged = 5003 - 2501 = 2502
  constexpr uint64_t SelfDestructGas = 3 + 5000; // PUSH1 + SELFDESTRUCT
  EXPECT_EQ(Result.GasUsed, SelfDestructGas)
      << "GasUsed must be PUSH1 + SELFDESTRUCT base cost on Byzantium";

  uint64_t HalfCap = Result.GasUsed / 2; // = 2501
  EXPECT_EQ(Result.GasRefund, HalfCap)
      << "Pre-London SELFDESTRUCT refund must be capped at GasUsed/2";
  EXPECT_LT(Result.GasRefund, zen::evm::EXTRA_REFUND_BEFORE_LONDON)
      << "Raw EXTRA_REFUND_BEFORE_LONDON (24000) must be capped";

  EXPECT_EQ(Result.GasCharged, SelfDestructGas - HalfCap)
      << "GasCharged must be GasUsed minus the /2 cap after SELFDESTRUCT";

  intx::uint256 ExpectedCost = SettlementGasPrice * Result.GasCharged;
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            ExpectedCost)
      << "Sender must be charged GasCharged * gas_price after SELFDESTRUCT";
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/588
// Exercises the actual dtvm.cpp CLI settlement path (not the test-host helper)
// to ensure intrinsic gas, refund cap, and balance update stay aligned.
TEST(EVMRegressionTest, Issue588_CliSettlement_CappedRefundOnCancun) {
  const std::string BytecodeHex = "6005600055600060005500";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse issue #588 CLI settlement bytecode";

  auto Result = runDtvmCliSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, *BytecodeBuf, EVMC_CANCUN,
      SettlementGasLimit, [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, *BytecodeBuf, SettlementSenderAddr,
                              SettlementContractAddr);
      });

  ASSERT_TRUE(Result.Success) << "CLI settlement transaction should succeed";

  EXPECT_EQ(Result.GasUsed, 43212u)
      << "CLI path should be the actual SSTORE execution cost";
  EXPECT_EQ(Result.GasCharged, 34570u)
      << "CLI path refund cap should produce the actual Cancun gas charge";

  intx::uint256 ExpectedCost = SettlementGasPrice * Result.GasCharged;
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            ExpectedCost)
      << "CLI path sender should be charged GasCharged * gas_price";
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/588
// Same as the Byzantium SSTORE test above, but driven through the CLI path.
TEST(EVMRegressionTest, Issue588_CliSettlement_HalfRefundCapOnByzantium) {
  const std::string BytecodeHex = "6005600055600060005500";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf)
      << "Failed to parse issue #588 pre-London CLI settlement bytecode";

  auto Result = runDtvmCliSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, *BytecodeBuf,
      EVMC_BYZANTIUM, SettlementGasLimit,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, *BytecodeBuf, SettlementSenderAddr,
                              SettlementContractAddr);
      });

  ASSERT_TRUE(Result.Success)
      << "CLI settlement pre-London transaction should succeed";

  EXPECT_EQ(Result.GasUsed, 46012u)
      << "CLI path GasUsed should equal PUSH base + SSTORE costs on Byzantium";

  EXPECT_EQ(Result.GasRefund, 15000u)
      << "CLI path pre-London refund must reserve the original refund counter";

  EXPECT_EQ(Result.GasCharged, 31012u)
      << "CLI path GasCharged should be GasUsed minus the refund";

  intx::uint256 ExpectedCost = SettlementGasPrice * Result.GasCharged;
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            ExpectedCost)
      << "CLI path sender should be charged GasCharged * gas_price";
}

// Regression test for https://github.com/DTVMStack/DTVM/issues/545
// BALANCE should reflect the upfront gas deduction (gas_price * gas_limit)
// from the sender's balance, per EVM spec (Yellow Paper §6).
TEST(EVMRegressionTest, Issue545_BalanceReflectsUpfrontGasDeduction) {
  // Contract: CALLER BALANCE PUSH1 0x00 MSTORE PUSH1 0x20 PUSH1 0x00 RETURN
  // Returns the caller's balance as 32-byte output.
  const std::string BytecodeHex = "333160005260206000f3";
  auto BytecodeBuf = zen::utils::fromHex(BytecodeHex);
  ASSERT_TRUE(BytecodeBuf) << "Failed to parse bytecode hex";

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;

  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "00000000000000000000000000000000000000f1");
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();

  // Contract account with the CALLER+BALANCE bytecode
  evmc::MockedAccount ContractAccount;
  ContractAccount.code = evmc::bytes(BytecodeBuf->data(), BytecodeBuf->size());

  // Sender with initial balance 0xFFFFFFFFFF (1099511627775)
  evmc::MockedAccount SenderAccount;
  SenderAccount.set_balance(0xFFFFFFFFFF);

  HostPtr->accounts[ContractAddr] = ContractAccount;
  HostPtr->accounts[SenderAddr] = SenderAccount;

  // Set gas_price = 16, base_fee = 16 (matching issue reproduction)
  evmc_tx_context TxCtx{};
  TxCtx.tx_origin = SenderAddr;
  TxCtx.tx_gas_price = intx::be::store<evmc::uint256be>(intx::uint256(16));
  TxCtx.block_base_fee = intx::be::store<evmc::uint256be>(intx::uint256(16));
  HostPtr->tx_context = TxCtx;

  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  ASSERT_TRUE(RT != nullptr) << "Failed to create runtime";
  HostPtr->setRuntime(RT.get());

  auto ModRet = RT->loadEVMModule("issue545_balance_gas", BytecodeBuf->data(),
                                  BytecodeBuf->size());
  ASSERT_TRUE(ModRet) << "Failed to load module";
  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  ASSERT_TRUE(Iso != nullptr) << "Failed to create isolation";

  constexpr uint64_t GasLimit = 1000000;
  const int64_t IntrinsicGas =
      zen::utils::computeIntrinsicGas(EVMC_CANCUN, EVMC_CALL, nullptr, 0);
  ASSERT_GT(GasLimit, static_cast<uint64_t>(IntrinsicGas));
  const uint64_t ExecutionGasLimit = GasLimit - IntrinsicGas;

  auto InstRet = Iso->createEVMInstance(*Mod, ExecutionGasLimit);
  ASSERT_TRUE(InstRet) << "Failed to create instance";
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(EVMC_CANCUN);

  // Deduct upfront gas cost from sender's balance before execution,
  // matching EVM spec and the CLI fix for issue #545.
  intx::uint256 GasPrice =
      intx::be::load<intx::uint256>(HostPtr->tx_context.tx_gas_price);
  intx::uint256 BaseFee =
      intx::be::load<intx::uint256>(HostPtr->tx_context.block_base_fee);
  intx::uint256 EffectiveGasPrice = GasPrice > BaseFee ? GasPrice : BaseFee;
  intx::uint256 UpfrontGasCost = intx::uint256(GasLimit) * EffectiveGasPrice;
  auto &SenderAcc = HostPtr->accounts[SenderAddr];
  intx::uint256 SenderBalance =
      intx::be::load<intx::uint256>(SenderAcc.balance);
  ASSERT_GE(SenderBalance, UpfrontGasCost)
      << "Sender balance insufficient for upfront gas cost";
  SenderBalance -= UpfrontGasCost;
  SenderAcc.balance = intx::be::store<evmc::bytes32>(SenderBalance);

  evmc_message Msg = {
      .kind = EVMC_CALL,
      .flags = 0u,
      .depth = 0,
      .gas = static_cast<int64_t>(ExecutionGasLimit),
      .recipient = ContractAddr,
      .sender = SenderAddr,
      .input_data = nullptr,
      .input_size = 0,
      .value = {},
      .create2_salt = {},
      .code_address = ContractAddr,
      .code = reinterpret_cast<const uint8_t *>(Mod->Code),
      .code_size = Mod->CodeSize,
  };

  evmc::Result RawResult;
  EXPECT_NO_THROW({ RT->callEVMMain(*Inst, Msg, RawResult); });
  ASSERT_EQ(RawResult.status_code, EVMC_SUCCESS)
      << "EVM execution failed with status code "
      << static_cast<int>(RawResult.status_code);

  // Expected: initial balance - gas_price * gas_limit
  // 0xFFFFFFFFFF - 16 * 1000000 = 0xFFFF0BDBFF
  evmc::bytes32 OutputBytes{};
  std::memcpy(OutputBytes.bytes, RawResult.output_data, 32);
  intx::uint256 ReturnedBalance = intx::be::load<intx::uint256>(OutputBytes);

  intx::uint256 ExpectedBalance =
      intx::uint256(0xFFFFFFFFFF) - intx::uint256(16) * intx::uint256(1000000);

  EXPECT_EQ(ReturnedBalance, ExpectedBalance)
      << "BALANCE should return sender balance after upfront gas deduction";
}

// ---------------------------------------------------------------------------
// Regression tests for Issue #563 / #564:
//   loadState() must clamp int64_t fields (block_gas_limit, block_number,
//   block_timestamp) to INT64_MAX when JSON values exceed INT64_MAX, rather
//   than silently wrapping them to negative values.
// ---------------------------------------------------------------------------

// Helper: write a minimal state JSON with a specific tx_context field set to
// the given numeric value. All other tx_context fields use safe defaults.
static void writeInt64OverflowStateJson(const std::string &FilePath,
                                        const std::string &FieldName,
                                        const std::string &NumericValue) {
  // clang-format off
  const std::string DefaultJson = R"({
  "accounts": {
    "a94f5374fce5edbc8e2a8697c15331677e6ebf0b": {
      "balance": "000000000000000000000000000000000000000000000000000000ffffffffff",
      "nonce": 0, "code": "", "codehash": "0000000000000000000000000000000000000000000000000000000000000000", "storage": {}
    }
  },
  "tx_context": {
    "gas_price": "0000000000000000000000000000000000000000000000000000000000000010",
    "block_number": 1,
    "block_timestamp": 1000,
    "block_coinbase": "b94f5374fce5edbc8e2a8697c15331677e6ebf0b",
    "block_prev_randao": "0000000000000000000000000000000000000000000000000000000000200000",
    "block_gas_limit": 30000000,
    "block_base_fee": "0000000000000000000000000000000000000000000000000000000000000010"
  }
})";
  // clang-format on

  // Map each field name to its default value in the template above.
  std::string DefaultKeyValue;
  if (FieldName == "block_gas_limit") {
    DefaultKeyValue = "\"block_gas_limit\": 30000000";
  } else if (FieldName == "block_number") {
    DefaultKeyValue = "\"block_number\": 1";
  } else if (FieldName == "block_timestamp") {
    DefaultKeyValue = "\"block_timestamp\": 1000";
  } else {
    FAIL() << "Unsupported field name: " << FieldName;
  }

  std::string Content = DefaultJson;
  auto Pos = Content.find(DefaultKeyValue);
  ASSERT_NE(Pos, std::string::npos) << "Could not find default value for "
                                    << FieldName << " in template JSON";
  Content.replace(Pos, DefaultKeyValue.size(),
                  "\"" + FieldName + "\": " + NumericValue);

  std::ofstream OutFile(FilePath);
  OutFile << Content;
  OutFile.close();
}

// block_gas_limit = 2^63  →  clamped to INT64_MAX (no negative wrap)
TEST(EVMStateSaveLoad, BlockGasLimitOverflowInt64) {
  const std::string FilePath = "/tmp/dtvm_test_gas_limit_overflow_int64.json";
  writeInt64OverflowStateJson(FilePath, "block_gas_limit",
                              "9223372036854775808"); // 2^63

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_gas_limit,
            std::numeric_limits<int64_t>::max());
  EXPECT_GT(Host->tx_context.block_gas_limit, 0)
      << "block_gas_limit must not be negative after clamp";

  std::filesystem::remove(FilePath);
}

// block_gas_limit = 2^64 - 1  →  clamped to INT64_MAX
TEST(EVMStateSaveLoad, BlockGasLimitOverflowUint64Max) {
  const std::string FilePath =
      "/tmp/dtvm_test_gas_limit_overflow_uint64max.json";
  writeInt64OverflowStateJson(FilePath, "block_gas_limit",
                              "18446744073709551615"); // 2^64 - 1

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_gas_limit,
            std::numeric_limits<int64_t>::max());
  EXPECT_GT(Host->tx_context.block_gas_limit, 0)
      << "block_gas_limit must not be negative after clamp";

  std::filesystem::remove(FilePath);
}

// block_gas_limit = INT64_MAX  →  exact value preserved (boundary)
TEST(EVMStateSaveLoad, BlockGasLimitAtInt64Max) {
  const std::string FilePath = "/tmp/dtvm_test_gas_limit_int64max.json";
  writeInt64OverflowStateJson(FilePath, "block_gas_limit",
                              "9223372036854775807"); // INT64_MAX

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_gas_limit,
            std::numeric_limits<int64_t>::max());

  std::filesystem::remove(FilePath);
}

// block_number = 2^63  →  clamped to INT64_MAX
TEST(EVMStateSaveLoad, BlockNumberOverflowInt64) {
  const std::string FilePath =
      "/tmp/dtvm_test_block_number_overflow_int64.json";
  writeInt64OverflowStateJson(FilePath, "block_number",
                              "9223372036854775808"); // 2^63

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_number, std::numeric_limits<int64_t>::max());
  EXPECT_GT(Host->tx_context.block_number, 0)
      << "block_number must not be negative after clamp";

  std::filesystem::remove(FilePath);
}

// block_number = INT64_MAX  →  exact value preserved
TEST(EVMStateSaveLoad, BlockNumberAtInt64Max) {
  const std::string FilePath = "/tmp/dtvm_test_block_number_int64max.json";
  writeInt64OverflowStateJson(FilePath, "block_number",
                              "9223372036854775807"); // INT64_MAX

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_number, std::numeric_limits<int64_t>::max());

  std::filesystem::remove(FilePath);
}

// block_timestamp = 2^63  →  clamped to INT64_MAX
TEST(EVMStateSaveLoad, BlockTimestampOverflowInt64) {
  const std::string FilePath =
      "/tmp/dtvm_test_block_timestamp_overflow_int64.json";
  writeInt64OverflowStateJson(FilePath, "block_timestamp",
                              "9223372036854775808"); // 2^63

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_timestamp,
            std::numeric_limits<int64_t>::max());
  EXPECT_GT(Host->tx_context.block_timestamp, 0)
      << "block_timestamp must not be negative after clamp";

  std::filesystem::remove(FilePath);
}

// block_timestamp = INT64_MAX  →  exact value preserved
TEST(EVMStateSaveLoad, BlockTimestampAtInt64Max) {
  const std::string FilePath = "/tmp/dtvm_test_block_timestamp_int64max.json";
  writeInt64OverflowStateJson(FilePath, "block_timestamp",
                              "9223372036854775807"); // INT64_MAX

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, FilePath));
  EXPECT_EQ(Host->tx_context.block_timestamp,
            std::numeric_limits<int64_t>::max());

  std::filesystem::remove(FilePath);
}
// A malformed account entry must fail the whole state load without invoking a
// RapidJSON member lookup.  RapidJSON asserts on HasMember when the value is
// not an object, so wrapping parser calls in try/catch is insufficient.
TEST(EVMStateSaveLoad, NullAccountObjectFailsCleanly) {
  const std::string StateFilePath = "/tmp/dtvm_null_account_state.json";
  {
    std::ofstream StateFile(StateFilePath);
    ASSERT_TRUE(StateFile) << "Failed to create malformed state file";
    StateFile << R"({
      "accounts": {
        "00000000000000000000000000000000000000f1": null
      }
    })";
  }

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  EXPECT_FALSE(zen::utils::loadState(*Host, StateFilePath))
      << "A null account value must fail state loading";
  EXPECT_TRUE(Host->accounts.empty())
      << "A malformed state file must not partially mutate the host";
  std::filesystem::remove(StateFilePath);
}

// MockedHost caps its debugging access journal at 200 entries.  A valid access
// list can fill that journal, after which generic MockedHost::access_account
// would report later prewarmed addresses as cold.  This Host's unbounded set
// must remain the warmth source of truth.
TEST(EVMStateSaveLoad, LargeAccessListDoesNotEvictPrewarm) {
  const std::string StateFilePath = "/tmp/dtvm_large_access_list.json";
  std::string StateJson = R"({
  "accounts": {},
  "access_list": [
)";
  constexpr int EntryCount =
      static_cast<int>(evmc::MockedHost::max_recorded_account_accesses);
  for (int I = 0; I < EntryCount; ++I) {
    char Address[48];
    std::snprintf(Address, sizeof(Address),
                  "    \"000000000000000000000000000000000000%04x\"", I);
    StateJson += Address;
    StateJson += (I + 1 == EntryCount) ? "\n" : ",\n";
  }
  StateJson += "  ]\n}";
  {
    std::ofstream StateFile(StateFilePath);
    ASSERT_TRUE(StateFile) << "Failed to create large access-list state";
    StateFile << StateJson;
  }

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  ASSERT_TRUE(zen::utils::loadState(*Host, StateFilePath));
  evmc::address Sender{};
  evmc::address Recipient{};
  evmc::address Coinbase{};
  zen::utils::prewarmTransactionAccounts(*Host, EVMC_CANCUN, Sender, Recipient,
                                         Coinbase);
  evmc::address KZGPrecompile{};
  KZGPrecompile.bytes[19] = 0x0a;
  EXPECT_EQ(Host->access_account(KZGPrecompile), EVMC_ACCESS_WARM)
      << "KZG prewarm must survive a 200-entry MockedHost access journal";
  std::filesystem::remove(StateFilePath);
}

// ---------------------------------------------------------------------------
// Regression tests for upstream issues that were fixed by PR #601 - #610.
// ---------------------------------------------------------------------------
TEST(EVMStateSaveLoad, Issue601_OverlongUint256FailsWithoutAbort) {
  const std::string StateFilePath = "/tmp/dtvm_issue601_state.json";
  const std::string OverlongHex(66, 'a');
  const std::string StateJson = R"({
    "accounts": {
      "00000000000000000000000000000000000000f1": {
        "balance": ")" + OverlongHex +
                                R"(",
        "code": "0x5f00",
        "nonce": 0,
        "storage": {}
      }
    }
  })";

  {
    std::ofstream StateFile(StateFilePath);
    ASSERT_TRUE(StateFile) << "Failed to create issue #601 state file";
    StateFile << StateJson;
  }

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  EXPECT_FALSE(zen::utils::loadState(*Host, StateFilePath))
      << "An overlong uint256 must be rejected, not crash the process";
  std::filesystem::remove(StateFilePath);
}

TEST(EVMRegressionTest, Issue602_KZGPrecompileIsWarmOnCancun) {
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  evmc::address Sender{};
  evmc::address Recipient{};
  evmc::address Coinbase{};
  zen::utils::prewarmTransactionAccounts(*Host, EVMC_CANCUN, Sender, Recipient,
                                         Coinbase);
  evmc::address KZGPrecompile{};
  KZGPrecompile.bytes[19] = 0x0a;
  EXPECT_EQ(Host->access_account(KZGPrecompile), EVMC_ACCESS_WARM)
      << "EIP-4844 KZG precompile must be warm at transaction start";
}

TEST(EVMRegressionTest, Issue602_KZGPrecompileStaysColdBeforeCancun) {
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  evmc::address Sender{};
  evmc::address Recipient{};
  evmc::address Coinbase{};
  zen::utils::prewarmTransactionAccounts(*Host, EVMC_BERLIN, Sender, Recipient,
                                         Coinbase);
  evmc::address KZGPrecompile{};
  KZGPrecompile.bytes[19] = 0x0a;
  EXPECT_EQ(Host->access_account(KZGPrecompile), EVMC_ACCESS_COLD)
      << "KZG precompile must not be pre-warmed before Cancun";
}

// EIP-2537 (Prague) activates the BLS12-381 precompiles at 0x0b-0x11, so the
// revision-aware prewarm range must extend to 0x11; 0x12 and above stay cold.
TEST(EVMRegressionTest, BLSPrecompilesAreWarmOnPrague) {
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  evmc::address Sender{};
  evmc::address Recipient{};
  evmc::address Coinbase{};
  zen::utils::prewarmTransactionAccounts(*Host, EVMC_PRAGUE, Sender, Recipient,
                                         Coinbase);
  for (int PrecompileIdx = 0x0b; PrecompileIdx <= 0x11; ++PrecompileIdx) {
    evmc::address BLSPrecompile{};
    BLSPrecompile.bytes[19] = static_cast<uint8_t>(PrecompileIdx);
    EXPECT_EQ(Host->access_account(BLSPrecompile), EVMC_ACCESS_WARM)
        << "BLS precompile at index " << PrecompileIdx
        << " must be warm at transaction start on Prague";
  }
  // Nothing above 0x11 is a known precompile, so it must not be pre-warmed.
  evmc::address FirstUnassigned{};
  FirstUnassigned.bytes[19] = 0x12;
  EXPECT_EQ(Host->access_account(FirstUnassigned), EVMC_ACCESS_COLD)
      << "Addresses above the BLS range must not be pre-warmed";
}

TEST(EVMRegressionTest, BLSPrecompilesStayColdBeforePrague) {
  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  evmc::address Sender{};
  evmc::address Recipient{};
  evmc::address Coinbase{};
  zen::utils::prewarmTransactionAccounts(*Host, EVMC_CANCUN, Sender, Recipient,
                                         Coinbase);
  evmc::address BLSPrecompile{};
  BLSPrecompile.bytes[19] = 0x0b;
  EXPECT_EQ(Host->access_account(BLSPrecompile), EVMC_ACCESS_COLD)
      << "BLS precompiles must not be pre-warmed before Prague";
}

TEST(EVMRegressionTest, Issue606_EmptyPrestateAccountChargesNewAccountGas) {
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "1111111111111111111111111111111111111111");
  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
  const evmc::address CalleeAddr = evmc::literals::operator""_address(
      "a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7");
  std::string BytecodeHex = "5f5f5f5f600173";
  for (int I = 0; I < 20; ++I) {
    BytecodeHex += "a7";
  }
  BytecodeHex += "600ff15a6280010555";
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex(BytecodeHex).value();

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  Host->accounts[ContractAddr].code =
      evmc::bytes(Bytecode.data(), Bytecode.size());
  Host->accounts[SenderAddr].set_balance(0x1bc16d674ec80000ULL);
  Host->accounts[CalleeAddr] = evmc::MockedAccount{};
  Host->tx_context.block_coinbase = evmc::address{};
  Host->tx_context.tx_origin = SenderAddr;
  Host->tx_context.tx_gas_price =
      intx::be::store<evmc::uint256be>(intx::uint256(1));
  Host->tx_context.block_base_fee =
      intx::be::store<evmc::uint256be>(intx::uint256(1));

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;
  auto RT = Runtime::newEVMRuntime(Config, Host.get());
  ASSERT_TRUE(RT);
  Host->setRuntime(RT.get());

  zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
  ExecConfig.ModuleName = "issue606";
  ExecConfig.Bytecode = Bytecode.data();
  ExecConfig.BytecodeSize = Bytecode.size();
  ExecConfig.Revision = EVMC_CANCUN;
  // The issue's CLI command starts the contract with gas-limit 1000000 minus
  // the basic intrinsic gas.  This keeps the verdict independent of the
  // CLI's different fee paths.
  ExecConfig.GasLimit = 1000000 - 21000;
  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = 1000000 - 21000;
  Msg.sender = SenderAddr;
  Msg.recipient = ContractAddr;
  Msg.code_address = ContractAddr;
  ExecConfig.Message = Msg;

  auto Result = Host->executeTransaction(ExecConfig);
  ASSERT_TRUE(Result.Success) << Result.ErrorMessage;
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);

  const evmc::bytes32 StorageKey = zen::utils::parseBytes32(
      "0000000000000000000000000000000000000000000000000000000000800105");
  const auto &StorageValue =
      Host->accounts[ContractAddr].storage.at(StorageKey).current;
  const evmc::bytes32 ExpectedRemainingGas = zen::utils::parseBytes32(
      "00000000000000000000000000000000000000000000000000000000000e6a29");
  EXPECT_EQ(std::memcmp(StorageValue.bytes, ExpectedRemainingGas.bytes, 32), 0)
      << "CALL with value to an empty prestate account must charge 25000 gas";
}

// Before Spurious Dragon (EIP-161), an account loaded in prestate exists even
// if it is empty.  In particular, CALL to such an account must not charge the
// 25000-gas new-account cost that the absence-based account check triggers.
TEST(EVMRegressionTest,
     PreSpuriousDragonEmptyAccountDoesNotChargeNewAccountGas) {
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "1111111111111111111111111111111111111111");
  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
  const evmc::address CalleeAddr = evmc::literals::operator""_address(
      "a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7");
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex(
          "6000600060006000600073a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7"
          "600ff15a6280010555")
          .value();
  const evmc::bytes32 StorageKey = zen::utils::parseBytes32(
      "0000000000000000000000000000000000000000000000000000000000800105");

  auto RunWithOptionallyMaterializedCallee =
      [&](bool MaterializeCallee) -> uint64_t {
    auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
    Host->accounts[ContractAddr].code =
        evmc::bytes(Bytecode.data(), Bytecode.size());
    Host->accounts[SenderAddr].set_balance(0x1bc16d674ec80000ULL);
    if (MaterializeCallee) {
      Host->accounts[CalleeAddr] = evmc::MockedAccount{};
    }

    RuntimeConfig Config;
    Config.Mode = common::RunMode::InterpMode;
    auto RT = Runtime::newEVMRuntime(Config, Host.get());
    EXPECT_TRUE(RT);
    Host->setRuntime(RT.get());

    zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
    ExecConfig.ModuleName = "pre-spurious-dragon-empty-account";
    ExecConfig.Bytecode = Bytecode.data();
    ExecConfig.BytecodeSize = Bytecode.size();
    ExecConfig.Revision = EVMC_TANGERINE_WHISTLE;
    ExecConfig.GasLimit = 1000000;
    evmc_message Msg{};
    Msg.kind = EVMC_CALL;
    Msg.gas = 1000000;
    Msg.sender = SenderAddr;
    Msg.recipient = ContractAddr;
    Msg.code_address = ContractAddr;
    ExecConfig.Message = Msg;

    auto Result = Host->executeTransaction(ExecConfig);
    EXPECT_TRUE(Result.Success) << Result.ErrorMessage;
    EXPECT_EQ(Result.Status, EVMC_SUCCESS);
    const auto &Storage = Host->accounts[ContractAddr].storage;
    EXPECT_NE(Storage.find(StorageKey), Storage.end());
    return static_cast<uint64_t>(intx::be::load<intx::uint256>(
        Host->accounts[ContractAddr].storage.at(StorageKey).current));
  };

  const uint64_t ExistingEmptyAccountGas =
      RunWithOptionallyMaterializedCallee(true);
  const uint64_t AbsentAccountGas = RunWithOptionallyMaterializedCallee(false);
  EXPECT_EQ(ExistingEmptyAccountGas, AbsentAccountGas + 25000);
}

// This test uses the same Host->upfront->call path as dtvm.cpp.  It also
// demonstrates why the Host revision must be selected before that path: an
// instance set to Tangerine Whistle still consults a Host that defaults to the
// latest revision if only the instance is synchronized.
TEST(EVMRegressionTest, CLIPathSynchronizesPreSpuriousDragonHostRevision) {
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "1111111111111111111111111111111111111111");
  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
  const evmc::address CalleeAddr = evmc::literals::operator""_address(
      "a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7");
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex(
          "6000600060006000600073a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7"
          "600ff15a6280010555")
          .value();
  const evmc::bytes32 StorageKey = zen::utils::parseBytes32(
      "0000000000000000000000000000000000000000000000000000000000800105");

  auto RunWithHostRevision = [&](bool SynchronizeHostRevision) -> uint64_t {
    auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
    if (SynchronizeHostRevision) {
      Host->setRevision(EVMC_TANGERINE_WHISTLE);
    }
    Host->accounts[CalleeAddr] = evmc::MockedAccount{};
    Host->accounts[ContractAddr].code =
        evmc::bytes(Bytecode.data(), Bytecode.size());
    Host->accounts[SenderAddr].set_balance(0x1bc16d674ec80000ULL);
    evmc_tx_context TxCtx{};
    TxCtx.tx_origin = SenderAddr;
    TxCtx.block_coinbase = evmc::address{};
    TxCtx.tx_gas_price = intx::be::store<evmc::uint256be>(intx::uint256(1));
    TxCtx.block_base_fee = intx::be::store<evmc::uint256be>(intx::uint256(1));
    Host->tx_context = TxCtx;

    RuntimeConfig Config;
    Config.Mode = common::RunMode::InterpMode;
    auto RT = Runtime::newEVMRuntime(Config, Host.get());
    EXPECT_TRUE(RT);
    if (!RT) {
      return 0;
    }
    Host->setRuntime(RT.get());

    auto ModRet = RT->loadEVMModule("cli_host_revision", Bytecode.data(),
                                    Bytecode.size());
    EXPECT_TRUE(ModRet);
    if (!ModRet) {
      return 0;
    }
    EVMModule *Mod = *ModRet;

    Isolation *Iso = RT->createManagedIsolation();
    EXPECT_NE(Iso, nullptr);
    if (!Iso) {
      return 0;
    }

    evmc_message Msg{};
    Msg.kind = EVMC_CALL;
    Msg.gas = 1000000;
    Msg.sender = SenderAddr;
    Msg.recipient = ContractAddr;
    Msg.code_address = ContractAddr;

    EXPECT_EQ(zen::utils::applyEvmUpfrontGas(*Host, Msg, 1000000,
                                             EVMC_TANGERINE_WHISTLE),
              zen::utils::EvmUpfrontGasResult::Success);

    auto InstRet = Iso->createEVMInstance(*Mod, Msg.gas);
    EXPECT_TRUE(InstRet);
    if (!InstRet) {
      return 0;
    }
    EVMInstance *Inst = *InstRet;
    Inst->setRevision(EVMC_TANGERINE_WHISTLE);

    evmc::Result ExecResult{};
    RT->callEVMMain(*Inst, Msg, ExecResult);
    EXPECT_EQ(ExecResult.status_code, EVMC_SUCCESS);
    EXPECT_NE(Host->accounts[ContractAddr].storage.find(StorageKey),
              Host->accounts[ContractAddr].storage.end());
    return static_cast<uint64_t>(intx::be::load<intx::uint256>(
        Host->accounts[ContractAddr].storage.at(StorageKey).current));
  };

  const uint64_t SyncedRemainingGas = RunWithHostRevision(true);
  const uint64_t DesyncedRemainingGas = RunWithHostRevision(false);
  EXPECT_EQ(SyncedRemainingGas, DesyncedRemainingGas + 25000);
}

TEST(EVMRegressionTest, Issue593_CreateDoesNotCreditPhantomBalance) {
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "000000000000000000000000000000ca1100f022");
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex(
          "5f5f5f5f5f5f5f63304130305f5ff13030f45f5f5ff05f5f5ff0")
          .value();
  std::vector<uint8_t> Calldata(80, 0x01);

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  Host->accounts[ContractAddr].code =
      evmc::bytes(Bytecode.data(), Bytecode.size());
  Host->accounts[SenderAddr].set_balance(0x3fffffffffffffffULL);
  evmc_tx_context TxCtx{};
  TxCtx.tx_origin = SenderAddr;
  TxCtx.tx_gas_price = intx::be::store<evmc::uint256be>(intx::uint256(0x80));
  TxCtx.block_base_fee = intx::be::store<evmc::uint256be>(intx::uint256(0x10));
  TxCtx.block_coinbase = evmc::literals::operator""_address(
      "b94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  Host->tx_context = TxCtx;

  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;
  auto RT = Runtime::newEVMRuntime(Config, Host.get());
  ASSERT_TRUE(RT);
  Host->setRuntime(RT.get());

  zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
  ExecConfig.ModuleName = "issue593";
  ExecConfig.Bytecode = Bytecode.data();
  ExecConfig.BytecodeSize = Bytecode.size();
  ExecConfig.Revision = EVMC_CANCUN;
  ExecConfig.GasLimit = 20000000;
  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.sender = SenderAddr;
  Msg.recipient = ContractAddr;
  Msg.code_address = ContractAddr;
  Msg.gas = 20000000;
  Msg.input_data = Calldata.data();
  Msg.input_size = Calldata.size();
  ExecConfig.Message = Msg;

  auto Result = Host->executeTransaction(ExecConfig);
  ASSERT_TRUE(Result.Success) << Result.ErrorMessage;

  for (const auto &[Address, Account] : Host->accounts) {
    if (Address == SenderAddr || Address == TxCtx.block_coinbase) {
      continue;
    }
    const intx::uint256 Balance =
        intx::be::load<intx::uint256>(Account.balance);
    EXPECT_EQ(Balance, intx::uint256(0))
        << "No account may receive balance out of thin air";
  }
}

TEST(EVMRegressionTest, Issue610_IntrinsicGasChargedOnce) {
  const std::vector<uint8_t> Bytecode = {0x00}; // STOP
  auto Result = runDtvmCliSettlementTransaction(
      SettlementContractAddr, SettlementSenderAddr, Bytecode, EVMC_CANCUN,
      21000, [&](zen::evm::ZenMockedEVMHost &Host) {
        prepareSettlementHost(Host, Bytecode, SettlementSenderAddr,
                              SettlementContractAddr);
      });
  ASSERT_TRUE(Result.Success) << "A plain STOP transaction should succeed";
  EXPECT_EQ(Result.GasUsed, 21000u)
      << "Only basic intrinsic gas may be charged for an empty contract";
  EXPECT_EQ(Result.GasCharged, 21000u);
  EXPECT_EQ(Result.InitialSenderBalance - Result.FinalSenderBalance,
            SettlementGasPrice * 21000);
}

#ifdef ZEN_ENABLE_MULTIPASS_JIT
TEST(EVMRegressionTest, Issue592_DelegateCallRecursionMultipassSucceeds) {
  const evmc::address SenderAddr = evmc::literals::operator""_address(
      "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
  const evmc::address ContractAddr = evmc::literals::operator""_address(
      "000000000000000000000000000000ca1100f022");
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex("5f5f5f60305f5f5f5f5f5ff23030f45f5f535f5f53").value();

  RuntimeConfig Config;
  Config.Mode = common::RunMode::MultipassMode;
  Config.EnableEvmGasMetering = true;

  auto Host = std::make_unique<zen::evm::ZenMockedEVMHost>();
  Host->accounts[ContractAddr].code =
      evmc::bytes(Bytecode.data(), Bytecode.size());
  Host->accounts[SenderAddr].set_balance(0x3fffffffffffffffULL);
  Host->tx_context.tx_origin = SenderAddr;

  auto RT = Runtime::newEVMRuntime(Config, Host.get());
  ASSERT_TRUE(RT);
  Host->setRuntime(RT.get());

  zen::evm::ZenMockedEVMHost::TransactionExecutionConfig ExecConfig;
  ExecConfig.ModuleName = "issue592_multipass";
  ExecConfig.Bytecode = Bytecode.data();
  ExecConfig.BytecodeSize = Bytecode.size();
  ExecConfig.Revision = EVMC_CANCUN;
  ExecConfig.GasLimit = 20000000;
  evmc_message Msg{};
  Msg.kind = EVMC_CALL;
  Msg.gas = 20000000;
  Msg.sender = SenderAddr;
  Msg.recipient = ContractAddr;
  Msg.code_address = ContractAddr;
  ExecConfig.Message = Msg;

  auto Result = Host->executeTransaction(ExecConfig);
  ASSERT_TRUE(Result.Success) << Result.ErrorMessage;
  ASSERT_EQ(Result.Status, EVMC_SUCCESS)
      << "Multipass must execute the delegatecall recursion without aborting";
}

TEST(EVMRegressionTest, Issue603_KeccakOffsetThenMLoadsMultipassSucceeds) {
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex("6020610100205f515f51").value();
  auto InterpExec = executeEvmBytecode("issue603_interp", Bytecode,
                                       common::RunMode::InterpMode);
  auto MultipassExec = executeEvmBytecode("issue603_multipass", Bytecode,
                                          common::RunMode::MultipassMode);
  ASSERT_EQ(InterpExec.Status, EVMC_SUCCESS);
  ASSERT_EQ(MultipassExec.Status, EVMC_SUCCESS)
      << "Multipass must not crash for KECCAK with a non-zero offset followed "
         "by memory loads";
#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled);
#endif
}

TEST(EVMRegressionTest, Issue594_JumpdestThenMloadMultipassSucceeds) {
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex("5b5f515f5f57").value();
  auto InterpExec = executeEvmBytecode("issue594_interp", Bytecode,
                                       common::RunMode::InterpMode);
  auto MultipassExec = executeEvmBytecode("issue594_multipass", Bytecode,
                                          common::RunMode::MultipassMode);
  ASSERT_EQ(InterpExec.Status, EVMC_SUCCESS);
  ASSERT_EQ(MultipassExec.Status, EVMC_SUCCESS)
      << "Multipass must execute a simple JUMPDEST/MLOAD/JUMPI sequence";
#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled);
#endif
}

// ---------------------------------------------------------------------------
// Review follow-up to the Issue #603 KECCAK memory-cache fix: the specialized
// two-word KECCAK helpers (caller-slot and calldata-slot macro-ops) also take
// the prepared-memory no-expand path and can initialize/grow the instance
// memory on first use. Direct memory operations right after them must not see
// a stale cached base pointer/size.
// ---------------------------------------------------------------------------
TEST(EVMRegressionTest, KeccakCallerSlotThenMLoadsMultipassSucceeds) {
  // CALLER PUSH1 0x40 MSTORE PUSH1 5 PUSH1 0x60 MSTORE
  // PUSH1 0x40 PUSH1 0x40 KECCAK256 PUSH0 MLOAD PUSH0 MLOAD STOP
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex("33604052600560605260406040205f515f5100").value();
  auto InterpExec = executeEvmBytecode("keccak_caller_slot_interp", Bytecode,
                                       common::RunMode::InterpMode);
  auto MultipassExec = executeEvmBytecode(
      "keccak_caller_slot_multipass", Bytecode, common::RunMode::MultipassMode);
  ASSERT_EQ(InterpExec.Status, EVMC_SUCCESS);
  ASSERT_EQ(MultipassExec.Status, EVMC_SUCCESS)
      << "Multipass must not crash for the caller-slot KECCAK macro-op "
         "followed by memory loads";
#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled);
#endif
}

TEST(EVMRegressionTest, KeccakCallDataSlotThenMLoadsMultipassSucceeds) {
  // PUSH1 0 CALLDATALOAD PUSH1 0x40 MSTORE PUSH1 5 PUSH1 0x60 MSTORE
  // PUSH1 0x40 PUSH1 0x40 KECCAK256 PUSH0 MLOAD PUSH0 MLOAD STOP
  const std::vector<uint8_t> Bytecode =
      zen::utils::fromHex("600035604052600560605260406040205f515f5100").value();
  const std::vector<uint8_t> CallData = makeUint256Calldata(0x1234);
  auto InterpExec = executeEvmBytecode("keccak_calldata_slot_interp", Bytecode,
                                       common::RunMode::InterpMode, CallData);
  auto MultipassExec =
      executeEvmBytecode("keccak_calldata_slot_multipass", Bytecode,
                         common::RunMode::MultipassMode, CallData);
  ASSERT_EQ(InterpExec.Status, EVMC_SUCCESS);
  ASSERT_EQ(MultipassExec.Status, EVMC_SUCCESS)
      << "Multipass must not crash for the calldata-slot KECCAK macro-op "
         "followed by memory loads";
#ifdef ZEN_ENABLE_JIT
  EXPECT_TRUE(MultipassExec.JITCompiled);
#endif
}
#endif // ZEN_ENABLE_MULTIPASS_JIT

// ---------------------------------------------------------------------------
// AUTH (0xf6) / AUTHCALL (0xf7) interpreter tests (EIP-3074, revision OSAKA).
//
// The opcodes are still undefined in the vendored evmc catalog at the time of
// writing, so every case drives them with raw bytecode (0xf6 / 0xf7) instead
// of referencing opcode enums.  All signature vectors were generated offline
// (secp256k1 + keccak256) and are hardcoded; the interpreter never signs.
//
// AUTH    : stack (authority, offset, length); memory[offset..offset+97) holds
//           yParity(1) || r(32) || s(32) || commit(32); the signed message is
//           keccak256(0x04 || chainId(32) || nonce(32) || invoker(32) ||
//           commit(32)).
// AUTHCALL: stack (gas, addr, value, argsOffset, argsLength, retOffset,
//           retLength); like CALL but sender = authorized, value comes from
//           the authorized balance, no stipend, and gas == 0 forwards
//           all_but_one_64th of the remaining gas (EIP-150).
// ---------------------------------------------------------------------------
namespace {

const evmc::address kAuthContractAddr = evmc::literals::operator""_address(
    "00000000000000000000000000000000000000f1");
const evmc::address kAuthSenderAddr = evmc::literals::operator""_address(
    "a94f5374fce5edbc8e2a8697c15331677e6ebf0b");
const evmc::address kAuthChildAddr = evmc::literals::operator""_address(
    "00000000000000000000000000000000000000aa");
const evmc::address kAuthEmptyAddr = evmc::literals::operator""_address(
    "00000000000000000000000000000000000000bb");

// Offline-generated EIP-3074 vectors (see companion generator script):
//   V1: chainId=0, nonce=0, invoker=0x00..00, commit=0x..12340000.
//   V2: chainId=1, nonce=0, invoker=0x..00f1, commit=0xa1..ff.
//   V5: chainId=0, nonce=0, invoker=0x..00f1, commit=0xa1..ff.
//   V8: chainId=0, nonce=5, invoker=0x00..00, commit=0x..12340000.
constexpr std::string_view kV1AuthorityHex =
    "19e7e376e7c213b7e7e7e46cc70a5dd086daff2a";
constexpr std::string_view kV1Sig0 =
    "01c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709e";
constexpr std::string_view kV1Sig1 =
    "e526db451f87f8cdfa75b26deb193ac1e9c5668fbb99e87b07ecacb5aa76a77c";
constexpr std::string_view kV1Sig2 =
    "6000000000000000000000000000000000000000000000000000000000123400";
constexpr std::string_view kV1LastByte = "00";

constexpr std::string_view kV2AuthorityHex =
    "1563915e194d8cfba1943570603f7606a3115508";
constexpr std::string_view kV2Sig0 =
    "00f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036";
constexpr std::string_view kV2Sig1 =
    "f965ea0e72920f51797d59b4ad57dd0b4bfb829a78ccba3bff777b6ea4bbd99e";
constexpr std::string_view kV2Sig2 =
    "aca1b2c3d4e5f60718293a4b5c6d7e8f900102030405060708090a0b0c0d0e0f";
constexpr std::string_view kV2LastByte = "ff";

constexpr std::string_view kV5AuthorityHex =
    "e1fae9b4fab2f5726677ecfa912d96b0b683e6a9";
constexpr std::string_view kV5Sig0 =
    "015cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9";
constexpr std::string_view kV5Sig1 =
    "bc6ed6ab8804052f39d5f7bc33dd9f1e1aed2520aab7b00286425663f8e5a4e2";
constexpr std::string_view kV5Sig2 =
    "58a1b2c3d4e5f60718293a4b5c6d7e8f900102030405060708090a0b0c0d0e0f";
constexpr std::string_view kV5LastByte = "ff";

constexpr std::string_view kV8AuthorityHex =
    "62f94e9ac9349bccc61bfe66ddade6292702ecb6";
constexpr std::string_view kV8Sig0 =
    "01f28773c2d975288bc7d1d205c3748651b075fbc6610e58cddeeddf8f19405a";
constexpr std::string_view kV8Sig1 =
    "a859cb0db3ef54fadc40de2ac7a4def77688a14cbe74846eb5d1592117d61982";
constexpr std::string_view kV8Sig2 =
    "7600000000000000000000000000000000000000000000000000000000123400";
constexpr std::string_view kV8LastByte = "00";

constexpr std::string_view kS = "0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOne =
    "0000000000000000000000000000000000000000000000000000000000000001";

// Child that returns CALLER: CALLER PUSH0 MSTORE PUSH1 32 PUSH0 RETURN.
constexpr std::string_view kChildReturnCallerHex = "335f5260205ff3";
// Child that returns its remaining gas: GAS PUSH0 MSTORE PUSH1 32 PUSH0 RETURN.
constexpr std::string_view kChildReturnGasHex = "5a5f5260205ff3";

evmc::address AuthAddrFromHex(std::string_view Hex) {
  return evmc::literals::operator""_address(std::string(Hex).c_str());
}

std::string UpperHex(std::string_view S) {
  std::string R(S);
  for (char &C : R) {
    C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
  }
  return R;
}

std::string AddrHex(const evmc::address &A) {
  return zen::utils::toHex(A.bytes, sizeof(A.bytes));
}

std::string ToUint256Hex(uint64_t V) {
  // 32-byte big-endian hex (64 chars), matching zen::utils::toHex output for
  // a 32-byte word: value occupies the least-significant 8 bytes.  Only the
  // low 8 bytes (16 hex chars) are written; shifting uint64_t by >= 64 bits
  // is UB (the x86 shift-count mask would ghost-copy the value upwards).
  std::string Hex(64, '0');
  static const char *kDigits = "0123456789ABCDEF";
  for (int I = 0; I < 8; ++I) {
    Hex[62 - 2 * I] = kDigits[(V >> (8 * I + 4)) & 0xF];
    Hex[63 - 2 * I] = kDigits[(V >> (8 * I)) & 0xF];
  }
  return Hex;
}

// Minimal big-endian PUSH of an unsigned constant (PUSH1/PUSH2/PUSH3).
std::string PushBytes(uint64_t Value) {
  char Buf[16];
  if (Value <= 0xFF) {
    std::snprintf(Buf, sizeof(Buf), "%02X", static_cast<unsigned>(Value));
    return "60" + std::string(Buf);
  }
  if (Value <= 0xFFFF) {
    std::snprintf(Buf, sizeof(Buf), "%04X", static_cast<unsigned>(Value));
    return "61" + std::string(Buf);
  }
  std::snprintf(Buf, sizeof(Buf), "%06X", static_cast<unsigned>(Value));
  return "62" + std::string(Buf);
}

// Assembles the 97-byte signature block at memory[0..97):
//   word0 = yParity || r[0..31)
//   word1 = r[31]   || s[0..31)
//   word2 = s[31]   || commit[0..31)
//   byte  = commit[31]
std::string SigStoreHex(std::string_view W0, std::string_view W1,
                        std::string_view W2, std::string_view Last) {
  std::string Hex;
  Hex += "7f";  // PUSH32 W0
  Hex += W0;
  Hex += "600052";  // PUSH1 0x00 MSTORE
  Hex += "7f";  // PUSH32 W1
  Hex += W1;
  Hex += "602052";  // PUSH1 0x20 MSTORE
  Hex += "7f";  // PUSH32 W2
  Hex += W2;
  Hex += "604052";  // PUSH1 0x40 MSTORE
  Hex += "60";  // PUSH1 commit[31]
  Hex += Last;
  Hex += "606053";  // PUSH1 0x60 MSTORE8
  return Hex;
}

// AUTH(length, 0, authority) = "PUSH1 length PUSH1 0 PUSH20 authority AUTH".
std::string AuthHex(uint8_t Length, std::string_view AuthorityHex) {
  std::string Hex;
  Hex += "60";
  char Buf[3];
  std::snprintf(Buf, sizeof(Buf), "%02x", static_cast<unsigned>(Length));
  Hex += Buf;
  Hex += "6000";  // PUSH1 0x00 (offset)
  Hex += "73";  // PUSH20
  Hex += AuthorityHex;
  Hex += "f6";  // AUTH
  return Hex;
}

// AUTHCALL with the given target/gas/value.  Stack order (top first): gas,
// addr, value, argsOffset, argsLength, retOffset, retLength, so operands are
// pushed in the reverse order.
std::string AutcallHex(std::string_view AddrHex, uint32_t Gas,
                       std::string_view ValueWord, uint8_t ArgsLength = 0,
                       uint8_t ArgsOffset = 0, uint8_t RetLength = 32,
                       uint8_t RetOffset = 0) {
  std::string Hex;
  Hex += "60";
  char Buf[3];
  std::snprintf(Buf, sizeof(Buf), "%02x", static_cast<unsigned>(RetLength));
  Hex += Buf;
  std::snprintf(Buf, sizeof(Buf), "%02x", static_cast<unsigned>(RetOffset));
  Hex += "60";
  Hex += Buf;
  std::snprintf(Buf, sizeof(Buf), "%02x", static_cast<unsigned>(ArgsLength));
  Hex += "60";
  Hex += Buf;
  std::snprintf(Buf, sizeof(Buf), "%02x", static_cast<unsigned>(ArgsOffset));
  Hex += "60";
  Hex += Buf;
  Hex += "7f";  // PUSH32 value
  Hex += ValueWord;
  Hex += "73";  // PUSH20 addr
  Hex += AddrHex;
  Hex += PushBytes(Gas);
  Hex += "f7";  // AUTHCALL
  return Hex;
}

// CALL(child, gas, value): CALL with args/ret areas at 0.  Stack order (top
// first): gas, addr, value, argsOffset, argsLength, retOffset, retLength, so
// operands are pushed in the reverse order.  Then store the CALL success flag
// into memory[0] and RETURN it.
std::string CallStoreResultHex(std::string_view AddrHex, uint32_t Gas,
                               std::string_view ValueWord) {
  std::string Hex;
  Hex += "5f";  // PUSH0 retLength
  Hex += "5f";  // PUSH0 retOffset
  Hex += "5f";  // PUSH0 argsLength
  Hex += "5f";  // PUSH0 argsOffset
  Hex += "7f";  // PUSH32 value
  Hex += ValueWord;
  Hex += "73";  // PUSH20 addr
  Hex += AddrHex;
  Hex += PushBytes(Gas);
  Hex += "f1";  // CALL
  // Store the CALL success flag into memory[0] and return it as a word.
  Hex += "5f";
  Hex += "52";  // PUSH0 MSTORE
  Hex += "6020";  // PUSH1 32
  Hex += "5f";  // PUSH0
  Hex += "f3";  // RETURN
  return Hex;
}

// DELEGATECALL(child, gas): DELEGATECALL has no value operand.  Stack order
// (top first): gas, addr, argsOffset, argsLength, retOffset, retLength, so
// operands are pushed in the reverse order.  Store and return the flag.
std::string DelegateStoreResultHex(std::string_view AddrHex, uint32_t Gas) {
  std::string Hex;
  Hex += "5f";  // PUSH0 retLength
  Hex += "5f";  // PUSH0 retOffset
  Hex += "5f";  // PUSH0 argsLength
  Hex += "5f";  // PUSH0 argsOffset
  Hex += "73";  // PUSH20 addr
  Hex += AddrHex;
  Hex += PushBytes(Gas);
  Hex += "f4";  // DELEGATECALL
  Hex += "5f";
  Hex += "52";  // PUSH0 MSTORE
  Hex += "6020";  // PUSH1 32
  Hex += "5f";  // PUSH0
  Hex += "f3";  // RETURN
  return Hex;
}

// Stores the top stack word to memory[0] and returns it: used to surface the
// AUTH success flag (0/1) as the program output.
std::string StoreTopAndReturnHex() {
  return "5f52"      // PUSH0 MSTORE
         "6020" "5f" "f3";  // RETURN(0, 32)
}

// RETURNDATACOPY it into memory[0..32) and RETURN: used to surface the child
// return data through AUTHCALL exactly like CALL.
std::string ReturndataToOutputHex() {
  // PUSH1 0x20 PUSH1 0x00 PUSH0 RETURNDATACOPY  ... (retLen=32, retOff=0)
  // then RETURN(0, 32).
  return "6020" "6000" "5f" "3e" "6020" "5f" "f3";
}

evmc::bytes BytesFromHex(std::string_view Hex) {
  auto Buf = zen::utils::fromHex(std::string(Hex));
  EXPECT_TRUE(Buf) << "Bad test bytecode " << Hex;
  return evmc::bytes(Buf->begin(), Buf->end());
}

struct AuthCallRunResult {
  evmc_status_code Status = EVMC_INTERNAL_ERROR;
  int64_t GasLeft = 0;
  std::string OutputHex;
};

// Host-attached interpreter runner: creates a fresh ZenMockedEVMHost, lets the
// caller prepare accounts / tx_context / code, then executes Bytecode in a
// fresh instance at the requested revision.
template <typename PrepareHostT>
AuthCallRunResult RunAuthProgram(const std::string &ModuleName,
                                 const std::vector<uint8_t> &Bytecode,
                                 evmc_revision Revision, uint32_t Flags,
                                 const evmc::address &Recipient,
                                 uint64_t GasLimit, PrepareHostT &&Prepare) {
  RuntimeConfig Config;
  Config.Mode = common::RunMode::InterpMode;

  auto HostPtr = std::make_unique<zen::evm::ZenMockedEVMHost>();
  std::forward<PrepareHostT>(Prepare)(*HostPtr);

  auto RT = Runtime::newEVMRuntime(Config, HostPtr.get());
  if (!RT) {
    return AuthCallRunResult{};
  }
  HostPtr->setRuntime(RT.get());

  auto ModRet = RT->loadEVMModule(ModuleName, Bytecode.data(), Bytecode.size());
  if (!ModRet) {
    return AuthCallRunResult{};
  }
  EVMModule *Mod = *ModRet;

  Isolation *Iso = RT->createManagedIsolation();
  if (!Iso) {
    return AuthCallRunResult{};
  }

  auto InstRet = Iso->createEVMInstance(*Mod, GasLimit);
  if (!InstRet) {
    return AuthCallRunResult{};
  }
  EVMInstance *Inst = *InstRet;
  Inst->setRevision(Revision);

  evmc_message Msg = {};
  Msg.kind = EVMC_CALL;
  Msg.flags = Flags;
  Msg.depth = 0;
  Msg.gas = static_cast<int64_t>(GasLimit);
  Msg.recipient = Recipient;
  Msg.sender = kAuthSenderAddr;
  Msg.code_address = Recipient;
  Msg.code = reinterpret_cast<const uint8_t *>(Mod->Code);
  Msg.code_size = Mod->CodeSize;

  evmc::Result RawResult;
  EXPECT_NO_THROW({ RT->callEVMMain(*Inst, Msg, RawResult); });
  AuthCallRunResult Result;
  Result.Status = RawResult.status_code;
  Result.GasLeft = RawResult.gas_left;
  Result.OutputHex =
      zen::utils::toHex(RawResult.output_data, RawResult.output_size);
  return Result;
}

}  // namespace

// Contract item #1: a valid signature restores the authority address and a
// subsequent AUTHCALL executes the child with msg.sender == authorized.  The
// child returns CALLER, which must equal the recovered authority.
TEST(EVMAuthCall, ValidSignatureMakesAutcallCallerAuthorized) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + "50" +  // POP the AUTH flag
      AutcallHex(AddrHex(kAuthChildAddr), 50000, std::string(kS)) +
      ReturndataToOutputHex());
  ASSERT_TRUE(Bytecode) << "Failed to build AUTH bytecode";

  auto Authority = AuthAddrFromHex(kV5AuthorityHex);
  auto Result = RunAuthProgram(
      "auth_valid", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[kAuthChildAddr].code = BytesFromHex(kChildReturnCallerHex);
        Host.accounts[Authority].set_balance(1000000);
      });

  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  // The child frame's CALLER must be the recovered authority (32-byte padded).
  const std::string PaddedAuthority =
      std::string(24, '0') + UpperHex(kV5AuthorityHex);
  EXPECT_EQ(Result.OutputHex, PaddedAuthority)
      << "AUTHCALL must set the child msg.sender to the authorized address";
}

// Contract item #2: AUTH costs 3100 + memory expansion + EIP-2929 access on
// the authority.  The warm/cold difference is exactly 2600 - 100 = 2500.
TEST(EVMAuthCall, AUTHChargesWarmOrColdAuthorityAccess) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + StoreTopAndReturnHex());
  ASSERT_TRUE(Bytecode) << "Failed to build AUTH bytecode";

  auto Authority = AuthAddrFromHex(kV5AuthorityHex);
  const auto Run = [&](bool Warm) {
    return RunAuthProgram(
        "auth_warm_cold", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr,
        1000000,
        [&](zen::evm::ZenMockedEVMHost &Host) {
          if (Warm) {
            Host.access_account(Authority);
          }
        });
  };

  auto Cold = Run(false);
  auto Warm = Run(true);
  ASSERT_EQ(Cold.Status, EVMC_SUCCESS);
  ASSERT_EQ(Warm.Status, EVMC_SUCCESS);
  EXPECT_EQ(Cold.GasLeft, Warm.GasLeft - 2500)
      << "AUTH must charge 2600 for a cold authority and 100 for a warm one";
}

// Contract item #6 (gas): the memory expansion fee is charged on the full
// declared length, not clamped to 97.  length=200 must expand memory further
// than length=97 by exactly the 9-gas difference (4 vs 7 words).
TEST(EVMAuthCall, AUTHMemoryExpansionChargedForFullLength) {
  const auto Run = [&](uint8_t Length) {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
        AuthHex(Length, kV5AuthorityHex) + StoreTopAndReturnHex());
    if (!Bytecode) {
      return AuthCallRunResult{};
    }
    return RunAuthProgram(
        "auth_memexp", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
        [](zen::evm::ZenMockedEVMHost &) {});
  };

  auto Short = Run(97);
  auto Long = Run(200);
  ASSERT_EQ(Short.Status, EVMC_SUCCESS);
  ASSERT_EQ(Long.Status, EVMC_SUCCESS);
  // ceil(97/32)=4 words -> 3*4 + 4^2/512 = 12; ceil(200/32)=7 -> 21.  Delta 9.
  EXPECT_EQ(Long.GasLeft, Short.GasLeft - 9)
      << "AUTH memory expansion must be charged on the full declared length";
}

// Contract item #3: invalid signatures (garbage r, bad yParity, high-half s,
// tampered commit) make AUTH return 0 and reset authorized, so a subsequent
// AUTHCALL is invalid execution (EVMC_INVALID_INSTRUCTION).
TEST(EVMAuthCall, InvalidSignatureResetsAuthorized) {
  struct Case {
    const char *Name;
    std::string_view Sig0;
    std::string_view Sig1;
    std::string_view Sig2;
    std::string_view SigLast;
  };
  const std::vector<Case> Cases = {
      // r >= n: full 0xFF r is invalid.
      {"r_garbage",
       "01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
       "ff26db451f87f8cdfa75b26deb193ac1e9c5668fbb99e87b07ecacb5aa76a77c",
       kV1Sig2, kV1LastByte},
      // yParity outside {0, 1} is rejected before recovery.
      {"yparity_2", "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709e",
       kV1Sig1, kV1Sig2, kV1LastByte},
      // s in the upper half (malleability) is rejected.
      {"s_high_half", kV1Sig0,
       "e5d924bae0780732058a4d9214e6c53e14f5484d2b15602533d325a8e2598ec4",
       "e100000000000000000000000000000000000000000000000000000000123400", "00"},
      // commit byte differs from the signed message.
      {"commit_tamper", kV1Sig0, kV1Sig1, kV1Sig2, "01"},
  };

  for (const auto &C : Cases) {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(C.Sig0, C.Sig1, C.Sig2, C.SigLast) +
        AuthHex(97, kV1AuthorityHex) + "50" +  // POP AUTH flag (0)
        AutcallHex(AddrHex(kAuthEmptyAddr), 0, std::string(kS), 0, 0,
                   0, 0));
    ASSERT_TRUE(Bytecode) << "Failed to build invalid-signature bytecode: "
                          << C.Name;
    auto Result = RunAuthProgram(
        std::string("auth_invalid_") + C.Name, *Bytecode, EVMC_OSAKA, 0u,
        evmc::address{}, 1000000, [](zen::evm::ZenMockedEVMHost &) {});
    EXPECT_EQ(Result.Status, EVMC_INVALID_INSTRUCTION)
        << "AUTHCALL after failed AUTH must be invalid execution ("
        << C.Name << ")";
  }
}

// Contract item #4: message-field bindings.  Changing the host chain id, the
// signer nonce, or the executing (invoker) address must each break recovery.
TEST(EVMAuthCall, ChainIdNonceAndInvokerBindToMessage) {
  // chainId mismatch: vector V2 is signed for chainId=1, host reports 0.
  {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV2Sig0, kV2Sig1, kV2Sig2, kV2LastByte) +
        AuthHex(97, kV2AuthorityHex) + StoreTopAndReturnHex());
    ASSERT_TRUE(Bytecode);
    auto Result = RunAuthProgram(
        "auth_chainid", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
        [](zen::evm::ZenMockedEVMHost &Host) {
          Host.tx_context.chain_id =
              intx::be::store<evmc::uint256be>(intx::uint256(0));
        });
    ASSERT_EQ(Result.Status, EVMC_SUCCESS);
    EXPECT_EQ(Result.OutputHex, ToUint256Hex(0))
        << "AUTH must fail when the signed chainId differs from host chain_id";
  }
  // nonce mismatch: vector V8 is signed for nonce=5 (and invoker=0x00..00),
  // the host reports nonce 0 and the frame runs at the zero address so that
  // only the nonce field differs.
  {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV8Sig0, kV8Sig1, kV8Sig2, kV8LastByte) +
        AuthHex(97, kV8AuthorityHex) + StoreTopAndReturnHex());
    ASSERT_TRUE(Bytecode);
    auto V8Authority = AuthAddrFromHex(kV8AuthorityHex);
    auto Result = RunAuthProgram(
        "auth_nonce", *Bytecode, EVMC_OSAKA, 0u, evmc::address{}, 1000000,
        [&](zen::evm::ZenMockedEVMHost &Host) {
          Host.accounts[V8Authority].nonce = 0;
        });
    ASSERT_EQ(Result.Status, EVMC_SUCCESS);
    EXPECT_EQ(Result.OutputHex, ToUint256Hex(0))
        << "AUTH must fail when the signed nonce differs from the account "
           "nonce";
  }
  // invoker mismatch: vector V1 is signed for invoker=0x00..00, but the frame
  // runs at kAuthContractAddr (0x..00f1).
  {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV1Sig0, kV1Sig1, kV1Sig2, kV1LastByte) +
        AuthHex(97, kV1AuthorityHex) + StoreTopAndReturnHex());
    ASSERT_TRUE(Bytecode);
    auto Result = RunAuthProgram(
        "auth_invoker", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
        [](zen::evm::ZenMockedEVMHost &) {});
    ASSERT_EQ(Result.Status, EVMC_SUCCESS);
    EXPECT_EQ(Result.OutputHex, ToUint256Hex(0))
        << "AUTH must fail when the signed invoker differs from the frame "
           "address";
  }
}

// Contract item #5: an authority with non-empty code (EXTCODESIZE != 0) is
// rejected even with a perfectly valid signature, and authorized is reset.
TEST(EVMAuthCall, AuthorityWithCodeRejected) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + StoreTopAndReturnHex());
  ASSERT_TRUE(Bytecode);
  auto Authority = AuthAddrFromHex(kV5AuthorityHex);
  auto Result = RunAuthProgram(
      "auth_extcodesize", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr,
      1000000,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[Authority].code = evmc::bytes{0x00};
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, ToUint256Hex(0))
      << "AUTH must fail when the authority has code (EXTCODESIZE != 0)";
}

// Contract item #7: AUTHCALL without a preceding successful AUTH throws an
// invalid execution (consumes the frame's gas, EVMC_INVALID_INSTRUCTION).
TEST(EVMAuthCall, AutcallWithoutAuthIsInvalidExecution) {
  auto Bytecode = zen::utils::fromHex(
      AutcallHex(AddrHex(kAuthEmptyAddr), 100000, std::string(kS)));
  ASSERT_TRUE(Bytecode);
  auto Result = RunAuthProgram(
      "authcall_noauth", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
      [](zen::evm::ZenMockedEVMHost &) {});
  EXPECT_EQ(Result.Status, EVMC_INVALID_INSTRUCTION)
      << "AUTHCALL with authorized unset must be invalid execution";
}

// Contract item #8: AUTHCALL value must not exceed the authorized balance,
// even when the invoker is rich.  Value 1 with authorized balance 0 throws.
TEST(EVMAuthCall, AutcallValueAboveAuthorizedBalanceIsInvalid) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + "50" +
      AutcallHex(AddrHex(kAuthEmptyAddr), 100000, std::string(kOne)));
  ASSERT_TRUE(Bytecode);
  auto Authority = AuthAddrFromHex(kV5AuthorityHex);
  auto Result = RunAuthProgram(
      "authcall_value", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[kAuthContractAddr].set_balance(1000000);  // invoker rich
        Host.accounts[Authority].set_balance(0);                 // authorized poor
      });
  EXPECT_EQ(Result.Status, EVMC_INVALID_INSTRUCTION)
      << "AUTHCALL value must be checked against the authorized balance";
}

// Contract item #9: no 2300 stipend.  AUTHCALL with value > 0 forwards the
// exact requested gas; the child reports 50000 - 2 (GAS opcode cost).
TEST(EVMAuthCall, AutcallHasNoStipend) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + "50" +
      AutcallHex(AddrHex(kAuthChildAddr), 50000, std::string(kOne)) +
      ReturndataToOutputHex());
  ASSERT_TRUE(Bytecode);
  auto Authority = AuthAddrFromHex(kV5AuthorityHex);
  auto Result = RunAuthProgram(
      "authcall_stipend", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr,
      1000000,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[kAuthChildAddr].code = BytesFromHex(kChildReturnGasHex);
        Host.accounts[Authority].set_balance(1000000);
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, ToUint256Hex(50000 - 2))
      << "AUTHCALL must not add a 2300 stipend for value > 0";
}

// Contract item #9 (EIP-150): gas == 0 forwards all_but_one_64th of the
// remaining gas.  A balance-transfer-free call with value 0, a warm target, no
// memory expansion leaves remaining R after the 100 warm_storage_read; the
// child must see R - R/64.
TEST(EVMAuthCall, AutcallGasZeroForwardsAllButOne64th) {
  constexpr uint64_t kGasLimit = 1000000;
  // Parent program: signature stores (48) + AUTH with warm authority
  // (3+3+3 pushes + 3100 + 100) + POP (2) + AUTHCALL arg pushes (21).
  // The 100 warm_storage_read of AUTHCALL is charged before computing the
  // all_but_one_64th remainder.
  constexpr uint64_t kPrefix = 48 + 9 + 3100 + 100 + 2 + 21;
  const uint64_t R = kGasLimit - kPrefix - 100;  // warm_storage_read
  const uint64_t Expected = R - R / 64;

  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + "50" +
      AutcallHex(AddrHex(kAuthChildAddr), 0, std::string(kS)) +
      ReturndataToOutputHex());
  ASSERT_TRUE(Bytecode);
  auto Authority = AuthAddrFromHex(kV5AuthorityHex);
  auto Result = RunAuthProgram(
      "authcall_63_64", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr,
      kGasLimit,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[kAuthChildAddr].code = BytesFromHex(kChildReturnGasHex);
        Host.accounts[Authority].set_balance(1000000);
        Host.access_account(kAuthChildAddr);  // warm target, no cold surcharge
        Host.access_account(Authority);       // warm authority
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, ToUint256Hex(Expected - 2))
      << "AUTHCALL with gas=0 must forward all_but_one_64th (EIP-150)";
}

// Contract item #10: dynamic costs.  Cold target adds 2500; value > 0 adds
// 6700 (not CALL's 9000); value to an empty account adds 25000.
TEST(EVMAuthCall, AutcallDynamicCosts) {
  const auto Run = [&](std::string_view AddrHex, uint32_t Gas,
                       std::string_view ValueWord, bool WarmTarget,
                       bool TargetEmpty) {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
        AuthHex(97, kV5AuthorityHex) + "50" +
        AutcallHex(std::string(AddrHex), Gas, std::string(ValueWord)));
    if (!Bytecode) {
      return AuthCallRunResult{};
    }
    auto Target = AuthAddrFromHex(AddrHex);
    auto Authority = AuthAddrFromHex(kV5AuthorityHex);
    return RunAuthProgram(
        "authcall_dyn", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
        [&](zen::evm::ZenMockedEVMHost &Host) {
          Host.accounts[Authority].set_balance(1000000);
          if (!TargetEmpty) {
            // Non-empty target: account exists with code, but the code is the
            // zero-execution STOP so the subcall consumes no extra gas; this
            // isolates the empty-account 25000 surcharge from local exec gas.
            Host.accounts[Target].code = evmc::bytes{0x00};
          }
          if (WarmTarget) {
            Host.access_account(Target);
          }
        });
  };

  const std::string ChildHex = AddrHex(kAuthChildAddr);
  const std::string EmptyHex = AddrHex(kAuthEmptyAddr);

  // (a) cold vs warm target, value = 0: delta 2500.
  auto Cold = Run(EmptyHex, 100000, std::string(kS), false, true);
  auto Warm = Run(EmptyHex, 100000, std::string(kS), true, true);
  ASSERT_EQ(Cold.Status, EVMC_SUCCESS);
  ASSERT_EQ(Warm.Status, EVMC_SUCCESS);
  EXPECT_EQ(Cold.GasLeft, Warm.GasLeft - 2500)
      << "AUTHCALL to a cold address must add 2500";

  // (b) value 0 vs 1 to a warm non-empty target: delta 6700 (not 9000).
  auto NoValue = Run(ChildHex, 100000, std::string(kS), true, false);
  auto WithValue = Run(ChildHex, 100000, std::string(kOne), true, false);
  ASSERT_EQ(NoValue.Status, EVMC_SUCCESS);
  ASSERT_EQ(WithValue.Status, EVMC_SUCCESS);
  EXPECT_EQ(WithValue.GasLeft, NoValue.GasLeft - 6700)
      << "AUTHCALL value transfer must cost 6700, not CALL's 9000";

  // (c) value=1 to an empty account adds 25000 over a non-empty target.
  auto EmptyTarget = Run(EmptyHex, 100000, std::string(kOne), true, true);
  auto CodeTarget = Run(ChildHex, 100000, std::string(kOne), true, false);
  ASSERT_EQ(EmptyTarget.Status, EVMC_SUCCESS);
  ASSERT_EQ(CodeTarget.Status, EVMC_SUCCESS);
  EXPECT_EQ(EmptyTarget.GasLeft, CodeTarget.GasLeft - 25000)
      << "AUTHCALL value to an empty account must add 25000";
}

// Contract item #11: authorized persists across several AUTHCALLs; the second
// AUTHCALL still executes with msg.sender == authorized.
TEST(EVMAuthCall, AuthorizedPersistsAcrossMultipleAutcalls) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + "50" +
      AutcallHex(AddrHex(kAuthChildAddr), 50000, std::string(kS)) +
      "50" +  // POP first success
      AutcallHex(AddrHex(kAuthChildAddr), 50000, std::string(kS)) +
      ReturndataToOutputHex());
  ASSERT_TRUE(Bytecode);
  auto Result = RunAuthProgram(
      "auth_persist", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[kAuthChildAddr].code = BytesFromHex(kChildReturnCallerHex);
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  const std::string Padded =
      std::string(24, '0') + UpperHex(kV5AuthorityHex);
  EXPECT_EQ(Result.OutputHex, Padded)
      << "authorized must persist across multiple AUTHCALLs in one frame";
}

// Contract item #12: authorized is frame-scoped and must not leak through
// CALL or DELEGATECALL into a nested frame.  A child that runs AUTHCALL in a
// fresh frame gets invalid execution (call returns 0).
TEST(EVMAuthCall, AuthorizedDoesNotLeakIntoNestedFrames) {
  // Child bytecode: AUTHCALL(gas=0xffff, empty addr, value=0) and return its
  // success flag.  In a fresh frame authorized is unset, so this invalid.
  std::string ChildCode =
      "5f" "5f"  // argsLength, argsOffset = 0
      "5f" "5f"  // retLength, retOffset = 0
      "5f"  // value = 0
      "73";  // PUSH20 empty
  ChildCode += AddrHex(kAuthEmptyAddr);
  ChildCode += "61ffff" "f7";  // PUSH2 0xffff AUTHCALL
  ChildCode += "5f52" "6020" "5ff3";  // return the success flag

  auto RunWithChildCode = [&](const std::string &Hex) {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
        AuthHex(97, kV5AuthorityHex) + Hex);
    if (!Bytecode) {
      return AuthCallRunResult{};
    }
    return RunAuthProgram(
        "auth_noleak", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr, 1000000,
        [&](zen::evm::ZenMockedEVMHost &Host) {
          Host.accounts[kAuthChildAddr].code = BytesFromHex(ChildCode);
        });
  };

  // CALL: parent AUTH then CALL the child; child AUTHCALL is invalid
  // execution, so CALL returns 0.
  auto CallResult =
      RunWithChildCode(CallStoreResultHex(AddrHex(kAuthChildAddr),
                                          100000, std::string(kS)));
  ASSERT_EQ(CallResult.Status, EVMC_SUCCESS);
  EXPECT_EQ(CallResult.OutputHex, ToUint256Hex(0))
      << "authorized must not leak into a CALLed frame";

  // DELEGATECALL: same expectation.
  auto DelegateResult =
      RunWithChildCode(DelegateStoreResultHex(AddrHex(kAuthChildAddr),
                                              100000));
  ASSERT_EQ(DelegateResult.Status, EVMC_SUCCESS);
  EXPECT_EQ(DelegateResult.OutputHex, ToUint256Hex(0))
      << "authorized must not leak into a DELEGATECALLed frame";
}

// Contract item #13: revision gating.  The same raw 0xf6 bytecode is an
// undefined instruction at pre-OSAKA revisions and executes at OSAKA.
TEST(EVMAuthCall, AuthUndefinedBeforeOsaka) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV1Sig0, kV1Sig1, kV1Sig2, kV1LastByte) +
      AuthHex(97, kV1AuthorityHex) + StoreTopAndReturnHex());
  ASSERT_TRUE(Bytecode);
  for (auto Revision : {EVMC_PRAGUE, EVMC_CANCUN}) {
    auto Result = RunAuthProgram(
        "auth_revision", *Bytecode, Revision, 0u, kAuthContractAddr, 1000000,
        [](zen::evm::ZenMockedEVMHost &) {});
    EXPECT_EQ(Result.Status, EVMC_UNDEFINED_INSTRUCTION)
        << "0xf6 must be undefined before OSAKA (revision "
        << static_cast<int>(Revision) << ")";
  }
}

// Contract item #14: static context rejects AUTHCALL (mirrors the CALL
// handler) while AUTH itself is allowed because it only verifies a signature.
TEST(EVMAuthCall, StaticContextAllowsAuthButRejectsAutcall) {
  // AUTH alone in a static frame succeeds (returns 1).
  {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
        AuthHex(97, kV5AuthorityHex) + StoreTopAndReturnHex());
    ASSERT_TRUE(Bytecode);
    auto Result = RunAuthProgram(
        "auth_static_ok", *Bytecode, EVMC_OSAKA, EVMC_STATIC, kAuthContractAddr,
        1000000, [](zen::evm::ZenMockedEVMHost &) {});
    ASSERT_EQ(Result.Status, EVMC_SUCCESS);
    EXPECT_EQ(Result.OutputHex, ToUint256Hex(1))
        << "AUTH must be allowed in a static context";
  }
  // AUTHCALL in a static frame is rejected with EVMC_STATIC_MODE_VIOLATION.
  {
    auto Bytecode = zen::utils::fromHex(
        SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
        AuthHex(97, kV5AuthorityHex) + "50" +
        AutcallHex(AddrHex(kAuthEmptyAddr), 100000, std::string(kS)));
    ASSERT_TRUE(Bytecode);
    auto Result = RunAuthProgram(
        "authcall_static_bad", *Bytecode, EVMC_OSAKA, EVMC_STATIC,
        kAuthContractAddr, 1000000, [](zen::evm::ZenMockedEVMHost &) {});
    EXPECT_EQ(Result.Status, EVMC_STATIC_MODE_VIOLATION)
        << "AUTHCALL must be rejected in a static context";
  }
}

// Contract item #15: AUTH with insufficient gas for its fixed 3100 + memory
// expansion throws OUT_OF_GAS and leaves no frame behind.
TEST(EVMAuthCall, AuthOutOfGas) {
  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV1Sig0, kV1Sig1, kV1Sig2, kV1LastByte) +
      AuthHex(97, kV1AuthorityHex) + StoreTopAndReturnHex());
  ASSERT_TRUE(Bytecode);
  auto Result = RunAuthProgram(
      "auth_oog", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr,
      1000,  // far below 3100 + memory expansion
      [](zen::evm::ZenMockedEVMHost &) {});
  EXPECT_EQ(Result.Status, EVMC_OUT_OF_GAS);
  EXPECT_TRUE(Result.OutputHex.empty());
}

// Contract item #16: AUTHCALL sets the return-data buffer exactly like CALL,
// so the parent can consume it with RETURNDATACOPY / RETURNDATASIZE.
TEST(EVMAuthCall, AutcallReturnDataMatchesCallSemantics) {
  // Child returns a known 32-byte payload.
  const std::string Payload =
      "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";
  std::string ChildCode =
      "7f";  // PUSH32 payload
  ChildCode += Payload;
  ChildCode += "5f52" "6020" "5ff3";  // MSTORE(0) RETURN(0,32)

  // RETURNDATACOPY(stack: destOffset, offset, size); push size first so that
  // destOffset ends up on top of the stack.
  const std::string Tail =
      std::string("50") +  // POP AUTHCALL success flag
      "6020" +  // PUSH1 32 size
      "5f" +  // PUSH0 offset
      "5f" +  // PUSH0 destOffset
      "3e" +  // RETURNDATACOPY
      "6020" "5f" "f3";  // RETURN(0, 32)

  auto Bytecode = zen::utils::fromHex(
      SigStoreHex(kV5Sig0, kV5Sig1, kV5Sig2, kV5LastByte) +
      AuthHex(97, kV5AuthorityHex) + "50" +
      AutcallHex(AddrHex(kAuthChildAddr), 50000, std::string(kS)) +
      Tail);
  ASSERT_TRUE(Bytecode);

  auto Result = RunAuthProgram(
      "auth_returndata", *Bytecode, EVMC_OSAKA, 0u, kAuthContractAddr,
      1000000,
      [&](zen::evm::ZenMockedEVMHost &Host) {
        Host.accounts[kAuthChildAddr].code = BytesFromHex(ChildCode);
      });
  ASSERT_EQ(Result.Status, EVMC_SUCCESS);
  EXPECT_EQ(Result.OutputHex, Payload)
      << "AUTHCALL must expose child return data via RETURNDATACOPY";
}

// ============================================================================
// JIT suitability gates for AUTH/AUTHCALL (fallback-only degradation contract).
// The EVM JIT frontend has no MIR lowering for these protocol-extension
// opcodes, so EVMAnalyzer must mark modules containing them (when defined at
// the module's revision) as interpreter-fallback.  These tests pin the OSAKA /
// pre-Osaka / PUSH-immediate behavior of analyzeSuitabilityOnly.
// ============================================================================

TEST(EIP3074Auth, JitSuitabilityForcesFallbackForAuthAtOsaka) {
  // PUSH1 0x00 (three operands) AUTH STOP: AUTH consumes 3, produces 1.
  const std::vector<uint8_t> Code =
      authFromHex("600060006000f600");
  COMPILER::EVMAnalyzer Analyzer(EVMC_OSAKA);
  Analyzer.analyzeSuitabilityOnly(Code.data(), Code.size());
  const auto &Suit = Analyzer.getJITSuitability();
  EXPECT_TRUE(Suit.ShouldFallback)
      << "OSAKA module containing defined AUTH must fall back to the "
         "interpreter (no JIT lowering)";
  EXPECT_EQ(Suit.AuthOpcodeCount, 1U);
}

TEST(EIP3074Auth, JitSuitabilityForcesFallbackForAuthCallAtOsaka) {
  const std::vector<uint8_t> Code =
      authFromHex("6000600060006000600060006000f700");
  COMPILER::EVMAnalyzer Analyzer(EVMC_OSAKA);
  Analyzer.analyzeSuitabilityOnly(Code.data(), Code.size());
  const auto &Suit = Analyzer.getJITSuitability();
  EXPECT_TRUE(Suit.ShouldFallback)
      << "OSAKA module containing defined AUTHCALL must fall back to the "
         "interpreter (no JIT lowering)";
  EXPECT_EQ(Suit.AuthOpcodeCount, 1U);
}

TEST(EIP3074Auth, JitSuitabilityDoesNotForceFallbackPreOsaka) {
  // At PRAGUE the 0xf6 byte is an undefined instruction.  Both engines agree
  // on EVMC_UNDEFINED_INSTRUCTION, so no fallback is forced; the opcode count
  // is still tracked for diagnostics.
  const std::vector<uint8_t> Code = authFromHex("f600");
  COMPILER::EVMAnalyzer Analyzer(EVMC_PRAGUE);
  Analyzer.analyzeSuitabilityOnly(Code.data(), Code.size());
  const auto &Suit = Analyzer.getJITSuitability();
  EXPECT_FALSE(Suit.ShouldFallback)
      << "Pre-Osaka undefined 0xf6 must not force interpreter fallback";
  EXPECT_EQ(Suit.AuthOpcodeCount, 1U);
}

TEST(EIP3074Auth, JitSuitabilityIgnoresAuthBytesInsidePushImmediates) {
  // 0xf6/0xf7 as PUSH2 immediate data must not be misread as opcodes.
  const std::vector<uint8_t> Code = authFromHex("61f6f700");
  COMPILER::EVMAnalyzer Analyzer(EVMC_OSAKA);
  Analyzer.analyzeSuitabilityOnly(Code.data(), Code.size());
  const auto &Suit = Analyzer.getJITSuitability();
  EXPECT_EQ(Suit.AuthOpcodeCount, 0U);
  EXPECT_FALSE(Suit.ShouldFallback)
      << "AUTH bytes inside PUSH immediates must not trigger fallback";
}
