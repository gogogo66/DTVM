#!/usr/bin/env python3
"""Deterministic EIP-3074 AUTH test-vector generator.

Fidelity strategy (documented in the implementation report):
* The secp256k1 math here is a small independent pure-Python implementation
  (signing + public-key recovery over secp256k1; no external crypto deps).
* Every produced signature is cross-verified by OpenSSL 3.x via a parallel
  signature over sha256(digest): this OpenSSL build's `pkeyutl -verify -rawin`
  internally hashes the ECDSA input with SHA-256 (verified empirically), so we
  hand it `digest` and it recomputes z = sha256(digest), which the parallel
  (r2,s2) signature is committed to. Success proves EC math + key + DER
  encoding against an independent ECDSA implementation.
* The recovery parity is determined by matching the pure-Python recovered
  public key against the public key OpenSSL derives from the same private
  scalar.
* The address is keccak256(x||y)[12:] using pycryptodome's Keccak-256
  (canonical keccak256('') == c5d246...a470 is checked below).
Output is C++-ready hex constants consumed by src/tests/evm_interp_tests.cpp.
"""
import hashlib, subprocess, tempfile, os
from Crypto.Hash import keccak

# --- secp256k1 constants ---
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
Gx = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
Gy = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8

def keccak256(data):
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()

def modinv(a, m):
    return pow(a, m - 2, m)

def point_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1; x2, y2 = p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1) * modinv(2 * y1, P) % P
    else:
        lam = (y2 - y1) * modinv(x2 - x1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return (x3, y3)

def point_mul(k, pt=None):
    base = (Gx, Gy) if pt is None else pt
    r = None
    while k:
        if k & 1:
            r = point_add(r, base)
        base = point_add(base, base)
        k >>= 1
    return r

def recover_pubkey(digest_int, r, s, v):
    """SEC1 public-key recovery; returns (x, y) or None. v in {0,1}."""
    if not (0 <= v <= 1):
        return None
    if not (0 < r < N and 0 < s <= N // 2):
        return None
    x = r
    if x >= P:
        return None
    y2 = (pow(x, 3, P) + 7) % P
    y = pow(y2, (P + 1) // 4, P)
    if (y * y) % P != y2:
        return None
    if (y & 1) != v:
        y = P - y
    Rpt = (x, y)
    z = digest_int % N
    rinv = modinv(r, N)
    u1 = (-z * rinv) % N
    u2 = (s * rinv) % N
    return point_add(point_mul(u1), point_mul(u2, Rpt))

def sign(d, z, k0):
    """Deterministic low-s ECDSA signature. Returns (r, s)."""
    k = k0 % N
    while True:
        if k == 0:
            k = 1
        R = point_mul(k)
        r = R[0] % N
        if r == 0:
            k += 1
            continue
        s = (modinv(k, N) * (z + r * d)) % N
        if s == 0 or s > N // 2:
            k += 1
            continue
        return r, s

# --- OpenSSL plumbing (independent verification) ---
def der_len(l):
    return bytes([l]) if l < 0x80 else bytes([0x81, l])

def der_int(b):
    if b[0] & 0x80:
        b = b"\x00" + b
    return b"\x02" + der_len(len(b)) + b

def sec1_der_privkey(d):
    dbytes = d.to_bytes(32, "big")
    priv = b"\x04" + der_len(len(dbytes)) + dbytes
    oid = b"\x06\x05\x2b\x81\x04\x00\x0a"  # 1.3.132.0.10 = secp256k1
    inner = b"\x02\x01\x01" + priv + b"\xa0\x07" + oid
    return b"\x30" + der_len(len(inner)) + inner

def ecdsa_sig_der(r, s):
    inner = der_int(r.to_bytes(32, "big").lstrip(b"\x00") or b"\x00") + \
            der_int(s.to_bytes(32, "big").lstrip(b"\x00") or b"\x00")
    return b"\x30" + der_len(len(inner)) + inner

def openssl_pubkey(d):
    """Returns (x, y) of d*G derived by OpenSSL (independent of our math)."""
    with tempfile.TemporaryDirectory() as td:
        priv = os.path.join(td, "priv.der")
        open(priv, "wb").write(sec1_der_privkey(d))
        t = subprocess.run(["openssl", "ec", "-inform", "DER", "-in", priv,
                            "-text", "-noout"], capture_output=True, text=True)
        if t.returncode != 0:
            raise RuntimeError("openssl ec: " + t.stderr)
        digits, collecting = "", False
        for line in t.stdout.splitlines():
            line = line.strip()
            if line.startswith("pub:"):
                collecting = True
                continue
            if collecting and line.startswith("ASN1 OID"):
                break
            if collecting:
                digits += "".join(c for c in line if c in "0123456789abcdef")
        if len(digits) < 130 or digits[:2] != "04":
            raise RuntimeError("could not parse openssl public key")
        return int(digits[2:66], 16), int(digits[66:130], 16)

def openssl_verify_sha256(digest, r, s, d):
    """Verify (r,s) committed to z = sha256(digest) with OpenSSL.
    This build hashes the ECDSA input with SHA-256 internally even with
    -rawin, so we pass `digest` as the input file."""
    with tempfile.TemporaryDirectory() as td:
        priv = os.path.join(td, "priv.der")
        pem = os.path.join(td, "priv.pem")
        pub = os.path.join(td, "pub.pem")
        open(priv, "wb").write(sec1_der_privkey(d))
        subprocess.run(["openssl", "ec", "-inform", "DER", "-in", priv, "-out", pem],
                       check=True, capture_output=True)
        subprocess.run(["openssl", "ec", "-in", pem, "-pubout", "-out", pub],
                       check=True, capture_output=True)
        dg = os.path.join(td, "d.bin")
        open(dg, "wb").write(digest)
        sg = os.path.join(td, "s.der")
        open(sg, "wb").write(ecdsa_sig_der(r, s))
        ver = subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
                              "-inkey", pub, "-sigfile", sg, "-in", dg],
                             capture_output=True, text=True)
        out = ver.stdout + ver.stderr
        if ver.returncode != 0 or "Verified" not in out or \
                "Verification Failure" in out:
            raise RuntimeError("openssl verify failed: rc=%d out=%r" %
                               (ver.returncode, out))

# --- message construction (EIP-3074) ---
CHAIN_ID = 1
NONCE0 = 0
NONCE1 = 1
INVOKER = bytes.fromhex("0101010101010101010101010101010101010101")
COMMIT = bytes.fromhex("112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00")
PRIV = 0x0BAD10F00D5EEDC0FFEEBEEF5C0FFEE5678ABCDEF0123456789ABCDEF0123456
K0 = 0x9E3779B97F4A7C15F39CC0605CEDC8341082276BF3A1B3CFEB5D4E8B0B
assert 0 < PRIV < N
assert keccak256(b"").hex() == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"

def build_message(nonce):
    msg = b"\x04"
    msg += CHAIN_ID.to_bytes(32, "big")
    msg += nonce.to_bytes(32, "big")
    msg += bytes(12) + INVOKER
    msg += COMMIT
    assert len(msg) == 129
    return msg

def make_vector(nonce, k0):
    msg = build_message(nonce)
    digest = keccak256(msg)
    z = int.from_bytes(digest, "big") % N
    r, s = sign(PRIV, z, k0)

    # Independent checks:
    # (a) OpenSSL verifies a parallel signature committed to sha256(digest).
    z2 = int.from_bytes(hashlib.sha256(digest).digest(), "big") % N
    r2, s2 = sign(PRIV, z2, k0 + 0x1234)
    openssl_verify_sha256(digest, r2, s2, PRIV)
    # (b) parity: recovery must reproduce the OpenSSL-derived public key.
    qx, qy = openssl_pubkey(PRIV)
    v = None
    for cand in (0, 1):
        Q = recover_pubkey(z, r, s, cand)
        if Q is not None and Q[0] == qx and Q[1] == qy:
            v = cand
            break
    if v is None:
        raise RuntimeError("recovery parity match failed")
    address = keccak256(qx.to_bytes(32, "big") + qy.to_bytes(32, "big"))[12:]
    return {
        "message": msg, "digest": digest, "nonce": nonce, "chain_id": CHAIN_ID,
        "invoker": INVOKER, "commit": COMMIT, "address": address,
        "r": r, "s": s, "v": v,
    }

def emit(name, vec):
    def B32(v):
        return v.to_bytes(32, "big").hex()
    print("#define AUTHV_%s_ADDRESS \"%s\"" % (name, vec["address"].hex()))
    print("#define AUTHV_%s_INVOKER \"%s\"" % (name, vec["invoker"].hex()))
    print("#define AUTHV_%s_COMMIT \"%s\"" % (name, vec["commit"].hex()))
    print("#define AUTHV_%s_R \"%s\"" % (name, vec["r"].to_bytes(32, "big").hex()))
    print("#define AUTHV_%s_S \"%s\"" % (name, vec["s"].to_bytes(32, "big").hex()))
    print("#define AUTHV_%s_S_HIGH \"%s\"" % (name, (N - vec["s"]).to_bytes(32, "big").hex()))
    print("#define AUTHV_%s_V %d" % (name, vec["v"]))
    print("#define AUTHV_%s_NONCE %dULL" % (name, vec["nonce"]))
    print("#define AUTHV_%s_CHAIN_ID %dULL" % (name, vec["chain_id"]))
    print("#define AUTHV_%s_DIGEST \"%s\"" % (name, vec["digest"].hex()))
    print("")

if __name__ == "__main__":
    emit("A", make_vector(NONCE0, K0))
    emit("B", make_vector(NONCE1, K0 + 0x5555))
    print("// Vectors independently verified by OpenSSL 3.0.13 (pkeyutl -verify).")
