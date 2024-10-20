#!/bin/sh

#!/usr/bin/env bash

set -ex

if [ -z "$VK_DRIVER" ]; then
   echo 'VK_DRIVER must be to something like "radeon" or "intel" for the test run'
   exit 1
fi


INSTALL=`pwd`/install

# Set up the driver environment.
export LD_LIBRARY_PATH=`pwd`/install/lib/
export VK_ICD_FILENAMES=`pwd`/install/share/vulkan/icd.d/"$VK_DRIVER"_icd.x86_64.json

# To store Fossilize logs on failure.
RESULTS=`pwd`/results

INSTALL=$PWD/install

# Set up the driver environment.
export LD_LIBRARY_PATH="$INSTALL/lib/"
export VK_DRIVER_FILES="$INSTALL/share/vulkan/icd.d/${VK_DRIVER}_icd.x86_64.json"

# To store Fossilize logs on failure.
RESULTS="$PWD/results"

mkdir -p results

"$INSTALL/fossils/fossils.sh" "$INSTALL/fossils.yml" "$RESULTS"
