#!/usr/bin/env python3
"""
run_submission.py - run the entire submission process, from build to verify
"""
# Copyright (c) 2025, Amazon Web Services
# All rights reserved.
#
# This software is licensed under the terms of the Apache v2 License.
# See the LICENSE.md file for details.
import sys
import os
import argparse
import subprocess
from pathlib import Path
import numpy as np
import utils
from params import InstanceParams, TOY, LARGE, instance_name

def main():
    """
    Run the entire submission process, from build to verify
    """
    # Parse arguments using argparse
    parser = argparse.ArgumentParser(description='Run the fetch-by-similarity FHE benchmark.')
    parser.add_argument('size', type=int, choices=range(TOY, LARGE+1),
                        help='Instance size (0-toy/1-small/2-medium/3-large)')
    parser.add_argument('--num_runs', type=int, default=1,
                        help='Number of times to run steps 4-9 (default: 1)')
    parser.add_argument('--seed', type=int,
                        help='Random seed for dataset and query generation')
    parser.add_argument('--count_only', action='store_true',
                        help='Only count # of matches, do not return payloads')
    parser.add_argument('--remote', action='store_true',
                        help='Run example submission in remote backend mode')
    parser.add_argument('--target', default='local',
                        help='Replay target for server_encrypted_compute (cooperative mode). '
                             '"local" (default) replays via the in-tree fhetch_driver; any other '
                             'value ships the recorded trace to a running '
                             'nbcc_fhetch_replay_server (set NBCC_FHETCH_SERVER for a non-local URL). '
                             'Use "FOG" to run on Niobium\'s stable FPGA device — the server '
                             'resolves it to its pinned hardware id, so you never need to know '
                             'internal device names. Other values (e.g. FUNC_SIM_HW) are '
                             'forwarded to the server verbatim.')
    parser.add_argument('--toy-ring-dim', dest='toy_ring_dim', action='store_true',
                        help='Run with reduced ring dimension 2^11 instead of 2^16. '
                             'Only valid for size 0 (TOY) with --target local. '
                             'NOTE: 2^11 does not provide 128-bit security; this is '
                             'for fast functional iteration only.')
    parser.add_argument('--opt-level', dest='opt_level', default='O3',
                        help='Optimization level (O0..O3) for the compiler-side replay. '
                             'Forwarded to server_encrypted_compute and on to the replay '
                             'server. Omitted means O3 for maximum performance.')

    args, _ = parser.parse_known_args()
    size = args.size
    remote_be = args.remote

    if args.toy_ring_dim:
        if size != TOY or args.target != 'local' or remote_be:
            parser.error('--toy-ring-dim is only allowed with size 0 (TOY) '
                         'and --target local (and without --remote)')

    # Use params.py to get instance parameters
    params = InstanceParams(size, args.count_only)


    # Ensure the required directories exist
    utils.ensure_directories(params.rootdir)

    # Verify dependencies and build the submission, if not built already
    utils.build_submission(params.rootdir/"scripts", remote_be)

    # The harness scripts are in the 'harness' directory,
    # the submission code is either in submission or submission_remote
    harness_dir = params.rootdir/"harness"
    exec_dir = params.rootdir/ ("submission_remote/src" if remote_be else "submission")

    # Cooperative record/replay env for server_encrypted_compute (Niobium).
    # The binary owns the explicit lifecycle; its replay() dispatches a disk
    # replay — local via the in-tree fhetch_driver (NBCC_FHETCH_DRIVER), or for
    # a non-local --target via the transport forwarder (NBCC_FHETCH_REPLAY) to a
    # running server (NBCC_FHETCH_SERVER, default http://127.0.0.1:9443).
    if not remote_be:
        client_dir = params.rootdir / "submission" / "niobium-client"
        openfhe_lib = client_dir / "vendor" / "lib" / "openfhe" / "lib"
        env_file = client_dir / "build" / "niobium_client.env"
        if env_file.exists():
            for line in env_file.read_text().splitlines():
                if line.startswith("OPENFHE_LIB="):
                    openfhe_lib = Path(line.split("=", 1)[1].strip())
        for var in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            prev = os.environ.get(var)
            os.environ[var] = os.pathsep.join([str(openfhe_lib)] + ([prev] if prev else []))
        if args.target == "local":
            os.environ["NBCC_FHETCH_DRIVER"] = str(
                client_dir / "vendor" / "niobium-fhetch" / "build" / "tests"
                / "fhetch_driver" / "fhetch_driver")
        else:
            os.environ["NBCC_FHETCH_REPLAY"] = str(
                client_dir / "build" / "src" / "fhetch_transport" / "nbcc_fhetch_replay")

    print(f"\n[harness] Running submission for {instance_name(size,args.count_only)} dataset")
    if args.count_only:
        print("          only counting matches")
    else:
        print("          returning matching payloads")

    # 0. Generate the dataset (and centers) using generate_dataset.py

    # Remove and re-create IO directory
    io_dir = params.iodir()
    if io_dir.exists():
        subprocess.run(["rm", "-rf", str(io_dir)], check=True)
    io_dir.mkdir(parents=True)

    if args.seed is not None:
        np.random.seed(args.seed)
        rng = np.random.default_rng(args.seed)
    utils.log_step(0, "Init", True)

    # Common command-line arguments for all steps
    cmd_args = [str(size), ]
    if args.count_only:
        cmd_args.extend(["--count_only"])
    if args.toy_ring_dim:
        # Forwarded to every step; the C++ binaries pick it up via
        # ring_dim_from_args() (params.h), the harness scripts ignore it.
        cmd_args.extend(["--ring_dim", str(2**11)])
    query_args = cmd_args      # Query steps should not get the global seed
    if args.seed is not None:  # Use seed if provided
        generic_seed = rng.integers(0,0x7fffffff)
        cmd_args.extend(["--seed", str(generic_seed)])

    # 1. Client-side: Generate the datasets
    utils.run_exe_or_python(harness_dir, "generate_dataset", *cmd_args)
    utils.log_step(1, "Dataset generation")

    # 1.1 Communication: Get cryptographic context
    if remote_be:
        utils.run_exe_or_python(exec_dir, "server_get_params", str(size))
        utils.log_step(1.1 , "Communication: Get cryptographic context")

    # 2. Client-side: Preprocess the dataset using client_preprocess_dataset
    utils.run_exe_or_python(exec_dir, "client_preprocess_dataset", *cmd_args)
    utils.log_step(2, "Dataset preprocessing")

    # 3. Client-side: Generate the cryptographic keys
    # Note: this does not use the rng seed above, it lets the implementation
    #   handle its own prg needs. It means that even if called with the same
    #   seed multiple times, the keys and ciphertexts will still be different.
    utils.run_exe_or_python(exec_dir, "client_key_generation", *cmd_args)
    utils.log_step(3, "Key Generation")

    # Report size of keys
    utils.log_size(io_dir / "keys", "Public and evaluation keys")

    # 3.1 Communication: Upload evaluation key
    if remote_be:
        utils.run_exe_or_python(exec_dir, "server_upload_ek", str(size))
        utils.log_step(3.1 , "Communication: Upload evaluation key")

    # 4. Client-side: Encode and encrypt the dataset
    utils.run_exe_or_python(exec_dir, "client_encode_encrypt_db", *cmd_args)
    utils.log_step(4, "Dataset encoding and encryption")

    # Report size of encrypted data
    utils.log_size(io_dir / "ciphertexts_upload", "Encrypted database")

    # 4.1 Communication: Upload encrypted database
    if remote_be:
        utils.run_exe_or_python(exec_dir, "server_upload_db", str(size))
        utils.log_step(4.1 , "Communication: Upload encrypted database")


    # 5. Server-side: Preprocess the encrypted dataset using server_preprocess_dataset
    utils.run_exe_or_python(exec_dir, "server_preprocess_dataset", *cmd_args)
    utils.log_step(5, "Encrypted dataset preprocessing")

    # Run steps 6-11 multiple times if requested
    for run in range(args.num_runs):
        if args.num_runs > 1:
            print(f"\n         [harness] Run {run+1} of {args.num_runs}")

        # 6. Client-side: Generate a new random query using generate_query.py
        this_query_args = query_args
        if args.seed is not None:  # Use dervied seed if seed argument is provided
            genqry_seed = rng.integers(0,0x7fffffff)
            this_query_args.extend(["--seed", str(genqry_seed)])
        utils.run_exe_or_python(harness_dir, "generate_query", *this_query_args)
        utils.log_step(6, "Query generation")

        # 7. Client-side: preprocess query
        utils.run_exe_or_python(exec_dir, "client_preprocess_query", *this_query_args)
        utils.log_step(7, "Query preprocessing")

        # 8. Client-side: Encrypt the query
        utils.run_exe_or_python(exec_dir, "client_encode_encrypt_query", *this_query_args)
        utils.log_step(8, "Query encryption")
        utils.log_size(io_dir / "ciphertexts_upload" / "query.bin" , "Encrypted query")

        # 9. Server-side: run server_encrypted_compute. --target selects the
        #    cooperative replay path (the binary's init() consumes it).
        compute_args = list(this_query_args)
        if not remote_be:
            compute_args += ["--target", args.target]
            if args.opt_level:
                compute_args += ["--opt-level", args.opt_level]
            if args.toy_ring_dim:
                # 2^11 fails the Niobium hardware compatibility checks
                # (ring dim != 2^16, primes not = 1 mod 2^16); fine for the
                # local functional simulator.
                compute_args += ["--no-ring-dim-check", "--no-prime-check"]
        utils.run_exe_or_python(exec_dir, "server_encrypted_compute", *compute_args)
        utils.log_step(9, "Encrypted computation")
        utils.log_size(io_dir / "ciphertexts_download" / "results.bin" , "Encrypted results")

        # 10. Client-side: decrypt and postprocess
        utils.run_exe_or_python(exec_dir, "client_decrypt_decode", *this_query_args)
        utils.run_exe_or_python(exec_dir, "client_postprocess", *this_query_args)
        utils.log_step(10, "Result decryption and postprocessing")

        # 11. Run the plaintext processing in cleartext_impl.py and verify_results
        utils.run_exe_or_python(harness_dir, "cleartext_impl", *this_query_args)

        # 12. Verify results
        expected_file = params.datadir() / "expected.bin"
        result_file = io_dir / "results.bin"

        if not result_file.exists():
            print(f"Error: Result file {result_file} not found")
            sys.exit(1)

        utils.run_exe_or_python(harness_dir, "verify_result", str(expected_file), str(result_file), *this_query_args[1:])  # skip size arg

        # 13. Store measurements
        run_path = params.measuredir() / f"results-{run+1}.json"
        run_path.parent.mkdir(parents=True, exist_ok=True)
        submission_report_path = io_dir / "server_reported_steps.json"
        utils.save_run(run_path, submission_report_path)

    print(f"\nAll steps completed for the {instance_name(size,args.count_only)} dataset!")

if __name__ == "__main__":
    main()
