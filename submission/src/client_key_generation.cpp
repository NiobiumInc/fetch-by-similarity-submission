// client_key_generation.cpp - Client key generation
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

#include "utils.h"
#include "params.h"
#include "running_sums.h"
#include "slot_replication.h"

using namespace lbcrypto;

KeyPair<DCRTPoly> key_gen(const InstanceParams& prms, bool count_only);

int main(int argc, char* argv[]) {
  if (argc < 2 || !std::isdigit(argv[1][0])) {
    std::cout << "Usage: " << argv[0] << " instance-size [--count_only]\n";
    std::cout << "  Instance-size: 0-TOY, 1-SMALL, 2-MEDIUM, 3-LARGE\n";
    return 0;
  }
  auto size = static_cast<InstanceSize>(std::stoi(argv[1]));
  InstanceParams prms(size);

  bool count_only = (argc > 2 && std::string(argv[2])=="--count_only");

  // Generate fresh keys
  auto keys = key_gen(prms, count_only);
  auto cc = keys.publicKey->GetCryptoContext();

  // Store context and keys to disk
  std::filesystem::create_directory(prms.keydir(), prms.rtdir());
  if (!Serial::SerializeToFile(prms.keydir()/"cc.bin", cc, SerType::BINARY) ||
      !Serial::SerializeToFile(prms.keydir()/"pk.bin",
                               keys.publicKey, SerType::BINARY) ||
      !Serial::SerializeToFile(prms.keydir()/"sk.bin",
                               keys.secretKey, SerType::BINARY)) {
    throw std::runtime_error("Failed to write keys to "+prms.keydir().string());
  }
  std::ofstream emult_file(prms.keydir()/"mk.bin",
                           std::ios::out | std::ios::binary);
  std::ofstream erot_file(prms.keydir()/"rk.bin",
                          std::ios::out | std::ios::binary);
  if (!emult_file.is_open() || !erot_file.is_open() ||
      !cc->SerializeEvalMultKey(emult_file, SerType::BINARY) ||
      !cc->SerializeEvalAutomorphismKey(erot_file, SerType::BINARY)) {
    throw std::runtime_error(
        "Failed to write eval keys to "+prms.keydir().string());
  }
  return 0;
}

// Generate keys that include all the rotations needed for replication,
// running sums, and total sums
KeyPair<DCRTPoly> key_gen(const InstanceParams& prms, bool count_only)
{
  CCParams<CryptoContextCKKSRNS> cParams;
  cParams.SetSecretKeyDist(UNIFORM_TERNARY);
  cParams.SetKeySwitchTechnique(HYBRID);
  cParams.SetMultiplicativeDepth(23);
  cParams.SetSecurityLevel(HEStd_128_classic);
  cParams.SetScalingTechnique(FLEXIBLEAUTO);
  cParams.SetScalingModSize(42);
  cParams.SetFirstModSize(57);
  CryptoContext<DCRTPoly> cc = GenCryptoContext(cParams);

  // Enable features that you wish to use
  cc->Enable(PKE);
  cc->Enable(KEYSWITCH);
  cc->Enable(LEVELEDSHE);
  cc->Enable(ADVANCEDSHE);
  assert(unsigned(prms.getRingDim())==cc->GetRingDimension());

  auto keyPair = cc->KeyGen();            // secret/public keys
  cc->EvalMultKeyGen(keyPair.secretKey);  // re-linearization key

  // Calculate the rotation amounts needed
  auto rots4reps = DFSSlotReplicator::get_rotation_amounts(prms.getDegrees());
  if (count_only) {
    cc->EvalAtIndexKeyGen(keyPair.secretKey, rots4reps);
    cc->EvalSumKeyGen(keyPair.secretKey);
  } else {
    std::vector<int> shifts(PAYLOAD_DIM - 1); // Generate summation keys
    for (int i = 1; i < PAYLOAD_DIM; i++) {
      shifts[i - 1] = -i * prms.getNCols();
    }
    auto shifts2 = RunningSums::get_shift_amounts(
      prms.getNSlots(), prms.getNCols(), RUNNING_SUM_LEVELS);
    std::vector<std::vector<int>> all_shifts = {rots4reps, shifts, shifts2};
    cc->EvalAtIndexKeyGen(keyPair.secretKey, vector_union(all_shifts));
    cc->EvalSumRowsKeyGen(keyPair.secretKey, keyPair.publicKey,
                          prms.getNCols() * PAYLOAD_DIM);
  }
  return keyPair;
}
