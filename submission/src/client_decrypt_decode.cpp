// client_decrypt_decode.cpp - decrypting answer from server
//============================================================================
// Copyright (c) 2025, Amazon Web Services
// All rights reserved.
//
// This software is licensed under the terms of the Apache License v2.
// See the file LICENSE.md for details.
//============================================================================
#include <cassert>

#include "openfhe.h"
// header files needed for de/serialization
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include "params.h"
#include "utils.h"

using namespace lbcrypto;

// Read public encryption key from disk
PrivateKey<DCRTPoly> read_key(InstanceParams prms);

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " instance-size\n";
    std::cout << "  Instance-size: 0-TOY, 1-SMALL, 2-MEDIUM, 3-LARGE\n";
    return 0;
  }
  auto size = static_cast<InstanceSize>(std::stoi(argv[1]));
  InstanceParams prms(size, ring_dim_from_args(argc, argv));

  // Read the encrypted answer from disk
  Ciphertext<DCRTPoly> eres;
  auto res_file = prms.downdir()/"results.bin";
  if (!Serial::DeserializeFromFile(res_file, eres, SerType::BINARY)) {
    throw std::runtime_error("failed to read answer from "+res_file.string());
  }

  // Read the secret keys from disk and decrypt
  Plaintext pt;
  auto sk = read_key(prms);
  sk->GetCryptoContext()->Decrypt(sk, eres, &pt);  // Decrypt
  auto slots = pt->GetRealPackedValue();           // Decode to slots

  write2disk<double>(prms.iodir()/"raw-result.bin",{slots});  // write to disk
  return 0;
}

// Read public encryption key from disk
PrivateKey<DCRTPoly> read_key(InstanceParams prms)
{
  CryptoContext<DCRTPoly> cc;
  if (!Serial::DeserializeFromFile(prms.keydir()/"cc.bin",cc,SerType::BINARY)){
    throw std::runtime_error(
        "Failed to get CryptoContext from " + prms.keydir().string());
  }
  PrivateKey<DCRTPoly> sk;
  if (!Serial::DeserializeFromFile(prms.keydir()/"sk.bin",sk,SerType::BINARY)){
    throw std::runtime_error(
        "Failed to get secret key from " + prms.keydir().string());
  }
  return sk;
}