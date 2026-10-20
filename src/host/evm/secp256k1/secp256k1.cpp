// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "host/evm/secp256k1/secp256k1.hpp"

#include <cstring>

namespace zen::host::evm::secp256k1 {
namespace {

using Limb = uint64_t;
using Wide = __uint128_t;

struct U256 {
  Limb L[4];
};

// secp256k1 domain parameters, 4x64-bit little-endian limbs.
// Field prime P = 2^256 - 0x1000003D1.
constexpr U256 kP = {0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL,
                     0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
// Fold constant for reductions mod P: FoldC = 2^256 - P.
constexpr U256 kFoldP = {0x1000003D1ULL, 0, 0, 0};
// (P+1)/4: square-root exponent (P ≡ 3 mod 4).
constexpr U256 kPPlus1Over4 = {0xFFFFFFFFBFFFFF0CULL, 0xFFFFFFFFFFFFFFFFULL,
                               0xFFFFFFFFFFFFFFFFULL, 0x3FFFFFFFFFFFFFFFULL};
// Group order N.
constexpr U256 kN = {0xBFD25E8CD0364141ULL, 0xBAAEDCE6AF48A03BULL,
                     0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};
// Fold constant for reductions mod N: FoldC = 2^256 - N.
constexpr U256 kFoldN = {0x402DA1732FC9BEBFULL, 0x4551231950B75FC4ULL, 1, 0};
// N-2 (exponent for Fermat modular inverse mod N) and N/2 (low-s bound).
constexpr U256 kNMinus2 = {0xBFD25E8CD036413FULL, 0xBAAEDCE6AF48A03BULL,
                           0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};
constexpr U256 kNHalf = {0xDFE92F46681B20A0ULL, 0x5D576E7357A4501DULL,
                         0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};
// Generator point G.
constexpr U256 kGx = {0x59F2815B16F81798ULL, 0x029BFCDB2DCE28D9ULL,
                      0x55A06295CE870B07ULL, 0x79BE667EF9DCBBACULL};
constexpr U256 kGy = {0x9C47D08FFB10D4B8ULL, 0xFD17B448A6855419ULL,
                      0x5DA4FBFC0E1108A8ULL, 0x483ADA7726A3C465ULL};
constexpr U256 kOne = {1, 0, 0, 0};
constexpr U256 kZero = {0, 0, 0, 0};

static bool isZero(const U256 &A) {
  return (A.L[0] | A.L[1] | A.L[2] | A.L[3]) == 0;
}

static bool equal(const U256 &A, const U256 &B) {
  return A.L[0] == B.L[0] && A.L[1] == B.L[1] && A.L[2] == B.L[2] &&
         A.L[3] == B.L[3];
}

// Compare as big-endian integers.
static int cmp(const U256 &A, const U256 &B) {
  for (int I = 3; I >= 0; --I) {
    if (A.L[I] > B.L[I]) {
      return 1;
    }
    if (A.L[I] < B.L[I]) {
      return -1;
    }
  }
  return 0;
}

// A - B; the caller must ensure A >= B (big-endian).
static U256 sub256(const U256 &A, const U256 &B) {
  U256 R{};
  uint64_t Borrow = 0;
  for (int I = 0; I < 4; ++I) {
    const Wide T = (Wide)A.L[I] - B.L[I] - Borrow;
    R.L[I] = static_cast<Limb>(T);
    Borrow = (T >> 64) & 1;
  }
  return R;
}

// Full 8-limb (512-bit) product of two 4-limb values.
static void mul256x256(const U256 &A, const U256 &B, Limb Out[8]) {
  Limb R[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int I = 0; I < 4; ++I) {
    uint64_t Carry = 0;
    for (int J = 0; J < 4; ++J) {
      const Wide T = (Wide)A.L[I] * B.L[J] + R[I + J] + Carry;
      R[I + J] = static_cast<Limb>(T);
      Carry = static_cast<Limb>(T >> 64);
    }
    int K = I + 4;
    while (Carry != 0) {
      const Wide T = (Wide)R[K] + Carry;
      R[K] = static_cast<Limb>(T);
      Carry = static_cast<Limb>(T >> 64);
      ++K;
    }
  }
  std::memcpy(Out, R, sizeof(R));
}

// Reduce a value that fits in 257 bits (R + Carry*2^256) to [0, Mod) using
// FoldC = 2^256 - Mod.  At most two carries can occur because FoldC < 2^128.
static U256 reduceFold(U256 R, bool Carry, const U256 &FoldC,
                       const U256 &Mod) {
  for (int Fold = 0; Fold < 3 && Carry; ++Fold) {
    uint64_t C2 = 0;
    U256 R2{};
    for (int I = 0; I < 4; ++I) {
      const Wide T = (Wide)R.L[I] + FoldC.L[I] + C2;
      R2.L[I] = static_cast<Limb>(T);
      C2 = static_cast<Limb>(T >> 64);
    }
    R = R2;
    Carry = C2;
  }
  for (int I = 0; I < 3; ++I) {
    if (cmp(R, Mod) < 0) {
      break;
    }
    R = sub256(R, Mod);
  }
  return R;
}

// (A + B) mod Mod.
static U256 addMod(const U256 &A, const U256 &B, const U256 &Mod,
                   const U256 &FoldC) {
  uint64_t C = 0;
  U256 R{};
  for (int I = 0; I < 4; ++I) {
    const Wide T = (Wide)A.L[I] + B.L[I] + C;
    R.L[I] = static_cast<Limb>(T);
    C = static_cast<Limb>(T >> 64);
  }
  return reduceFold(R, C != 0, FoldC, Mod);
}

// (A - B) mod Mod; requires Mod < 2^256 (always true here).
static U256 subMod(const U256 &A, const U256 &B, const U256 &Mod) {
  if (cmp(A, B) >= 0) {
    return sub256(A, B);
  }
  return sub256(Mod, sub256(B, A));
}

// (A * B) mod Mod via 512-bit product and FoldC folding.
static U256 mulMod(const U256 &A, const U256 &B, const U256 &Mod,
                   const U256 &FoldC) {
  Limb Prod[8];
  mul256x256(A, B, Prod);
  // Fold while the upper 256 bits are non-zero: value = Low + Top*2^256
  // ≡ Low + Top*FoldC (mod Mod).  The top part shrinks geometrically
  // because FoldC < 2^128, so this terminates within a few iterations.
  for (int Iter = 0; Iter < 8; ++Iter) {
    bool TopNonZero = false;
    for (int I = 4; I < 8; ++I) {
      TopNonZero |= Prod[I] != 0;
    }
    if (!TopNonZero) {
      break;
    }
    const U256 Top{Prod[4], Prod[5], Prod[6], Prod[7]};
    const U256 Low{Prod[0], Prod[1], Prod[2], Prod[3]};
    Limb T[8];
    mul256x256(Top, FoldC, T);
    uint64_t C = 0;
    for (int I = 0; I < 4; ++I) {
      const Wide X = (Wide)T[I] + Low.L[I] + C;
      T[I] = static_cast<Limb>(X);
      C = static_cast<Limb>(X >> 64);
    }
    int K = 4;
    while (C != 0) {
      const Wide X = (Wide)T[K] + C;
      T[K] = static_cast<Limb>(X);
      C = static_cast<Limb>(X >> 64);
      ++K;
    }
    std::memcpy(Prod, T, sizeof(T));
  }
  U256 R{Prod[0], Prod[1], Prod[2], Prod[3]};
  for (int I = 0; I < 3; ++I) {
    if (cmp(R, Mod) < 0) {
      break;
    }
    R = sub256(R, Mod);
  }
  return R;
}

// Base^Exp mod Mod (square-and-multiply, deterministic).
static U256 powMod(U256 Base, U256 Exp, const U256 &Mod, const U256 &FoldC) {
  U256 Res = kOne;
  for (int I = 0; I < 256; ++I) {
    if ((Exp.L[I >> 6] >> (I & 63)) & 1) {
      Res = mulMod(Res, Base, Mod, FoldC);
    }
    Base = mulMod(Base, Base, Mod, FoldC);
  }
  return Res;
}

// Jacobian-form curve point (y^2 = x^3 + 7).
struct JPoint {
  U256 X;
  U256 Y;
  U256 Z;
};

static JPoint jacDouble(const JPoint &P, const U256 &Mod, const U256 &FoldC) {
  if (isZero(P.Z) || isZero(P.Y)) {
    return {kZero, kZero, kZero};
  }
  const U256 A = mulMod(P.X, P.X, Mod, FoldC);
  const U256 B = mulMod(P.Y, P.Y, Mod, FoldC);
  const U256 C = mulMod(B, B, Mod, FoldC);
  const U256 XPB = addMod(P.X, B, Mod, FoldC);
  const U256 XPB2 = mulMod(XPB, XPB, Mod, FoldC);
  U256 D = subMod(XPB2, A, Mod);
  D = subMod(D, C, Mod);
  D = addMod(D, D, Mod, FoldC); // 2*((X+B)^2 - A - C)
  const U256 E = addMod(A, addMod(A, A, Mod, FoldC), Mod, FoldC); // 3*A
  const U256 F = mulMod(E, E, Mod, FoldC);
  U256 X3 = subMod(F, D, Mod);
  X3 = subMod(X3, D, Mod); // F - 2*D
  const U256 V = subMod(D, X3, Mod);
  const U256 EV = mulMod(E, V, Mod, FoldC);
  U256 C8 = addMod(C, C, Mod, FoldC);
  C8 = addMod(C8, C8, Mod, FoldC);
  C8 = addMod(C8, C8, Mod, FoldC); // 8*C
  const U256 Y3 = subMod(EV, C8, Mod);
  const U256 YZ = mulMod(P.Y, P.Z, Mod, FoldC);
  const U256 Z3 = addMod(YZ, YZ, Mod, FoldC); // 2*Y*Z
  return {X3, Y3, Z3};
}

static JPoint jacAdd(const JPoint &A, const JPoint &B, const U256 &Mod,
                     const U256 &FoldC) {
  if (isZero(A.Z)) {
    return B;
  }
  if (isZero(B.Z)) {
    return A;
  }
  const U256 Z1Z1 = mulMod(A.Z, A.Z, Mod, FoldC);
  const U256 Z2Z2 = mulMod(B.Z, B.Z, Mod, FoldC);
  const U256 U1 = mulMod(A.X, Z2Z2, Mod, FoldC);
  const U256 U2 = mulMod(B.X, Z1Z1, Mod, FoldC);
  const U256 S1 = mulMod(mulMod(A.Y, B.Z, Mod, FoldC), Z2Z2, Mod, FoldC);
  const U256 S2 = mulMod(mulMod(B.Y, A.Z, Mod, FoldC), Z1Z1, Mod, FoldC);
  if (equal(U1, U2)) {
    if (equal(S1, S2)) {
      return jacDouble(A, Mod, FoldC);
    }
    return {kZero, kZero, kZero}; // infinity
  }
  const U256 H = subMod(U2, U1, Mod);
  const U256 R = subMod(S2, S1, Mod);
  const U256 HH = mulMod(H, H, Mod, FoldC);
  const U256 HHH = mulMod(H, HH, Mod, FoldC);
  const U256 V = mulMod(U1, HH, Mod, FoldC);
  const U256 R2 = mulMod(R, R, Mod, FoldC);
  U256 X3 = subMod(R2, HHH, Mod);
  X3 = subMod(X3, V, Mod);
  X3 = subMod(X3, V, Mod); // R^2 - HHH - 2*V
  const U256 VX3 = subMod(V, X3, Mod);
  const U256 RVX3 = mulMod(R, VX3, Mod, FoldC);
  const U256 SHHH = mulMod(S1, HHH, Mod, FoldC);
  const U256 Y3 = subMod(RVX3, SHHH, Mod);
  const U256 Z3 = mulMod(mulMod(A.Z, B.Z, Mod, FoldC), H, Mod, FoldC);
  return {X3, Y3, Z3};
}

// Left-to-right double-and-add scalar multiplication.
static JPoint jacScalarMult(const JPoint &P, const U256 &K, const U256 &Mod,
                            const U256 &FoldC) {
  JPoint R{kZero, kZero, kZero};
  for (int I = 255; I >= 0; --I) {
    R = jacDouble(R, Mod, FoldC);
    if ((K.L[I >> 6] >> (I & 63)) & 1) {
      R = jacAdd(R, P, Mod, FoldC);
    }
  }
  return R;
}

// Convert a Jacobian point to affine coordinates; returns false for infinity.
static bool jacToAffine(const JPoint &P, U256 &X, U256 &Y, const U256 &Mod,
                        const U256 &FoldC) {
  if (isZero(P.Z)) {
    return false;
  }
  // Fermat inverse: z^(Mod-2).
  const U256 ZInv =
      powMod(P.Z, sub256(Mod, U256{2, 0, 0, 0}), Mod, FoldC);
  const U256 ZInv2 = mulMod(ZInv, ZInv, Mod, FoldC);
  const U256 ZInv3 = mulMod(ZInv2, ZInv, Mod, FoldC);
  X = mulMod(P.X, ZInv2, Mod, FoldC);
  Y = mulMod(P.Y, ZInv3, Mod, FoldC);
  return true;
}

static U256 beLoad(const uint8_t *In) {
  U256 R{};
  for (int I = 0; I < 32; ++I) {
    // Byte I (big-endian) belongs to little-endian limb 3 - (I / 8) at
    // bit offset 8*(7 - (I & 7)).
    R.L[3 - (I >> 3)] |= static_cast<Limb>(In[I]) << (8 * (7 - (I & 7)));
  }
  return R;
}

static void beStore(uint8_t *Out, const U256 &V) {
  for (int I = 0; I < 32; ++I) {
    Out[I] = static_cast<uint8_t>(V.L[3 - (I >> 3)] >> (8 * (7 - (I & 7))));
  }
}

} // namespace

bool recoverPublicKey(const uint8_t *Digest, const uint8_t *R, const uint8_t *S,
                      uint8_t YParity, uint8_t *OutPubKey) {
  if (YParity > 1) {
    return false;
  }
  const U256 Rv = beLoad(R);
  const U256 Sv = beLoad(S);
  // Ethereum signature validation: 0 < r < N and 0 < s <= N/2 (low-s).
  if (isZero(Rv) || cmp(Rv, kN) >= 0) {
    return false;
  }
  if (isZero(Sv) || cmp(Sv, kNHalf) > 0) {
    return false;
  }

  // Decompress the point R = (r, y) with the requested y-parity.
  // r < N < P, so x = r is a valid field element.
  const U256 R2 = mulMod(Rv, Rv, kP, kFoldP);
  const U256 R3 = mulMod(R2, Rv, kP, kFoldP);
  const U256 Y2 = addMod(R3, U256{7, 0, 0, 0}, kP, kFoldP);
  const U256 Y = powMod(Y2, kPPlus1Over4, kP, kFoldP);
  if (!equal(mulMod(Y, Y, kP, kFoldP), Y2)) {
    return false; // x^3 + 7 is not a quadratic residue
  }
  U256 Yv = Y;
  if ((Yv.L[0] & 1) != YParity) {
    Yv = sub256(kP, Yv);
  }
  const JPoint Rpt{Rv, Yv, kOne};

  // z = digest mod N
  U256 Z = beLoad(Digest);
  if (cmp(Z, kN) >= 0) {
    Z = sub256(Z, kN);
  }

  // Q = u1*G + u2*R with u1 = -z*r^-1, u2 = s*r^-1 (mod N).
  const U256 RInv = powMod(Rv, kNMinus2, kN, kFoldN);
  const U256 NegZ = isZero(Z) ? kZero : sub256(kN, Z);
  const U256 U1 = mulMod(NegZ, RInv, kN, kFoldN);
  const U256 U2 = mulMod(Sv, RInv, kN, kFoldN);

  const JPoint G{kGx, kGy, kOne};
  const JPoint Q =
      jacAdd(jacScalarMult(G, U1, kP, kFoldP),
             jacScalarMult(Rpt, U2, kP, kFoldP), kP, kFoldP);
  U256 QX;
  U256 QY;
  if (!jacToAffine(Q, QX, QY, kP, kFoldP)) {
    return false;
  }
  beStore(OutPubKey, QX);
  beStore(OutPubKey + 32, QY);
  return true;
}

} // namespace zen::host::evm::secp256k1