# FHE Benchmarking Suite - Fetch-by-Similarity Workload

This repository contains a reference implementation of the
Fetch-by-cosine-similarity workload of the FHE benchmarking suite of
[HomomorphicEncryption.org](https://www.HomomorphicEncryption.org).

Submitters need to fork this repository, then replace the content of
the `submission` or `submission_remote` subdirectory by their own implementation.
They also may need to changes or replace the script `scripts/build_task.sh`
to account for dependencies and build environment for their submission.

## Execution Modes

The Fetch-by-cosine-similarity benchmark supports two execution modes:

### Local Execution (Default)

All steps are executed on a single machine:
- Cryptographic context setup and preprocessing of the homomorphic workload
- Key generation
- DB preprocessing and encryption
- Query preprocessing and encryption
- Homomorphic DB cosine similarity search and retrieval
- Decryption and postprocessing

This corresponds to the reference submission in `submission/`.

### Remote Backend Execution (Optional)

Some FHE deployments separate client-side and server-side responsibilities.  
In this mode:

- **Client-side (local):**
  - Key generation
  - DB preprocessing and encryption
  - Query preprocessing and encryption
  - Decryption and postprocessing

- **Server-side (remote):**
  - Cryptographic context setup and preprocessing of the homomorphic workload
  - Homomorphic DB cosine similarity search and retrieval

This execution mode is enabled by passing the `--remote` flag to the harness, and the client-side implementation is under `submission_remote/`.

This reference repository contains two implementations, one local and one remote, serving as examples of how to use them. Note that closed-source software submissions can use either mode: They can use shims under `submission_remote/` that call a back-end server running the main implementation, or use shims under `submission/` that call a pre-compiled library or a container. That library/container must be included with the submission (e.g. using Github packages).

## Running the fetch-by-similarity workload
See also [Running it](https://github.com/NiobiumInc/fetch-by-similarity-submission/blob/main/NIOBIUM_INTEGRATION.md#running-it) in the Niobium integration guide.

#### Dependencies
- Python 3.12+
- The build environment for local execution depends on OpenFHE, which is built from the copy bundled inside `submission/niobium-client/vendor/niobium-fhetch/vendor/openfhe` by the niobium-client Makefile. See https://github.com/openfheorg/openfhe-development#installation.
- The build environment for remote-backend execution depends on lattica-query being installed as specified in `submission_remote/requirements.txt`. See https://platformdocs.lattica.ai/how-to-guides/client-installation/how-to-install-query-client. Should be installed on a `linux_x86_64` machine.

#### Execution
To run the workload, clone and install dependencies:
```console
git clone https://github.com/fhe-benchmarking/fetch-by-similarity.git
cd fetch-by-similarity

python -m venv virtualenv
source ./virtualenv/bin/activate
pip install -r requirements.txt

python3 harness/run_submission.py -h  # Information about command-line options
```

#### Submodules

This repository pulls in `submission/niobium-client` as a git submodule. The
submodule is configured to use HTTPS, so it can be cloned anonymously without a
GitHub account or SSH key. Be sure to fetch it when cloning:

```console
git clone --recurse-submodules https://github.com/NiobiumInc/fetch-by-similarity-submission.git
# or, if you already cloned without submodules:
git submodule update --init --recursive
```

If you prefer SSH (for example, because you push to these repositories), there is
no need to edit `.gitmodules`. Instead, tell git once to rewrite GitHub HTTPS
URLs to SSH for all your repositories:

```console
git config --global url."git@github.com:".insteadOf "https://github.com/"
```

The harness script `harness/run_submission.py` will attempt to build the submission itself, if it is not already built. If already built, it will use the same built code without re-building it, unless the code has changed. The build runs `make release` in `submission/niobium-client`, which builds the bundled OpenFHE, the fhetch library, and the client, and then compiles the submission against that OpenFHE install.

An example run is provided below.

```console
(virtualenv) $ python3 harness/run_submission.py -h
usage: run_submission.py [-h] [--num_runs NUM_RUNS] [--seed SEED] [--count_only] [--remote]
                         {0,1,2,3}

Run the fetch-by-similarity FHE benchmark.

positional arguments:
  {0,1,2,3}            Instance size (0-toy/1-small/2-medium/3-large)

options:
  -h, --help           show this help message and exit
  --num_runs NUM_RUNS  Number of times to run steps 4-9 (default: 1)
  --seed SEED          Random seed for dataset and query generation
  --count_only         Only count # of matches, do not return payloads
  --remote             Run example submission in remote backend mode
$
(virtualenv) $ python ./harness/run_submission.py 0 --seed 12345 --num_runs 3
-- FOUND PACKAGE OpenFHE
-- OpenFHE Version: 1.3.0
-- OpenFHE installed as shared libraries: ON
-- OpenFHE include files location: /usr/local/include/openfhe
-- OpenFHE lib files location: /usr/local/lib
-- OpenFHE Native Backend size: 64
-- Configuring done (0.0s)
-- Generating done (0.0s)
-- Build files have been written to: /home/ubuntu/fhe-benchmarking/fetch-by-similarity/submission/build
[  8%] Built target client_preprocess_dataset
[ 17%] Built target client_preprocess_query
[ 26%] Built target server_preprocess_dataset
[ 34%] Built target client_encode_encrypt_query
[ 43%] Built target client_decrypt_decode
[ 56%] Built target client_postprocess
[ 78%] Built target client_encode_encrypt_db
[ 82%] Built target server_encrypted_compute
[100%] Built target client_key_generation

[harness] Running submission for toy dataset
          returning matching payloads
15:09:51 [harness] 1: Dataset generation completed (elapsed: 0.1694s)
15:09:51 [harness] 2: Dataset preprocessing completed (elapsed: 0.0034s)
15:09:52 [harness] 3: Key Generation completed (elapsed: 0.1846s)
         [harness] Public and evaluation keys size: 30.3M
15:09:59 [harness] 4: Dataset encoding and encryption completed (elapsed: 7.7825s)
         [harness] Encrypted database size: 90.3M
15:09:59 [harness] 5: Encrypted dataset preprocessing completed (elapsed: 0.0069s)

         [harness] Run 1 of 3
15:10:00 [harness] 6: Query generation completed (elapsed: 0.171s)
15:10:00 [harness] 7: Query preprocessing completed (elapsed: 0.0031s)
15:10:00 [harness] 8: Query encryption completed (elapsed: 0.05s)
         [harness] Encrypted query size: 389.1K
15:10:00 [server] 0: Loading keys completed
15:10:50 [server] 1: Matrix-vector product completed (elapsed 50s)
15:11:01 [server] 2: Compare to threshold completed (elapsed 10s)
15:11:02 [server] 3: Running sums completed (elapsed 1s)
15:11:05 [server] 4: Output compression completed (elapsed 3s)
15:11:05 [harness] 9: Encrypted computation completed (elapsed: 65.2099s)
15:11:05 [harness] 10: Result decryption and postprocessing completed (elapsed: 0.032s)
         [harness] PASS (All 18 payload vectors match)
         [submission] Encrypted computation: 65s
         [submission] Total: 65s
[total latency] 73.6127s

         [harness] Run 2 of 3
15:11:05 [harness] 6: Query generation completed (elapsed: 0.5718s)
15:11:05 [harness] 7: Query preprocessing completed (elapsed: 0.0048s)
15:11:06 [harness] 8: Query encryption completed (elapsed: 0.1002s)
         [harness] Encrypted query size: 389.1K
15:11:06 [server] 0: Loading keys completed
15:11:46 [server] 1: Matrix-vector product completed (elapsed 40s)
15:11:49 [server] 2: Compare to threshold completed (elapsed 3s)
15:11:50 [server] 3: Running sums completed
15:11:53 [server] 4: Output compression completed (elapsed 3s)
15:11:53 [harness] 9: Encrypted computation completed (elapsed: 47.4987s)
15:11:53 [harness] 10: Result decryption and postprocessing completed (elapsed: 0.0219s)
         [harness] PASS (All 11 payload vectors match)
         [submission] Encrypted computation: 47s
         [submission] Total: 47s
[total latency] 56.3442s

         [harness] Run 3 of 3
15:11:54 [harness] 6: Query generation completed (elapsed: 0.4939s)
15:11:54 [harness] 7: Query preprocessing completed (elapsed: 0.0038s)
15:11:54 [harness] 8: Query encryption completed (elapsed: 0.1137s)
         [harness] Encrypted query size: 389.1K
15:11:54 [server] 0: Loading keys completed
15:12:36 [server] 1: Matrix-vector product completed (elapsed 42s)
15:12:40 [server] 2: Compare to threshold completed (elapsed 4s)
15:12:41 [server] 3: Running sums completed
15:12:45 [server] 4: Output compression completed (elapsed 3s)
15:12:45 [harness] 9: Encrypted computation completed (elapsed: 51.3689s)
15:12:45 [harness] 10: Result decryption and postprocessing completed (elapsed: 0.0207s)
         [harness] PASS (All 0 payload vectors match)
         [submission] Encrypted computation: 51s
         [submission] Total: 51s
[total latency] 60.1478s

All steps completed for the toy dataset!
```

After finishing the run, deactivate the virtual environment.
```console
deactivate
```

## Directory structure

```bash
[root] /
├─ README.md     # This file
├─ LICENSE.md    # Software license (Apache v2)
├─ harness/      # Scripts to drive the workload implementation
|   ├─ run_submission.py
|   ├─ cleartext_impl.py
|   ├─ verify_result.py
|   └─ [...]
├─ datasets/     # The harness scripts create and populate this directory
├─ io/           # This directory is used for client<->server communication
├─ measurements/ # Holds logs with performance numbers
├─ scripts/      # Helper scripts for dependencies and build system
└─ submission/   # This is where the workload implementation lives
    ├─ README.md   # Submission documentation (mandatory)
    ├─ LICENSE.md  # Optional software license (if different from Apache v2)
    ├─ docs/       # Optional: additional documentation
    └─ [...]
└─ submission_remote/  # This is where the remote-backend workload implementation lives
└─ [...]
```
Submitters must overwrite the contents of the `scripts` and `submissions`
subdirectories.

## Description of stages

A submitter should edit the `client_*` / `server_*` sources in `/submission`. 
Moreover, for the particular parameters related to a workload, the submitter can modify the params files.

The current stages are the following, targeted to a client-server scenario.
The order in which they are happening in `run_submission` assumes an initialization step which is 
database-dependent and run only once, and potentially multiple runs for multiple queries.
Each file can take as argument the test case size.


| Stage executables                | Description |
|----------------------------------|-------------|
| `server_get_params`              | (Optional) Get cryptographic context from a remote server.
| `client_key_generation`          | Generate all key material and cryptographic context at the client.           
| `server_upload_ek`               | (Optional) Upload evaluation key to a remote backend.  
| `client_preprocess_dataset`      | (Optional) Any in the clear computations the client wants to apply over the dataset/model.
| `client_preprocess_query`        | (Optional) Any in the clear computations the client wants to apply over the query/input.
| `client_encode_encrypt_db`       | (Optional) Plaintext encoding and encryption of the dataset/model at the client.
| `server_upload_db`               | (Optional) Upload encrypted DB to a remote backend.
| `client_encode_encrypt_query`    | Plaintext encoding and encryption of the query/input at the client.
| `server_preprocess_dataset`      | (Optional) Any in the clear or encrypted computations the server wants to apply over the dataset/model.
| `server_encrypted_compute`       | The computation the server applies to achieve the workload solution over encrypted daa.
| `client_decrypt_decode`          | Decryption and plaintext decoding of the result at the client.
| `client_postprocess`:            | Any in the clear computation that the client wants to apply on the decrypted result.


The outer python script measures the runtime of each stage.
The current stage separation structure requires reading and writing to files more times than minimally necessary.
For a more granular runtime measuring, which would account for the extra overhead described above, we encourage
submitters to separate and print in a log the individual times for reads/writes and computations inside each stage.

