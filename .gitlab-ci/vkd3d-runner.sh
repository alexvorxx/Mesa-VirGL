#!/usr/bin/env bash

set -ex

comma_separated() {
  local IFS=,
  echo "$*"
}

if [[ -z "$VK_DRIVER" ]]; then
    exit 1
fi

INSTALL=$(realpath -s "$PWD"/install)

RESULTS=$(realpath -s "$PWD"/results)

# Make sure the results folder exists
mkdir -p "$RESULTS"

# Set up the driver environment.
# Modifiying here directly LD_LIBRARY_PATH may cause problems when
# using a command wrapper. Hence, we will just set it when running the
# command.
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$INSTALL/lib/:/vkd3d-proton-tests/x64/"


# Set the Vulkan driver to use.
ARCH=$(uname -m)
export VK_DRIVER_FILES="$INSTALL/share/vulkan/icd.d/${VK_DRIVER}_icd.$ARCH.json"

# Set environment for Wine.
export WINEDEBUG="-all"
export WINEPREFIX="/vkd3d-proton-wine64"
export WINEESYNC=1

if [ -f "$INSTALL/$GPU_VERSION-vkd3d-skips.txt" ]; then
  mapfile -t skips < <(grep -vE '^#|^$' "$INSTALL/$GPU_VERSION-vkd3d-skips.txt")
  VKD3D_TEST_EXCLUDE=$(comma_separated "${skips[@]}")
  export VKD3D_TEST_EXCLUDE
fi

# Sanity check to ensure that our environment is sufficient to make our tests
# run against the Mesa built by CI, rather than any installed distro version.
MESA_VERSION=$(cat "$INSTALL/VERSION")
if ! vulkaninfo | grep driverInfo | tee /tmp/version.txt | grep -F "Mesa $MESA_VERSION"; then
    printf "%s\n" "Found $(cat /tmp/version.txt), expected $MESA_VERSION"
    exit 1
fi

# Gather the list expected failures
EXPECTATIONFILE="$RESULTS/$GPU_VERSION-vkd3d-fails.txt"
if [ -f "$INSTALL/$GPU_VERSION-vkd3d-fails.txt" ]; then
    cp "$INSTALL/$GPU_VERSION-vkd3d-fails.txt" "$EXPECTATIONFILE"
else
    printf "%s\n" "$GPU_VERSION-vkd3d-fails.txt not found, assuming a \"no failures\" baseline."
    touch "$EXPECTATIONFILE"
fi

if [ -f "$INSTALL/$GPU_VERSION-vkd3d-flakes.txt" ]; then
  mapfile -t flakes < <(grep -vE '^#|^$' "$INSTALL/$GPU_VERSION-vkd3d-flakes.txt")
else
  flakes=()
fi

# Some sanity checks before we start
mapfile -t flakes_dups < <(
  [ ${#flakes[@]} -eq 0 ] ||
  printf '%s\n' "${flakes[@]}" | sort | uniq -d
)
if [ ${#flakes_dups[@]} -gt 0 ]; then
  echo >&2 'Duplicate flakes lines:'
  printf >&2 '  %s\n' "${flakes_dups[@]}"
  exit 1
fi

flakes_in_baseline=()
for flake in "${flakes[@]}"; do
  if grep -qF "$flake" "$EXPECTATIONFILE"; then
    flakes_in_baseline+=("$flake")
  fi
done
if [ ${#flakes_in_baseline[@]} -gt 0 ]; then
  echo >&2 "Flakes found in $EXPECTATIONFILE:"
  printf >&2 '  %s\n' "${flakes_in_baseline[@]}"
  exit 1
fi

printf "%s\n" "Running vkd3d-proton testsuite..."

if ! /vkd3d-proton-tests/x64/bin/d3d12 &> "$RESULTS/vkd3d-proton-log.txt"; then
    # Check if the executable finished (ie. no segfault).
    if ! grep "tests executed" "$RESULTS/vkd3d-proton-log.txt" > /dev/null; then
        error "Failed, see ${ARTIFACTS_BASE_URL}/results/vkd3d-proton-log.txt"
        exit 1
    fi

    # Collect all the failures
    RESULTSFILE="$RESULTS/$GPU_VERSION.txt"
    # Sometimes, some lines are randomly (?) prefixed with one of these:
    # 058f:info:vkd3d_pipeline_library_disk_cache_initial_setup:
    # 058f:info:vkd3d_pipeline_library_disk_cache_merge:
    # 058f:info:vkd3d_pipeline_library_disk_thread_main:
    # As a result, we have to specifically start the grep at the test name.
    if ! grep -oE "test_\w+:.*Test failed.*$" "$RESULTS"/vkd3d-proton-log.txt > "$RESULTSFILE"; then
      error "Failed to get the list of failing tests, see ${ARTIFACTS_BASE_URL}/results/vkd3d-proton-log.txt"
      exit 1
    fi

    # Ignore flakes when comparing
    STABLERESULTSFILE="$RESULTS/$GPU_VERSION-results-minus-flakes.txt"
    cp "$RESULTSFILE" "$STABLERESULTSFILE"
    for flake in "${flakes[@]}"; do
      grep -vF "$flake" "$STABLERESULTSFILE" > tmp && mv tmp "$STABLERESULTSFILE"
    done

    # Make sure that the failures found in this run match the current expectation
    if ! diff --color=always -u "$EXPECTATIONFILE" "$STABLERESULTSFILE"; then
        error "Changes found, see ${ARTIFACTS_BASE_URL}/results/vkd3d-proton-log.txt"
        exit 1
    fi

    # Print list of flakes seen this time
    flakes_seen=()
    for flake in "${flakes[@]}"; do
      if grep -qF "$flake" "$RESULTSFILE"; then
        flakes_seen+=("$flake")
      fi
    done
    if [ ${#flakes_seen[@]} -gt 0 ]; then
      # Keep this string and output format in line with the corresponding
      # deqp-runner message
      echo >&2 'Some known flakes found:'
      printf >&2 '  %s\n' "${flakes_seen[@]}"
    fi
fi

exit 0
