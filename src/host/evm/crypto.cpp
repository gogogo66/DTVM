// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "host/evm/crypto.h"
#include "host/evm/keccak/keccak.hpp"
#include "host/evm/secp256k1/secp256k1.hpp"
#include <array>
#include <cstring>

namespace zen::host::evm::crypto {

// Static member definitions
std::unique_ptr<CryptoInterface> CryptoProvider::Instance;
std::once_flag CryptoProvider::Initialized;

// RealCrypto implementation
void CryptoHost::keccak256(const uint8_t *Input, std::size_t InputLen,
                           uint8_t *Output) {
  const auto Result = ethash::keccak256(Input, InputLen);
  std::memcpy(Output, Result.bytes, 32);
  // placeholderKeccak256(Input, InputLen, Output);
}

std::vector<uint8_t> CryptoHost::keccak256(const std::vector<uint8_t> &Input) {
  std::vector<uint8_t> Result(32);
  keccak256(Input.data(), Input.size(), Result.data());
  return Result;
}

bool CryptoHost::ec_recover(const uint8_t *Digest, const uint8_t *R,
                            const uint8_t *S, uint8_t YParity,
                            uint8_t *OutAddress) {
  uint8_t PubKey[64];
  if (!secp256k1::recoverPublicKey(Digest, R, S, YParity, PubKey)) {
    return false;
  }
  const auto Hash = ethash::keccak256(PubKey, sizeof(PubKey));
  std::memcpy(OutAddress, Hash.bytes + 32 - 20, 20);
  return true;
}

// Convenience functions
void keccak256(const uint8_t *Input, size_t InputLen, uint8_t *Output) {
  CryptoProvider::getInstance().keccak256(Input, InputLen, Output);
}

std::vector<uint8_t> keccak256(const std::vector<uint8_t> &Input) {
  return CryptoProvider::getInstance().keccak256(Input);
}

bool ec_recover(const uint8_t *Digest, const uint8_t *R, const uint8_t *S,
                uint8_t YParity, uint8_t *OutAddress) {
  return CryptoProvider::getInstance().ec_recover(Digest, R, S, YParity,
                                                  OutAddress);
}

} // namespace zen::host::evm::crypto
