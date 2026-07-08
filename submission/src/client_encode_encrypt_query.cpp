// client_encode_encrypt_query.cpp - encrypting the query
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
PublicKey<DCRTPoly> read_keys(InstanceParams prms);

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " instance-size\n";
    std::cout << "  Instance-size: 0-TOY, 1-SMALL, 2-MEDIUM, 3-LARGE\n";
    return 0;
  }
  auto size = static_cast<InstanceSize>(std::stoi(argv[1]));
  InstanceParams prms(size, ring_dim_from_args(argc, argv));

  // Read the keys from storage
  auto pk = read_keys(prms);
  auto cc = pk->GetCryptoContext();

  // Read the query vector from disk
  auto qs = read2vecs<float>(prms.datadir()/"query.bin", prms.getRecordDim());
  assert(qs.size()==1);
  auto qry = qs[0];

  // Encrypt the query vector, repeated to fill all the slots in a ciphertext
  std::vector<double> slots(prms.getNSlots());
  for (int i = 0; i < prms.getNSlots(); i++) {
    slots[i] = qry[i % prms.getRecordDim()];
  }
  auto pt = cc->MakeCKKSPackedPlaintext(slots);
  auto eqry = cc->Encrypt(pk, pt);  // the encrypted query vector at top level
  auto q_file = prms.updir()/"query.bin";
  if (!Serial::SerializeToFile(q_file, eqry, SerType::BINARY)) {
      throw std::runtime_error("failed to write query to "+q_file.string());
  }
  return 0;
}

// Read public encryption key from disk
PublicKey<DCRTPoly> read_keys(InstanceParams prms)
{
  CryptoContext<DCRTPoly> cc;
  if (!Serial::DeserializeFromFile(prms.keydir()/"cc.bin",cc,SerType::BINARY)){
    throw std::runtime_error(
        "Failed to get CryptoContext from " + prms.keydir().string());
  }
  PublicKey<DCRTPoly> pk;
  if (!Serial::DeserializeFromFile(prms.keydir()/"pk.bin",pk,SerType::BINARY)){
    throw std::runtime_error(
        "Failed to get public key from " + prms.keydir().string());
  }
  return pk;
}