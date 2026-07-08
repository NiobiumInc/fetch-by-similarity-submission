// client_encode_encrypt_db.cpp - encrypting the dataset
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
void add_markers(std::vector<std::vector<int16_t>>& payloads);

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

  // Read the dataset matrix from storage
  auto db = read2vecs<float>(prms.datadir()/"db.bin", prms.getRecordDim());
  assert(int(db.size())==prms.getDbSize());

  // transpose the matrix, so it is in column-major order
  auto encoded_dataset = transpose_matrix<float>(db, prms.getNSlots());
  assert(int(encoded_dataset.size())==prms.getNCtxts());

  // Read and transpose the payloads from disk (PAYLOAD_DIM=8)
  auto payload_fname = prms.datadir()/"payloads.bin";
  std::vector<std::vector<int16_t>> payloads =
        read2vecs<int16_t>(payload_fname, PAYLOAD_DIM-1);
  assert(db.size() == payloads.size());

  // Add a marker at the beginning of each payload record, with value
  // equals to 2*MAX_PAYLOAD_VAL*PAYLOAD_PRECISION
  add_markers(payloads);

  // Encode the payloads in slots in column-major order
  auto encoded_payloads=transpose_matrix<int16_t>(payloads, prms.getNSlots());

  // scale payloads down by PAYLOAD_PRECISION
  for (auto& mat: encoded_payloads) for (auto& v: mat) for (auto& x: v) {
    x /= PAYLOAD_PRECISION;
  }

  // encrypt the batch-matrices and store to disk

  // The matrix rows will be multiplied by replicated cipehrtexts at level
  // at least degrees.size()-1, so encrypt them at that level to save space
  int encryption_level1 = prms.getDegrees().size() - 1;

  // encrypt the batch-payload and store to disk at a low level.
  int encryption_level2 = 20;

  auto cc = pk->GetCryptoContext();
  for (int i = 0; i < prms.getNCtxts(); i++) {  // go over the batches
    std::stringstream ssi;
    ssi << std::setw(4) << std::setfill('0') << i;
    auto dir = prms.updir() / ("batch" + ssi.str());
    // Create the batch directory and any parent directory as needed
    std::filesystem::create_directories(dir);

    // encrypt vectors in this batch
    for (auto j = 0; j < prms.getRecordDim(); j++) {
      auto pt = cc->MakeCKKSPackedPlaintext(encoded_dataset[i][j], 1,
                                            encryption_level1);
      auto ct = cc->Encrypt(pk, pt);
      std::stringstream ssj;
      ssj << std::setw(4) << std::setfill('0') << j;
      auto ct_fname = dir / ("row_" + ssj.str() + ".bin");
      if (!Serial::SerializeToFile(ct_fname, ct, SerType::BINARY)) {
        throw std::runtime_error("failed to write file " + ct_fname.string());
      }
    }
    // encrypt payloads in this batch
    for (size_t j = 0; j < PAYLOAD_DIM; j++) {
      auto pt = cc->MakeCKKSPackedPlaintext(encoded_payloads[i][j], 1,
                                            encryption_level2);
      auto ct = cc->Encrypt(pk, pt);
      std::stringstream ssj;
      ssj << std::setw(4) << std::setfill('0') << j;
      auto ct_fname = dir / ("payload_" + ssj.str() + ".bin");
      if (!Serial::SerializeToFile(ct_fname, ct, SerType::BINARY)) {
        throw std::runtime_error("failed to write file " + ct_fname.string());
      }
    }
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

// Add a marker at the beginning of each payload record, with value
// equals to 2*MAX_PAYLOAD_VAL*PAYLOAD_PRECISION
void add_markers(std::vector<std::vector<int16_t>>& payloads)
{
    for (auto& p: payloads) {
        p.insert(p.begin(), 2*MAX_PAYLOAD_VAL*PAYLOAD_PRECISION);
    }
}