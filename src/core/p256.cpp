#include "bh61/core/p256.hpp"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

namespace bh61::core {
namespace {

template <typename T, void (*Free)(T*)>
using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

using GroupPtr = OpenSslPtr<EC_GROUP, EC_GROUP_free>;
using PointPtr = OpenSslPtr<EC_POINT, EC_POINT_free>;
using BnPtr = OpenSslPtr<BIGNUM, BN_free>;
using ClearBnPtr = OpenSslPtr<BIGNUM, BN_clear_free>;
using BnCtxPtr = OpenSslPtr<BN_CTX, BN_CTX_free>;

auto group() -> GroupPtr {
  GroupPtr value(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1),
                 EC_GROUP_free);
  if (!value) {
    throw std::runtime_error("OpenSSL could not create the P-256 group");
  }
  return value;
}

auto context() -> BnCtxPtr {
  BnCtxPtr value(BN_CTX_new(), BN_CTX_free);
  if (!value) {
    throw std::runtime_error("OpenSSL could not create a BIGNUM context");
  }
  return value;
}

auto validated_private(const EC_GROUP* curve,
                       std::span<const std::uint8_t, 32> bytes) -> BnPtr {
  BnPtr scalar(BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr),
               BN_free);
  BnPtr order(BN_new(), BN_free);
  if (!scalar || !order || EC_GROUP_get_order(curve, order.get(), nullptr) != 1) {
    throw std::runtime_error("OpenSSL could not decode the P-256 scalar");
  }
  if (BN_is_zero(scalar.get()) == 1 || BN_is_negative(scalar.get()) == 1 ||
      BN_cmp(scalar.get(), order.get()) >= 0) {
    throw std::invalid_argument("P-256 private scalar is outside 1..n-1");
  }
  return scalar;
}

auto serialized_xy(const EC_GROUP* curve, const EC_POINT* point,
                   BN_CTX* bn_context) -> std::array<std::uint8_t, 64> {
  std::array<std::uint8_t, 65> encoded{};
  const auto count = EC_POINT_point2oct(curve, point,
                                        POINT_CONVERSION_UNCOMPRESSED,
                                        encoded.data(), encoded.size(),
                                        bn_context);
  if (count != encoded.size() || encoded[0] != 0x04U) {
    throw std::runtime_error("OpenSSL could not serialize the P-256 point");
  }
  std::array<std::uint8_t, 64> result{};
  std::copy(encoded.begin() + 1, encoded.end(), result.begin());
  return result;
}

auto validated_public(const EC_GROUP* curve,
                      std::span<const std::uint8_t, 64> xy,
                      BN_CTX* bn_context) -> PointPtr {
  std::array<std::uint8_t, 65> encoded{};
  encoded[0] = 0x04U;
  std::copy(xy.begin(), xy.end(), encoded.begin() + 1);
  PointPtr point(EC_POINT_new(curve), EC_POINT_free);
  if (!point || EC_POINT_oct2point(curve, point.get(), encoded.data(),
                                   encoded.size(), bn_context) != 1 ||
      EC_POINT_is_at_infinity(curve, point.get()) == 1 ||
      EC_POINT_is_on_curve(curve, point.get(), bn_context) != 1) {
    throw std::invalid_argument("P-256 public point is malformed or off-curve");
  }
  return point;
}

}  // namespace

auto p256_public_from_private(std::span<const std::uint8_t, 32> private_scalar)
    -> std::array<std::uint8_t, 64> {
  auto curve = group();
  auto bn_context = context();
  auto scalar = validated_private(curve.get(), private_scalar);
  PointPtr point(EC_POINT_new(curve.get()), EC_POINT_free);
  if (!point || EC_POINT_mul(curve.get(), point.get(), scalar.get(), nullptr,
                             nullptr, bn_context.get()) != 1) {
    throw std::runtime_error("OpenSSL could not calculate the P-256 public key");
  }
  return serialized_xy(curve.get(), point.get(), bn_context.get());
}

auto generate_p256_keypair() -> P256KeyPair {
  auto curve = group();
  auto bn_context = context();
  BnPtr order(BN_new(), BN_free);
  ClearBnPtr scalar(BN_secure_new(), BN_clear_free);
  if (!order || !scalar ||
      EC_GROUP_get_order(curve.get(), order.get(), bn_context.get()) != 1) {
    throw std::runtime_error("OpenSSL could not initialize P-256 key generation");
  }
  do {
    if (BN_priv_rand_range(scalar.get(), order.get()) != 1) {
      throw std::runtime_error("OpenSSL P-256 private-key generation failed");
    }
  } while (BN_is_zero(scalar.get()) == 1);
  P256KeyPair result{};
  if (BN_bn2binpad(scalar.get(), result.private_scalar.data(),
                   static_cast<int>(result.private_scalar.size())) !=
      static_cast<int>(result.private_scalar.size())) {
    throw std::runtime_error("OpenSSL could not serialize the P-256 scalar");
  }
  result.public_xy = p256_public_from_private(result.private_scalar);
  return result;
}

auto p256_shared_x(std::span<const std::uint8_t, 32> private_scalar,
                   std::span<const std::uint8_t, 64> remote_public_xy)
    -> std::array<std::uint8_t, 32> {
  auto curve = group();
  auto bn_context = context();
  auto scalar = validated_private(curve.get(), private_scalar);
  auto remote = validated_public(curve.get(), remote_public_xy, bn_context.get());
  PointPtr shared(EC_POINT_new(curve.get()), EC_POINT_free);
  ClearBnPtr x(BN_secure_new(), BN_clear_free);
  if (!shared || !x ||
      EC_POINT_mul(curve.get(), shared.get(), nullptr, remote.get(), scalar.get(),
                   bn_context.get()) != 1 ||
      EC_POINT_is_at_infinity(curve.get(), shared.get()) == 1 ||
      EC_POINT_get_affine_coordinates(curve.get(), shared.get(), x.get(), nullptr,
                                      bn_context.get()) != 1) {
    throw std::runtime_error("OpenSSL P-256 ECDH failed");
  }
  std::array<std::uint8_t, 32> result{};
  if (BN_bn2binpad(x.get(), result.data(), static_cast<int>(result.size())) !=
      static_cast<int>(result.size())) {
    throw std::runtime_error("OpenSSL could not serialize the ECDH X coordinate");
  }
  return result;
}

auto derive_peer_key(std::span<const std::uint8_t, 32> shared_x) -> Aes128Key {
  std::array<std::uint8_t, 32> digest{};
  unsigned int digest_length = 0;
  if (EVP_Digest(shared_x.data(), shared_x.size(), digest.data(), &digest_length,
                 EVP_sha256(), nullptr) != 1 || digest_length != digest.size()) {
    throw std::runtime_error("OpenSSL SHA-256 KDF failed");
  }
  Aes128Key result{};
  std::copy_n(digest.begin(), result.size(), result.begin());
  OPENSSL_cleanse(digest.data(), digest.size());
  return result;
}

}  // namespace bh61::core
