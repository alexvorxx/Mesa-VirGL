#!/usr/bin/env bash
# The relative paths in this file only become valid at runtime.
# shellcheck disable=SC1091

set -e
set -o xtrace

export DEBIAN_FRONTEND=noninteractive

# Ephemeral packages (installed for this script and removed again at the end)
EPHEMERAL=(
   build-essential:native
   ccache
   cmake
   config-package-dev
   debhelper-compat
   dpkg-dev
   ninja-build
   unzip
)

DEPS=(
    iproute2
)
apt-get install -y --no-remove --no-install-recommends \
      "${DEPS[@]}" "${EPHEMERAL[@]}"

############### Building ...

. .gitlab-ci/container/container_pre_build.sh

############### Downloading NDK for native builds for the guest ...

# Fetch the NDK and extract just the toolchain we want.
ndk=$ANDROID_NDK
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  -o "$ndk.zip" "https://dl.google.com/android/repository/$ndk-linux.zip"
unzip -d / "$ndk.zip"
rm "$ndk.zip"

############### Build dEQP runner

export ANDROID_NDK_HOME=/$ndk
export RUST_TARGET=x86_64-linux-android
. .gitlab-ci/container/build-rust.sh
. .gitlab-ci/container/build-deqp-runner.sh

rm -rf /root/.cargo
rm -rf /root/.rustup

############### Build dEQP GL

DEQP_API=GL \
DEQP_TARGET="android" \
EXTRA_CMAKE_ARGS="-DDEQP_TARGET_TOOLCHAIN=ndk-modern -DANDROID_NDK_PATH=/$ndk -DANDROID_ABI=x86_64 -DDE_ANDROID_API=28" \
. .gitlab-ci/container/build-deqp.sh

DEQP_API=GLES \
DEQP_TARGET="android" \
EXTRA_CMAKE_ARGS="-DDEQP_TARGET_TOOLCHAIN=ndk-modern -DANDROID_NDK_PATH=/$ndk -DANDROID_ABI=x86_64 -DDE_ANDROID_API=28" \
. .gitlab-ci/container/build-deqp.sh

############### Downloading Cuttlefish resources ...

CUTTLEFISH_VERSION=9082637   # Chosen from https://ci.android.com/builds/branches/aosp-master/grid?

mkdir /cuttlefish
pushd /cuttlefish

curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  -o aosp_cf_x86_64_phone-img-$CUTTLEFISH_VERSION.zip https://ci.android.com/builds/submitted/$CUTTLEFISH_VERSION/aosp_cf_x86_64_phone-userdebug/latest/raw/aosp_cf_x86_64_phone-img-$CUTTLEFISH_VERSION.zip
unzip aosp_cf_x86_64_phone-img-$CUTTLEFISH_VERSION.zip
rm aosp_cf_x86_64_phone-img-$CUTTLEFISH_VERSION.zip
ls -lhS ./*

curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  https://ci.android.com/builds/submitted/$CUTTLEFISH_VERSION/aosp_cf_x86_64_phone-userdebug/latest/raw/cvd-host_package.tar.gz | tar -xzvf-

popd

############### Building and installing Debian package ...

ANDROID_CUTTLEFISH_VERSION=f6494d9fbeaa9974b56923e3029909e5d5f440dd

mkdir android-cuttlefish
pushd android-cuttlefish
git init
git remote add origin https://github.com/google/android-cuttlefish.git
git fetch --depth 1 origin "$ANDROID_CUTTLEFISH_VERSION"
git checkout FETCH_HEAD

pushd base
dpkg-buildpackage -uc -us
popd

apt-get install -y ./cuttlefish-base_*.deb

popd
rm -rf android-cuttlefish

addgroup --system kvm
usermod -a -G kvm,cvdnetwork root

############### Uninstall the build software

rm -rf "/${ndk:?}"

apt-get purge -y "${EPHEMERAL[@]}"

. .gitlab-ci/container/container_post_build.sh

. .gitlab-ci/container/strip-rootfs.sh
