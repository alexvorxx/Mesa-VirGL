#!/usr/bin/env bash

#
# Copyright (C) 2022 Collabora Limited
# Author: Guilherme Gallo <guilherme.gallo@collabora.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# This script runs unit/integration tests related with LAVA CI tools

# SPDX-License-Identifier: MIT
# © Collabora Limited
# Author: Guilherme Gallo <guilherme.gallo@collabora.com>

# This script runs unit/integration tests related with LAVA CI tools
# shellcheck disable=SC1091 # The relative paths in this file only become valid at runtime.


set -ex

# Use this script in a python virtualenv for isolation
python3 -m venv .venv
. .venv/bin/activate

python3 -m pip install -r ${CI_PROJECT_DIR}/.gitlab-ci/lava/requirements-test.txt

python3 -m pip install --break-system-packages -r "${CI_PROJECT_DIR}/.gitlab-ci/lava/requirements-test.txt"


TEST_DIR=${CI_PROJECT_DIR}/.gitlab-ci/tests

PYTHONPATH="${TEST_DIR}:${PYTHONPATH}" python3 -m \
    pytest "${TEST_DIR}" \
            -W ignore::DeprecationWarning \
            --junitxml=artifacts/ci_scripts_report.xml \
            -m 'not slow'
