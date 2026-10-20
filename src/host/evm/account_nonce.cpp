// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "host/evm/account_nonce.h"

#include <mutex>
#include <unordered_map>

namespace zen::host::evm {
namespace {

std::mutex gMutex;
std::unordered_map<const evmc::Host *, AccountNonceProvider> gProviders;

} // namespace

void registerAccountNonceProvider(const evmc::Host *Host,
                                  AccountNonceProvider Provider) {
  std::lock_guard<std::mutex> Lock(gMutex);
  gProviders[Host] = Provider;
}

void unregisterAccountNonceProvider(const evmc::Host *Host) {
  std::lock_guard<std::mutex> Lock(gMutex);
  gProviders.erase(Host);
}

uint64_t getAccountNonce(const evmc::Host *Host, const evmc::address &Addr) {
  AccountNonceProvider Provider;
  {
    std::lock_guard<std::mutex> Lock(gMutex);
    const auto It = gProviders.find(Host);
    if (It == gProviders.end()) {
      return 0;
    }
    Provider = It->second;
  }
  return Provider.Query ? Provider.Query(Addr, Provider.Ctx) : 0;
}

} // namespace zen::host::evm