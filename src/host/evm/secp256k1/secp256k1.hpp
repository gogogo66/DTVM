// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#ifndef ZEN_HOST_EVM_SECP256K1_HPP
#define ZEN_HOST_EVM_SECP256K1_HPP

#include <cstdint>

namespace zen::host::evm::secp256k1 {

/**
 * secp256k1 ECDSA public-key recovery (Ethereum convention, used by EIP-3074
 * AUTH and the ecrecover precompile).
 *
 * Recovers the signer's uncompressed public key (X || Y, 64 bytes big-endian)
 * from a signature (R, S) over a 32-byte message digest with the given
 * y-parity.  Implements the reference SEC1 recovery equation
 * Q = r^-1 * (s*R - z*G) with R = (r, y_parity).
 *
 * Validation follows the Ethereum signature rules enforced by AUTH:
 *   - YParity must be 0 or 1;
 *   - 0 < R < N and 0 < S <= N/2 (low-s, malleability protection);
 *   - (r, y) must be a valid curve point (sqrt of r^3 + 7 must exist).
 *
 * The implementation is a self-contained, deterministic port of the
 * libsecp256k1 reference semantics: 4x64-bit limb field/scalar arithmetic,
 * Fermat-based modular inverse, (p+1)/4 square root (p ≡ 3 mod 4), and
 * Jacobian-coordinate point arithmetic.  It introduces no third-party
 * dependency and is not constant-time (it only processes public data).
 *
 * @param Digest    32-byte message digest (z).
 * @param R         32-byte big-endian signature component r.
 * @param S         32-byte big-endian signature component s.
 * @param YParity   Recovery y-parity: 0 (even y) or 1 (odd y).
 * @param OutPubKey Output buffer of exactly 64 bytes (X || Y big-endian).
 * @return          true on success; false for any invalid input.
 */
bool recoverPublicKey(const uint8_t *Digest, const uint8_t *R, const uint8_t *S,
                      uint8_t YParity, uint8_t *OutPubKey);

} // namespace zen::host::evm::secp256k1

#endif // ZEN_HOST_EVM_SECP256K1_HPP