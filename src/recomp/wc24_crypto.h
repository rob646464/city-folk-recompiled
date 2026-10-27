#pragma once
// The cryptography WiiConnect24's download daemon (KD, inside IOS) applies to a downloaded
// file before it lands in the title's wc24dl.vff (docs/DISTRIBUTIONS.md "The WC24 container"):
// SHA-1, AES-128 in OFB mode and an RSA-2048 PKCS#1 v1.5 signature check with exponent 65537.
// Written from the public specifications (FIPS 180-4, FIPS 197, SP 800-38A, RFC 8017); no
// third-party code. Self-test: wc24crypto::selfTest() (known-answer vectors).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wc24crypto {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

void sha1(const void* data, size_t len, u8 out[20]);

// AES-128-OFB: XORs data with the key stream (encryption and decryption are the same).
void aes128Ofb(const u8 key[16], const u8 iv[16], u8* data, size_t len);

// RSA-2048 signature check: sig^65537 mod n must be the PKCS#1 v1.5 encoding of SHA-1(msg)
// (00 01 FF..FF 00 + DigestInfo(SHA-1) + hash). modulus and sig are 256 bytes, big-endian.
bool rsa2048VerifySha1(const u8 modulus[256], const u8 sig[256], const void* msg, size_t len);

bool selfTest(std::string* failure = nullptr);

}  // namespace wc24crypto
