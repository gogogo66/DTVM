// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#ifndef ZEN_HOST_EVM_ACCOUNT_NONCE_H
#define ZEN_HOST_EVM_ACCOUNT_NONCE_H

#include "evmc/evmc.hpp"

#include <cstdint>

namespace zen::host::evm {

/**
 * Optional host capability used by EIP-3074 AUTH.
 *
 * The evmc::Host interface deliberately exposes no account-nonce accessor,
 * but AUTH must hash the signer's current nonce into the signed message.
 * Hosts that can provide the nonce register a provider here, keyed by the
 * evmc::Host instance the interpreter sees.  When no provider is registered
 * for a host, the deterministic fallback nonce is 0 (documented; production
 * hosts that need authenticating non-zero-nonce accounts must register a
 * provider - see the @B handoff in the AUTH/AUTHCALL implementation report).
 */
struct AccountNonceProvider {
  uint64_t (*Query)(const evmc::address &Addr, void *Ctx) = nullptr;
  void *Ctx = nullptr;
};

void registerAccountNonceProvider(const evmc::Host *Host,
                                  AccountNonceProvider Provider);
void unregisterAccountNonceProvider(const evmc::Host *Host);
uint64_t getAccountNonce(const evmc::Host *Host, const evmc::address &Addr);

} // namespace zen::host::evm

#endif // ZEN_HOST_EVM_ACCOUNT_NONCE_H