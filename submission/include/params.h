#ifndef PARAMS_H_
#define PARAMS_H_
/// params.h - parameters and directory structure for similarity search
//============================================================================
// Copyright (c) 2025, Amazon Web Services
// All rights reserved.
//
// This software is licensed under the terms of the Apache License v2.
// See the file LICENSE.md for details.
//============================================================================
#include <vector>
#include <string>
#include <stdexcept>
#include <filesystem>
namespace fs = std::filesystem;

// The level budget for the running-sums procedure
constexpr int RUNNING_SUM_LEVELS = 3;

// The payload slots contain numbers in the range [0,MAX_PAYLOAD_VAL)
// with precision of 1/PAYLOAD_PRECISION
constexpr int MAX_PAYLOAD_VAL = 256;
constexpr int PAYLOAD_PRECISION = 16;

// The dimension of the payload vectors (currently fixed to 8)
constexpr int PAYLOAD_DIM = 8;

// an enum for benchmark size
enum InstanceSize {
    TOY = 0,
    SMALL = 1,
    MEDIUM = 2,
    LARGE = 3
};
inline std::string instance_name(const InstanceSize size) {
    if (unsigned(size) > unsigned(InstanceSize::LARGE)) {
        return "unknown";
    }
    static const std::string names[] = {"toy", "small", "medium", "large"};
    return names[int(size)];
}

// Scan the command line for "<flag> value" or "<flag>=value" and return
// the value, or `dflt` if the flag is absent.
inline std::string arg_value(int argc, char* argv[], const std::string& flag,
                             const std::string& dflt = "") {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == flag && i + 1 < argc) {
            return argv[i + 1];
        }
        if (a.rfind(flag + "=", 0) == 0) {
            return a.substr(flag.size() + 1);
        }
    }
    return dflt;
}

// Scan the command line for an optional "--ring_dim N" argument, as
// forwarded by harness/run_submission.py --toy-ring-dim. Returns 0 if
// absent, meaning "use the default ring dimension".
inline int ring_dim_from_args(int argc, char* argv[]) {
    auto v = arg_value(argc, argv, "--ring_dim");
    return v.empty() ? 0 : std::stoi(v);
}

// Parameters that differ for different instance sizes
class InstanceParams {
    const InstanceSize size;
    int recordDim;  // dimension of the plaintext record
    int dbSize;     // number of records in the dataset
    int ringDim;    // dimenion of the FHE ring
    std::vector<int> degrees;  // must multiply to the record dimension
    fs::path rootdir; // root of the submission dir structure (see below)

public:
    // Constructor. _ringDim==0 means the default (65536); a non-default
    // ring dimension is only supported for the TOY instance (fast local
    // iteration, NOT cryptographically secure).
    explicit InstanceParams(InstanceSize _size, int _ringDim = 0,
                            fs::path _rootdir = fs::current_path())
                            : size(_size), rootdir(_rootdir)
    {
        if (unsigned(_size) > unsigned(InstanceSize::LARGE)) {
            throw std::invalid_argument("Invalid instance size");
        }
        // parameters for sizes:       toy  small   medium      large
        static const int recDims[] = { 128,   128,     256,      512};
        static const int dbSizes[] = {1000, 50000, 1000000, 20000000};

        ringDim = (_ringDim > 0) ? _ringDim : 65536;
        if (ringDim != 65536 && _size != InstanceSize::TOY) {
            throw std::invalid_argument(
                "A non-default ring dimension is only supported for TOY");
        }
        recordDim = recDims[int(_size)];
        dbSize    = dbSizes[int(_size)];

        // NOTE: The degrees vector specifies the shape of the tree used by
        // by the slot replicator. The entires must multiply to the record
        // dimension, and for a given shape the slot-replicator consumes
        // degrees.size() levels of mult-by-constant.
        // In theory, given a depth bound d, the best shape of the tree should
        // have been {dim/2^{d-1}, 2, ..., 2}, but in practice this is not
        // what happens. Maybe due to multi-threading??
        // Below are some fixed shapes for the different sizes. These are
        // unlikely to be optimal, the optimal shape is likely dependent on
        // the specific hardware platform. But at least for the larger sizes,
        // the replication time should be insignificant.
        switch (_size) {
            case InstanceSize::LARGE:
                degrees = {16, 8, 4};
                break;
            case InstanceSize::MEDIUM:
                degrees = {8, 8, 4};
                break;
            default:
                degrees = {8, 4, 4};
        }
    }

    // Getters for all the parameters. There are no setters, once
    // an object is constrcuted these parameters cannot be modified.
    const InstanceSize getSize() const { return size; }
    int getRecordDim() const { return recordDim; }
    int getDbSize() const { return dbSize; }
    int getRingDim() const { return ringDim; }
    std::vector<int> getDegrees() const { return degrees; }
    int getNSlots() const { return ringDim/2; } // # of plaintext slots

    // # of ciphertexts needed to hold one column of the dataset
    int getNCtxts() const {
        return (dbSize + getNSlots() - 1) / getNSlots(); 
    }

    // We view each ciphertext (with ringDim/2 slots) as a matrix with
    // 64 rows and rindDim/128 columns
    int getNCols() const { return ringDim/128; }

    // Since each payload takes PAYLOAD_DIM sots and columns have 64 slots
    // each, then a column can hold at most 64/PAYLOAD_DIM payload values
    int getMaxNMatch() const { return 64 / PAYLOAD_DIM; };

    // Directory structure: each submission to the fetch-by-similarity
    // workload in the FHE benchmarking is a branch of the repository
    //      https://github.com/fhe-benchmarking/fetch-by-similarity,
    // with (a subset of) the following directory structure:
    // [root] /
    //  ├─datasets/   # Holds cleartext data (centers.bin, db.bin, query.bin)
    //    ├─ toy/     # each instance-size in in a separate subdirectory
    //    ├─ small/
    //    ├─ medium/
    //    ├─ large/
    //  ├─docs/       # Documentation (beyond the top-level README.md)
    //  ├─harness/    # Scripts to generate data, run workload, check results
    //  ├─build/      # Handle installing dependencies and building the project
    //  ├─submission/ # The implementation, this is what the submitters modify
    //    └─ README.md  # likely also a src/ subdirectory, CMakeLists.txt, etc.
    //  ├─io/         # Directory to hold the I/O between client & server parts
    //    ├─ toy/       # The reference implementation has subdirectories
    //       ├─ keys/       # holds the keys
    //       ├─ ciphertexts_upload/  # holds the ciphertexts to be uploaded to the server
    //       └─ ciphertexts_download/  # holds the ciphertexts to be downloaded from the server    
    //    ├─ small/
    //       …
    //    ├─ medium/
    //       …
    //    ├─ large/
    //       …
    // The relevant directories where things are found
    fs::path rtdir() const  { return rootdir; }
    fs::path iodir() const  { return rootdir/"io"/instance_name(size); }
    fs::path keydir() const { return iodir() / "keys"; }
    fs::path updir() const { return iodir() / "ciphertexts_upload"; }
    fs::path downdir() const { return iodir() / "ciphertexts_download"; }
    fs::path datadir() const { 
        return rootdir/"datasets"/instance_name(size);
    }
};

#endif  // ifndef PARAMS_H_