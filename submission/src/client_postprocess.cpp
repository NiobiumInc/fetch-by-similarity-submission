// client_postprocess.cpp - Client post-process the answer from server
//============================================================================
// Copyright (c) 2025, Amazon Web Services
// All rights reserved.
//
// This software is licensed under the terms of the Apache License v2.
// See the file LICENSE.md for details.
//============================================================================
#include <cassert>

#include "params.h"
#include "utils.h"
#include "running_sums.h"

std::vector<std::vector<int16_t>>
decode_results(const std::vector<double>& slots, int n_cols);

int main(int argc, char* argv[]) {
  if (argc < 2 || !std::isdigit(argv[1][0])) {
    std::cout << "Usage: " << argv[0] << " instance-size [--count_only]\n";
    std::cout << "  Instance-size: 0-TOY, 1-SMALL, 2-MEDIUM, 3-LARGE\n";
    return 0;
  }
  auto size = static_cast<InstanceSize>(std::stoi(argv[1]));
  InstanceParams prms(size, ring_dim_from_args(argc, argv));

  bool count_only = (argc > 2 && std::string(argv[2])=="--count_only");

  // Read the raw result slots from disk
  auto vs = read2vecs<double>(prms.iodir()/"raw-result.bin",prms.getNSlots());
  assert(vs.size()==1);
  auto slots = vs[0];

  if (count_only) {  // Write a single integer containing the sum
    long count = std::round(slots[0]);
    write2disk<long>(prms.iodir()/"results.bin", {{count}});
  } else {  // Decode the raw results to a list of playloads
    auto res = decode_results(slots, prms.getNCols());
    write2disk<int16_t>(prms.iodir()/"results.bin", res);
  }
  return 0;
}

// Decode the slots of the results, returning a vector of recrods
// each a vector of PAYLOAD_DIM-1 bytes
std::vector<std::vector<int16_t>>
decode_results(const std::vector<double>& slots, int n_cols) {
  auto result_matrix = RunningSums::to_matrix_form({slots}, n_cols);
  std::vector<std::vector<int16_t>> obtained_vals;
  for (int j = 0; j < n_cols; j++) {
    for (size_t i = 0; i < result_matrix.size(); i += PAYLOAD_DIM) {
      int marker = -1;
      double maxval = 0;  // look for a non-zero among the PAYLOAD_DIM slots
      for (size_t ii = 0; ii < PAYLOAD_DIM; ii++) {
        if (result_matrix[i + ii][j] > maxval) {
          maxval = result_matrix[i + ii][j];
          marker = ii;
        }
      }  // For a match, maxval should be the marker, ~2*MAX_PAYLOAD_VAL
      if (maxval > MAX_PAYLOAD_VAL) {
        // Normalize and rotate the payload vector as needed.
        if (maxval < MAX_PAYLOAD_VAL * 1.4) { // An error condition
          std::stringstream ss;
          for (size_t k = 0; k < PAYLOAD_DIM; k++) {
            auto x = result_matrix[i + k][j];
            ss << x << ' ';
          }
          throw(std::runtime_error(
            "marker not found in payload: [" + ss.str() + "]"));
        }
        // The marker should be 2*MAX_PAYLOAD_VAL*PAYLOAD_PRECISION after
        // scaling, and the other values should be integers between 0
        // and MAX_PAYLOAD_VAL*PAYLOAD_PRECISION -1
        double scale = (MAX_PAYLOAD_VAL * 2 * PAYLOAD_PRECISION) 
                        / (result_matrix[i + marker][j]);
        std::vector<int16_t> rec(PAYLOAD_DIM - 1);
        for (size_t k = 1; k < PAYLOAD_DIM; k++) {
          auto idx = i + ((marker + k) % PAYLOAD_DIM);
          rec[k - 1] = std::round(scale * result_matrix[idx][j]);
        }
        obtained_vals.push_back(rec);
      }
    }
  }
  std::sort(obtained_vals.begin(), obtained_vals.end());
  return obtained_vals;
}
