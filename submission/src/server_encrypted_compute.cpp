// server_encrypted_computation.cpp - encrypted fetch-by-similarity
//============================================================================
// Copyright (c) 2025, Amazon Web Services
// All rights reserved.
//
// This software is licensed under the terms of the Apache License v2.
// See the file LICENSE.md for details.
//============================================================================
//
// Niobium integration (see ../../NIOBIUM_INTEGRATION.md for the overview).
//
// This is the one workload file with Niobium integration code. The FHE
// computation below is plain OpenFHE and is unchanged; everything Niobium-
// specific is guarded by `#ifdef NIOBIUM_COMPILER` so the file still builds and
// runs as a normal OpenFHE program without it.
//
// When built with NIOBIUM_COMPILER, main() drives Niobium's record/replay
// lifecycle: it initializes the compiler, enables cooperative auto-tagging (so
// the crypto context, keys, and input ciphertexts are captured automatically as
// they are deserialized), and gates only the RECORDING on is_cache_valid():
//   - cache miss (no trace): RECORD the trace in hollow mode (the FHE math is
//     skipped for speed/memory, so the value left in the output is NOT valid),
//     then finalize the trace;
//   - cache hit (trace exists): skip recording entirely (run NO FHE ops).
// In BOTH cases it then REPLAYS the trace on the --target backend (or the local
// simulator) and reconstructs the output — replay is the only path that yields
// a correct result, since hollow recording does no real math. The result
// ciphertext is then serialized the same way. Search for "NIOBIUM_COMPILER"
// below to see each step in context.
//============================================================================
#include <cassert>

#include "openfhe.h"
#include "cryptocontext-ser.h"  // header files needed for (de)serialization
#include "ciphertext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include "params.h"
#include "utils.h"
#include "slot_replication.h"
#include "running_sums.h"

#ifdef NIOBIUM_COMPILER
#include "niobium/compiler.h"
#endif

using namespace lbcrypto;

#undef DEBUG
#ifdef DEBUG
PrivateKey<DCRTPoly> sk;
#endif

// A utility function to get one encrypted ciphertext from the dataset. This
// implementation assumes that ciphertexts are just separate files on disk,
// it should be re-written if they are streamed from a remote location.
inline Ciphertext<DCRTPoly> get_ctxt(fs::path ct_name) {
  Ciphertext<DCRTPoly> ct;
  if (!Serial::DeserializeFromFile(ct_name, ct, SerType::BINARY)) {
    throw std::runtime_error("failed to read ciphertext from " + ct_name.string());
  }
  return ct;
}

// Print logging information to stdout
void log_step(int num, std::string name) {
  auto [timestamp, duration] = getCurrentTimeFormatted();
  std::cout << timestamp << " [server] " << num <<": "<< name << " completed";
  if (duration > 0) {
    std::cout << " (elapsed "<<duration<<"s)";
  }
  std::cout << std::endl;
}

// Matrix-vector product: The matrix rows are stored on disk in batches
// under iodir/<size>/encrypted/batchNNNN/. The query ciphertext contains
// the query vector, repeatd to fill in all the slots.
std::vector<Ciphertext<DCRTPoly>> mat_vec_mult(fs::path encdir,
                Ciphertext<DCRTPoly> qry, const InstanceParams& prms);

// Compare each slot in the ctxts to the threshold, using a Chebyshev
// approximation of the indicator function chi(x) = (x >= threshold).
// Rather than approximating 0/1 outcome, we scale it to 0/0.5, since we
// will sum up upto eight matches, then multiply by the original thing,
// and need to fit the result to a size-2 interval that can be shifted
// to [+-1].
void compare_to_threshold(std::vector<Ciphertext<DCRTPoly>>& ctxts,
                          double threshold, bool count_only);

// Compare each slot in the ciphertexts to the number, using a Chebyshev
// approximation of the function chi(x) = (x == number).
std::vector<Ciphertext<DCRTPoly>> compare_to_number(
    const std::vector<Ciphertext<DCRTPoly>>& ctxts, double number);

// Read from disk the ith payload value of all the records, namely the
// i'th row of the payload matrix.
Ciphertext<DCRTPoly> get_encrypted_payload(fs::path datadir, size_t batch,
                                            size_t idx);

// A SIMD-optimized procedure for computing total sums. The slots are viewed
// as a matrix, and total sums are computed in each column separately.
// All the entries of an output column contain the total sum of entries from
// that column in the input.
Ciphertext<DCRTPoly> total_sums(
  const Ciphertext<DCRTPoly>& ct, const InstanceParams& prms);

#ifdef DEBUG
static void printCts(
  const std::vector<Ciphertext<DCRTPoly>>& cts, std::string label)
{
  std::cout << label << "[";
  for (auto& ct: cts) {
    Plaintext pt;
    sk->GetCryptoContext()->Decrypt(sk, ct, &pt);
    std::vector<double> slots = pt->GetRealPackedValue();
    std::cout << label << " [";
    for (auto x: slots) {
      if (std::abs(x) < 0.1) { std::cout << "0 "; }
      else { printf("%.1f ", x); }
    }
    std::cout << ']' << std::endl;
  }
}
#endif
/*******************************************************************/
int main(int argc, char* argv[]) {
  if (argc < 2 || !std::isdigit(argv[1][0])) {
    std::cout << "Usage: " << argv[0] << " instance-size [--count_only]\n";
    std::cout << "  Instance-size: 0-TOY, 1-SMALL, 2-MEDIUM, 3-LARGE\n";
    return 0;
  }
  auto size = static_cast<InstanceSize>(std::stoi(argv[1]));
  bool count_only = (argc > 2 && std::string(argv[2])=="--count_only");

  InstanceParams prms(size);
  constexpr double threshold = 0.8;
  auto timing_fname = prms.iodir()/"server_reported_steps.json";
  auto start_server = std::chrono::system_clock::now();
  std::filesystem::create_directories(prms.downdir());

#ifdef NIOBIUM_COMPILER
  // Explicit cooperative record/replay lifecycle. init() consumes
  // any niobium flags (e.g. --target); enable_auto_tagging() opts into
  // Mode::COOPERATIVE so the auto-facade tags inputs/keys (with their source
  // paths) via the OpenFHE deserialize hooks, while THIS program owns the
  // record/replay decisions. Must run before the crypto context is loaded.
  niobium::compiler().init(argc, argv);
  niobium::compiler().enable_auto_tagging();
  // Stream tagged input/key data to disk at tag time instead of buffering all
  // captured input values in RAM until stop(). Safe here: replay happens in a
  // separate process (cooperative/transport) that reads the .bin from disk.
  niobium::compiler().enable_input_streaming();
  {
    niobium::Compiler::CacheParameters params;
    params.push_back({"wl", argv[1]});
    params.push_back({"mode", count_only ? "count" : "full"});
    niobium::compiler().cache_parameters(params);
  }
  niobium::compiler().set_program_info(
      "server_encrypted_compute", "1.0", "fetch-by-similarity encrypted compute");
  niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);
#endif

  // Read the crypto context and the public key from disk
  CryptoContext<DCRTPoly> cc;
  if (!Serial::DeserializeFromFile(prms.keydir()/"cc.bin", cc, SerType::BINARY)) {
    throw std::runtime_error("Failed to get CryptoContext from "+prms.keydir().string());
  }
  PublicKey<DCRTPoly> pk;
  if (!Serial::DeserializeFromFile(prms.keydir()/"pk.bin", pk, SerType::BINARY)) {
    throw std::runtime_error("Failed to get public key from "+prms.keydir().string());
  }
#ifdef DEBUG // Read also the secret key for debugging
  if (!Serial::DeserializeFromFile(prms.keydir()/"sk.bin", sk, SerType::BINARY)) {
    throw std::runtime_error("Failed to get secret key from "+prms.keydir().string());
  }
#endif

  std::ifstream emult_file(prms.keydir()/"mk.bin", std::ios::in | std::ios::binary);
  if (!emult_file.is_open() ||
      !cc->DeserializeEvalMultKey(emult_file, SerType::BINARY)) {
    throw std::runtime_error(
      "Failed to get re-linearization key from " +prms.keydir().string());
  }

  std::ifstream erot_file(prms.keydir()/"rk.bin", std::ios::in | std::ios::binary);
  if (!erot_file.is_open() ||
      !cc->DeserializeEvalAutomorphismKey(erot_file, SerType::BINARY)) {
    throw std::runtime_error(
      "Failed to get rotation keys from " +prms.keydir().string());
  }

  // Read the query vector from disk
  auto q_fname = prms.updir()/"query.bin";
  Ciphertext<DCRTPoly> eqry;
  if (!Serial::DeserializeFromFile(q_fname,eqry,SerType::BINARY)){
    throw std::runtime_error(
      "failed to read query ciphertext from " + q_fname.string());
  }
  log_step(0, "Loading keys");

  auto start_computing = std::chrono::system_clock::now();

  // `out` is always reconstructed from the trace via replay()+result() below.
  // On a cache miss the computation below also records the trace in hollow
  // mode, so the value it leaves in `out` is incorrect and is overwritten by
  // the replay; on a cache hit the computation is skipped entirely.
  Ciphertext<DCRTPoly> out;
#ifdef NIOBIUM_COMPILER
  const bool replaying = niobium::compiler().is_cache_valid();
#else
  const bool replaying = false;
#endif

  if (!replaying) {
#ifdef NIOBIUM_COMPILER
    niobium::compiler().start();                    // begin recording the FHETCH trace
    niobium::compiler().enable_hollow_mode(true);   // skip expensive FHE math while recording
#endif
  // Matrix-vector multiplication, reading the encrypted matrix one
  // ciphertexe at a time from updir
  auto result = mat_vec_mult(prms.updir(), eqry, prms);
  log_step(1, "Matrix-vector product");

  // Compare each slot in the results ctxts to the threshold, using a
  // Chebyshev approximation of the indicator function chi(x)=(x>=threshold).
  // If we only want to count the matches, then we use use a higher-degree
  // approximation since (a) we care about good approximation for both matches
  // and non-matches and (b) we can afford it level-wise.
  // Otherwise we use lower-degree approximation since we care a little less
  // about the accuracy of matches, more about non-matches (as we have more of
  // them). Also, we scale it to 0/0.5 rather than 0/1, since we sum up upto
  // eight matches, then multiply by the original thing, and need to fit the
  // result to a size-2 interval that can be shifted to the interval [-1,1].
  compare_to_threshold(result, threshold, count_only);
  log_step(2, "Compare to threshold");
#ifdef DEBUG
    printCts(result, " match vector:");
#endif

  // If we only want to count matches, return the total sum
  // of all the slots in all the ciphertexts.
  if (count_only) {
    for (size_t i=1; i<result.size(); i++) {
      cc->EvalAddInPlace(result[0], result[i]);
    }
    result[0] = cc->EvalSum(result[0], prms.getNSlots());
    log_step(3, "Summation");
#ifdef DEBUG
    printCts({result[0]}, " summed match vector:");
#endif
    out = result[0];
  } else {

  // Make a deep copy of the matches, it will be multiplied back into the
  // result after the running-sum procedure
  std::vector<Ciphertext<DCRTPoly>> matches;
  matches.reserve(result.size());
  for (auto& ct : result) {
    matches.push_back(ct->Clone());
  }

  // The "compaction" procedure views the matches vector (made of multiple
  // ciphertexts of dimension N_SLOTS) as a matrix with N_COLS=prms.getNCols()
  // columns, and expect no more than eight matches per column. The columns
  // are packed equally-spaced in the ciphertexts, so each ciphertext contains
  // N_SLOTS/N_COLS entries from each column.
  // For example, if we had three ciphertexts with N_SLOTS=8 and N_COLS=4,
  // we would have two entries from each column per ciphertexts, and the
  // arrnagement is as follows:    [ a1 a2 a3 a4 d1 d2 d3 d4 ]
  //                               [ b1 b2 b3 b4 e1 e2 e3 e4 ]
  //                               [ c1 c2 c3 c4 f1 f2 f3 f4 ]
  // This represents a matrix with i'th column being [ai bi ci di ei fi]^t,
  // we expect no more than 8 ones in each column.

  // Running sums in each column, so the first match will have value 1,
  // the second match will have 2, etc.
  RunningSums rs(cc,prms.getNCols(),RUNNING_SUM_LEVELS,result[0]->GetLevel());
  rs.eval_in_place(result);  // The actual running-sums procedure

  // Multiply by the matches vector, to zero out all the non-matches
  for (size_t i = 0; i < result.size(); i++) {
    result[i] = cc->EvalMult(result[i], matches[i]);
  }
  matches.clear();          // not needed anymore
  matches.shrink_to_fit();  // release the memory

  // Contents of slots are now in the range [0,2], shift them to [-1,1]
  for (auto& ct : result) {
    cc->EvalSubInPlace(ct, 1.0);
  }
  log_step(3, "Running sums");

  // We now get the actual payload data corresponding to the matches. Recall
  // that we expect at most MAX_N_MATCH(=8) matches per column: the 1st is
  // marked by a 1 slot in the result ciphertext, the 2nd by a 2 slot, etc.
  // Recall also that we have PAYLOAD_DIM(=8) of payload slots per record.

  // To get the actual data, we run MAX_N_MATCH(=8) iterations, in the i'th
  // iteration we isolate the PAYLOAD_DIM payload slots of the ith match
  // (i.e., the slot that contains i). We first compute an "one hot" indicator
  // ctxt with 1 in the slots where result has i, and zero elsewhere (so we
  // have a single 1 per column).

  // Once we compute the i'th indicator, we need to extract the PAYLOAD_DIM
  // payload entries in the columns corresponding to the 1s in this indicator,
  // then move them slots {i*PAYLOAD_DIM,...,(i+1)*PAYLOAD_DIM-1} in their
  // column. We do it in four steps:
  // 1. We multiply each of the PAYLOAD_DIM encrypted payload vectors by the
  //   indicator vector. This yeilds PAYLOAD_DIM vectors with the jth one
  //   containing the jth payload value of the records corresponding to the
  //   1s in the indicator. Each column has at most one non-zero payload
  //   values, all in the same slot index.
  // 2. We tile these PAYLOAD_DIM vectors so that the non-zero values appear
  //   in consecutive positions in the column. Since columns in spread
  //   acorss the slots then it means that the PAYLOAD_DIM payload slots
  //   for one record will appear in slots {x, x+N_COLS, x+2*N_COLS,...},
  //   where x is the slot where the indicator has 1 (in that column).
  // 3. We replicate the values across that column, so that it contains
  //   these PAYLOAD_DIM values repeatedly in all the slots in that column.
  // 4. We multiply the result by a mask which is 1 in positions
  //   {i*PAYLOAD_DIM,...,(i+1)*PAYLOAD_DIM-1} in each column and zero
  //   elsewhere.

  Ciphertext<DCRTPoly> accumulator;
  for (int i = 1; i <= prms.getMaxNMatch(); i++) {  // extract i'th match
    double x_i = i / 4.0 - 1.0;  // map from {0,8} to the interval [-1,1]
    auto indicator = compare_to_number(result, x_i);

    // Indicator has as many ciphertexe as it takes to store a row of the keys
    // matrix (i.e., one slot for each dataset recrod). It's a "one hot" vector
    // vector per column, containing 1 in slots where partial_sums contained i

    // A place holder for the extracted payload, before moving them to
    // their place in the output columns.
    Ciphertext<DCRTPoly> to_replicate;
    for (size_t j = 0; j < PAYLOAD_DIM; j++) {
      // Steps 1 & 2: Multiply by the indicator to get a single payload value
      // per column, then rotate by j*N_COLS to put that value in the next
      // available slot in its column.
      for (size_t k = 0; k < indicator.size(); k++) {
        auto payload_part = get_encrypted_payload(prms.updir(), k, j);
        // jth row in the k'th matrix

        payload_part = cc->EvalMult(payload_part, indicator[k]);

        // Shift the j'th payload value by j positions in its column, so we
        // pack all PAYLOAD_DIM=8 values consecutively in their column.
        if (j == 0 && k == 0) {   // initialize the inner-loop accumulator
          to_replicate = payload_part->Clone();
        } else {
          if (j != 0) {  // shift by j in its column
            payload_part = cc->EvalRotate(payload_part, -j * prms.getNCols());
          }
          to_replicate = cc->EvalAdd(to_replicate,payload_part);  // accumulate
        }
        // We assume that indicator has a single 1 in each output column and
        // all else are zero. So for each slot index s<N_SLOTS, at most one
        // of the values added to to_replicate[s] will be non-zero. This let us
        // use a single cipehrtext for to_replicate, even though the indicator
        // is a vector of ciphertexts, we just add everything and are assured
        // that at most one of the terms is non-zero.
      }
    }

    // Step 3: replicate the values across the column
    // We need to move the (potential) PAYLOAD_DIM non-zero slots in each
    // output column to positions {i*PAYLOAD_DIM,...,(i+1)*PAYLOAD_DIM-1}
    // in that column. This is done by first replicating them so that they
    // fill the entire column, then multiplying by a mask that zero out
    // everything else, leaving only those positions.
    auto replicated = total_sums(to_replicate, prms);

    // Step 4: multiply by a mask
    std::vector<double> slots(prms.getNSlots(), 0.0);
    for (size_t ell = 0; ell < slots.size(); ell++) {
      int row = ell / prms.getNCols();  // index within column
      if (row >= (i - 1) * PAYLOAD_DIM && row < i * PAYLOAD_DIM) {
        slots[ell] = 1.0;
      }
    }
    auto mask = cc->MakeCKKSPackedPlaintext(slots, 1, replicated->GetLevel());
    auto masked = cc->EvalMult(replicated, mask);

    // Finally, add the payload values to all the other matches in that column
    if (i == 1) {  // initialize the outter accumulator
      accumulator = masked;
    } else {
      accumulator = cc->EvalAdd(accumulator, masked);
    }
  }
  log_step(4, "Output compression");
    out = accumulator;
    }  // end else (full payload path)
#ifdef NIOBIUM_COMPILER
    niobium::compiler().enable_hollow_mode(false);  // must be OFF for probe/stop
    niobium::compiler().probe("result", out);
    niobium::compiler().stop();  // finalize the trace
#endif
  }  // end if (!replaying)
#ifdef NIOBIUM_COMPILER
  // Always replay to obtain the correct result: hollow-mode recording skips the
  // real FHE math (so the recorded `out` is incorrect), and the cache-hit path
  // ran no FHE at all. replay() dispatches to the --target backend (local
  // fhetch_driver / remote compiler); result() reconstructs the output.
  if (!niobium::compiler().replay()) {
    throw std::runtime_error("niobium replay failed");
  }
  if (!niobium::compiler().result(cc, "result", out) || !out) {
    throw std::runtime_error("niobium result reconstruction failed");
  }
#endif

  // Report the server-side overall computation time
  auto now = std::chrono::system_clock::now();
  int64_t comp_s = std::chrono::duration_cast<std::chrono::seconds>(
    now - start_computing).count();
  int64_t total_s = std::chrono::duration_cast<std::chrono::seconds>(
    now - start_server).count();
  store_server_time(timing_fname, comp_s, total_s);

  // Store the result back to disk
  std::string out_fname = prms.downdir()/"results.bin";
  if (!Serial::SerializeToFile(out_fname, out, SerType::BINARY)) {
    throw std::runtime_error("Failed to write ciphertext to " + out_fname);
  }
  return 0;
}
/*******************************************************************/
/*******************************************************************/


/*******************************************************************/
// Matrix-vector product: The matrix rows are stored on disk in batches
// under iodir/<size>/encrypted/batchNNNN/. The query ciphertext contains
// the query vector, repeatd to fill in all the slots.
std::vector<Ciphertext<DCRTPoly>> mat_vec_mult(fs::path encdir,
                Ciphertext<DCRTPoly> qry, const InstanceParams& prms)
{
  CryptoContext<DCRTPoly> cc = qry->GetCryptoContext();

  // The input ciphertext includes a pattern of length RECORD_DIM,
  // repeated N_SLOTS/RECORD_DIM many times to fill all the slot
  auto n_reps = prms.getNSlots() / prms.getRecordDim();
  DFSSlotReplicator replicator(cc, prms.getDegrees(), n_reps);

  auto n_batches = prms.getNCtxts();
  std::vector<Ciphertext<DCRTPoly>> acc(n_batches);  // an accumulator
  size_t i = 0;  // i is the ciphertext index within a batch
  for (auto ct_i = replicator.init(qry); ct_i != nullptr;
       ct_i = replicator.next_replica(), i++) {
       // ct_i has the i'th entry of the query vector in all its slots

    // read a row from each batch, multiply by ct_i and accumulate
    std::stringstream ssi;
    ssi << std::setw(4) << std::setfill('0') << i;
    for (int j = 0; j < n_batches; j++) {  // j is the batch index
      std::stringstream ssj;
      ssj << std::setw(4) << std::setfill('0') << j;

      auto ct_fname = encdir /
          ("batch" + ssj.str()) / ("row_" + ssi.str() + ".bin");
      Ciphertext<DCRTPoly> ct = get_ctxt(ct_fname);
      ct = cc->EvalMultNoRelin(ct, ct_i);
      if (i == 0) {  // initialize the accumulator
        acc[j] = ct;
      } else {       // add to the accumulator
        cc->EvalAddInPlace(acc[j], ct);
      }
    }
  }
  // relinearize the accumulators
  for (int j = 0; j < n_batches; j++) {
    cc->RelinearizeInPlace(acc[j]);
  }
  return acc;
}

/*******************************************************************/
// Compare each slot in the results ctxts to the threshold, using a
// Chebyshev approximation of the indicator function chi(x)=(x>=threshold).
// If we only want to count the matches, then we use use a higher-degree
// approximation since (a) we care about good approximation for both matches
// and non-matches and (b) we can afford it level-wise.
// Otherwise we use lower-degree approximation since we care a little less
// about the accuracy of matches, more about non-matches (as we have more of
// them). Also, we scale it to 0/0.5 rather than 0/1, since we sum up upto
// eight matches, then multiply by the original thing, and need to fit the
// result to a size-2 interval that can be shifted to the interval [-1,1].

// A sigmoid-like function. The constant 69 was determined by experiments
constexpr double sigmoid_inscale = 69.0;
double sigmoid(double x, double outscale = 1.0,
               double inscale = sigmoid_inscale) {
  return outscale / (1.0 + std::exp(-(x * inscale)));
}

void compare_to_threshold(std::vector<Ciphertext<DCRTPoly>>& ctxts,
                          double threshold, bool count_only) {
  double outscale = count_only? 1.0 : 0.504;
  auto func = [threshold, outscale](double x) {
    return sigmoid(x - threshold, outscale);
  };
  size_t degree = (count_only? 247 : 59);  // options are 59, 119, 247
  auto cc = ctxts.front()->GetCryptoContext();
  for (auto& ct : ctxts) {
    ct = cc->EvalChebyshevFunction(func, ct, -1.0, 1.0, degree);
  }
  // NOTE: If these results are not accurate enough then we can either switch
  // to higher-degree approximation or just suqare the result to get a better
  // approximation of the non-matches.
}

/*******************************************************************/
// Compare each point in the vectors to the number, using a Chebyshev
// approximation of the function chi(x) = (x == number).

// An impulse-like function, with impule(0)==1.
// The constant 0.04 was determined by experiments.
constexpr double impulse_sigma = 0.04;
double impulse(double x, double sigma = impulse_sigma) {
  double x_over_sigma = x / sigma;
  return std::exp(-x_over_sigma*x_over_sigma / 2);
}

std::vector<Ciphertext<DCRTPoly>> compare_to_number(
    const std::vector<Ciphertext<DCRTPoly>>& ctxts, double number) {
  auto func = [number](double x) {
    return impulse(x - number);
  };
  constexpr size_t degree = 119;  // options are 59, 119, 247

  auto cc = ctxts[0]->GetCryptoContext();
  std::vector<Ciphertext<DCRTPoly>> results;
  results.reserve(ctxts.size());
  for (auto& ct : ctxts) {
    results.push_back(cc->EvalChebyshevFunction(func, ct, -1.0, 1.0, degree));
  }
  return results;
}

/*******************************************************************/
// A SIMD-optimized procedure for computing total sums. The slots are viewed
// as a matrix, and total sums are computed in each column separately.
// All the entries of an output column contain the total sum of entries from
// that column in the input.
Ciphertext<DCRTPoly> total_sums(
  const Ciphertext<DCRTPoly>& ct, const InstanceParams& prms) {
  int period = prms.getNCols() * PAYLOAD_DIM;
  int s = std::log2(prms.getNSlots() / period);
  int r = std::log2(period);
  assert(unsigned(prms.getNSlots()) == 1UL<<(s+r));  // must be a power of two
  auto results = ct->Clone();
  auto cc = results->GetCryptoContext();

  // Total sums inside the vectors, in columns
  for (int i = s - 1; i >= 0; i--) {
    // cyclic rotation of results by 2^{i+r}
    int rot_amount = 1 << (i + r);
    auto tmp = cc->EvalRotate(results, rot_amount);
    // Add tmp back to the results
    cc->EvalAddInPlace(results, tmp);
  }
  return results;
}

// Read the ith payload value in a batch of records from disk
Ciphertext<DCRTPoly> get_encrypted_payload(fs::path datadir, size_t batch,
                                            size_t idx) {
  std::stringstream ssi, ssj;
  ssj << std::setw(4) << std::setfill('0') << batch;
  ssi << std::setw(4) << std::setfill('0') << idx;
  auto dir = datadir / ("batch" + ssj.str());
  auto ct_fname = dir / ("payload_" + ssi.str() + ".bin");

  // read the i'th payload ciphertext from this batch
  Ciphertext<DCRTPoly> result;
  if (!Serial::DeserializeFromFile(ct_fname, result, SerType::BINARY)) {
    throw std::runtime_error("failed to read ciphertext from " + ct_fname.string());
  }
  return result;
}
