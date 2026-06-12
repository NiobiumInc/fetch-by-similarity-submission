#!/usr/bin/env bash

# Copyright (c) 2025 HomomorphicEncryption.org
# All rights reserved.
#
# This software is licensed under the terms of the Apache v2 License.
# See the LICENSE.md file for details.

# ------------------------------------------------------------
# Usage: ./scripts/build_task.sh <submission-directory>
# Compiles the files in the source directory.
# ------------------------------------------------------------
set -euo pipefail
ROOT="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )"
TASK_DIR="$( cd -- "$1" &> /dev/null && pwd )"
OPENFHE_PREFIX_RAW="${2:-$ROOT/third_party/openfhe}"
OPENFHE_PREFIX="$( cd -- "$OPENFHE_PREFIX_RAW" &> /dev/null && pwd )"
BUILD="$TASK_DIR/build"

# OpenFHE install prefix can be passed as the second argument. Defaults to
# $ROOT/third_party/openfhe.
#
# We pin OpenFHE_DIR explicitly and disable the user package registry so
# that stale OpenFHE entries under ~/.cmake/packages/OpenFHE can't hijack
# find_package(OpenFHE).
cmake -S "$TASK_DIR" -B "$BUILD" \
      -DCMAKE_PREFIX_PATH="$OPENFHE_PREFIX" \
      -DOpenFHE_DIR="$OPENFHE_PREFIX/lib/OpenFHE" \
      -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
cd "$TASK_DIR/build"
make -j
