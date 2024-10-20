#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# DEBIAN_TEST_ANDROID_TAG
# DEBIAN_TEST_GL_TAG
# DEBIAN_TEST_VK_TAG
# KERNEL_ROOTFS_TAG

set -uex -o pipefail

# See `deqp_build_targets` below for which release is used to produce which
# binary. Unless this comment has bitrotten:
# - the VK release produces `deqp-vk`,
# - the GL release produces `glcts`, and
# - the GLES release produces `deqp-gles*` and `deqp-egl`

DEQP_VK_VERSION=1.3.9.2
DEQP_GL_VERSION=4.6.5.0
DEQP_GLES_VERSION=3.2.11.0

# Patches to VulkanCTS may come from commits in their repo (listed in
# cts_commits_to_backport) or patch files stored in our repo (in the patch
# directory `$OLDPWD/.gitlab-ci/container/patches/` listed in cts_patch_files).
# Both list variables would have comments explaining the reasons behind the
# patches.

# shellcheck disable=SC2034
vk_cts_commits_to_backport=(
    # Fix sync issues in copy and blit tests
    24f214d80a866c04745f6c3b5027a4b540568598

    # Don't access out-of-bounds per-vertex attributes
    6e36996fdf38b91bd08b97c1aa8ddb7601e43aae

    # Remove some of FDM + DRLR tests
    d712381852e7f90cdd0a0bac5e43ac10d9bb5241
)

# shellcheck disable=SC2034
vk_cts_patch_files=(
)

if [ "${DEQP_TARGET}" = 'android' ]; then
  vk_cts_patch_files+=(
    build-deqp-vk_Allow-running-on-Android-from-the-command-line.patch
    build-deqp-vk_Android-prints-to-stdout-instead-of-logcat.patch
  )
fi

# shellcheck disable=SC2034
gl_cts_commits_to_backport=(
)

# shellcheck disable=SC2034
gl_cts_patch_files=(
)

if [ "${DEQP_TARGET}" = 'android' ]; then
  gl_cts_patch_files+=(
    build-deqp-gl_Allow-running-on-Android-from-the-command-line.patch
    build-deqp-gl_Android-prints-to-stdout-instead-of-logcat.patch
  )
fi

# shellcheck disable=SC2034
# GLES builds also EGL
gles_cts_commits_to_backport=(
)

# shellcheck disable=SC2034
gles_cts_patch_files=(
)

if [ "${DEQP_TARGET}" = 'android' ]; then
  gles_cts_patch_files+=(
    build-deqp-gles_Allow-running-on-Android-from-the-command-line.patch
    build-deqp-gles_Android-prints-to-stdout-instead-of-logcat.patch
  )
fi


### Careful editing anything below this line


git config --global user.email "mesa@example.com"
git config --global user.name "Mesa CI"

# shellcheck disable=SC2153
case "${DEQP_API}" in
  VK) DEQP_VERSION="vulkan-cts-$DEQP_VK_VERSION";;
  GL) DEQP_VERSION="opengl-cts-$DEQP_GL_VERSION";;
  GLES) DEQP_VERSION="opengl-es-cts-$DEQP_GLES_VERSION";;
esac

git clone \
    https://github.com/KhronosGroup/VK-GL-CTS.git \
    -b $DEQP_VERSION \
    --depth 1 \
    /VK-GL-CTS
pushd /VK-GL-CTS

mkdir -p /deqp

# shellcheck disable=SC2153
deqp_api=${DEQP_API,,}

cts_commits_to_backport="${deqp_api}_cts_commits_to_backport[@]"
for commit in "${!cts_commits_to_backport}"
do
  PATCH_URL="https://github.com/KhronosGroup/VK-GL-CTS/commit/$commit.patch"
  echo "Apply patch to ${DEQP_API} CTS from $PATCH_URL"
  curl -L --retry 4 -f --retry-all-errors --retry-delay 60 $PATCH_URL | \
    GIT_COMMITTER_DATE=$(date -d@0) git am -
done

cts_patch_files="${deqp_api}_cts_patch_files[@]"
for patch in "${!cts_patch_files}"
do
  echo "Apply patch to ${DEQP_API} CTS from $patch"
  GIT_COMMITTER_DATE=$(date -d@0) git am < $OLDPWD/.gitlab-ci/container/patches/$patch
done

{
  echo "dEQP base version $DEQP_VERSION"
  echo "The following local patches are applied on top:"
  git log --reverse --oneline $DEQP_VERSION.. --format='- %s'
} > /deqp/version-$deqp_api

# --insecure is due to SSL cert failures hitting sourceforge for zlib and
# libpng (sigh).  The archives get their checksums checked anyway, and git
# always goes through ssh or https.
python3 external/fetch_sources.py --insecure

# Save the testlog stylesheets:
cp doc/testlog-stylesheet/testlog.{css,xsl} /deqp
popd

pushd /deqp

if [ "${DEQP_API}" = 'GLES' ]; then
  if [ "${DEQP_TARGET}" = 'android' ]; then
    cmake -S /VK-GL-CTS -B . -G Ninja \
        -DDEQP_TARGET=android \
        -DCMAKE_BUILD_TYPE=Release \
        ${EXTRA_CMAKE_ARGS:-}
    mold --run ninja modules/egl/deqp-egl
    mv /deqp/modules/egl/deqp-egl /deqp/modules/egl/deqp-egl-android
  else
    # When including EGL/X11 testing, do that build first and save off its
    # deqp-egl binary.
    cmake -S /VK-GL-CTS -B . -G Ninja \
        -DDEQP_TARGET=x11_egl_glx \
        -DCMAKE_BUILD_TYPE=Release \
        ${EXTRA_CMAKE_ARGS:-}
    mold --run ninja modules/egl/deqp-egl
    mv /deqp/modules/egl/deqp-egl /deqp/modules/egl/deqp-egl-x11

    cmake -S /VK-GL-CTS -B . -G Ninja \
        -DDEQP_TARGET=wayland \
        -DCMAKE_BUILD_TYPE=Release \
        ${EXTRA_CMAKE_ARGS:-}
    mold --run ninja modules/egl/deqp-egl
    mv /deqp/modules/egl/deqp-egl /deqp/modules/egl/deqp-egl-wayland
  fi
fi

cmake -S /VK-GL-CTS -B . -G Ninja \
      -DDEQP_TARGET=${DEQP_TARGET} \
      -DCMAKE_BUILD_TYPE=Release \
      ${EXTRA_CMAKE_ARGS:-}

# Make sure `default` doesn't silently stop detecting one of the platforms we care about
if [ "${DEQP_TARGET}" = 'default' ]; then
  grep -q DEQP_SUPPORT_WAYLAND=1 build.ninja
  grep -q DEQP_SUPPORT_X11=1 build.ninja
  grep -q DEQP_SUPPORT_XCB=1 build.ninja
fi

deqp_build_targets=()
case "${DEQP_API}" in
  VK)
    deqp_build_targets+=(deqp-vk)
    ;;
  GL)
    deqp_build_targets+=(glcts)
    ;;
  GLES)
    deqp_build_targets+=(deqp-gles{2,3,31})
    # deqp-egl also comes from this build, but it is handled separately above.
    ;;
esac
if [ "${DEQP_TARGET}" != 'android' ]; then
  deqp_build_targets+=(testlog-to-xml)
  deqp_build_targets+=(testlog-to-csv)
  deqp_build_targets+=(testlog-to-junit)
fi

mold --run ninja "${deqp_build_targets[@]}"

if [ "${DEQP_TARGET}" != 'android' ]; then
    # Copy out the mustpass lists we want.
    mkdir -p /deqp/mustpass

    if [ "${DEQP_API}" = 'VK' ]; then
        for mustpass in $(< /VK-GL-CTS/external/vulkancts/mustpass/main/vk-default.txt) ; do
            cat /VK-GL-CTS/external/vulkancts/mustpass/main/$mustpass \
                >> /deqp/mustpass/vk-main.txt
        done
    fi

    if [ "${DEQP_API}" = 'GL' ]; then
        cp \
            /VK-GL-CTS/external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass/main/*-main.txt \
            /deqp/mustpass/
        cp \
            /VK-GL-CTS/external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass_single/main/*-single.txt \
            /deqp/mustpass/
    fi

    if [ "${DEQP_API}" = 'GLES' ]; then
        cp \
            /VK-GL-CTS/external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main/*.txt \
            /deqp/mustpass/
        cp \
            /VK-GL-CTS/external/openglcts/data/gl_cts/data/mustpass/egl/aosp_mustpass/main/egl-main.txt \
            /deqp/mustpass/
        cp \
            /VK-GL-CTS/external/openglcts/data/gl_cts/data/mustpass/gles/khronos_mustpass/main/*-main.txt \
            /deqp/mustpass/
    fi

    # Save *some* executor utils, but otherwise strip things down
    # to reduct deqp build size:
    mkdir /deqp/executor.save
    cp /deqp/executor/testlog-to-* /deqp/executor.save
    rm -rf /deqp/executor
    mv /deqp/executor.save /deqp/executor
fi

# Compress the caselists, since Vulkan's in particular are gigantic; higher
# compression levels provide no real measurable benefit.
zstd -1 --rm /deqp/mustpass/*.txt

# Remove other mustpass files, since we saved off the ones we wanted to conventient locations above.
rm -rf /deqp/external/**/mustpass/
rm -rf /deqp/external/vulkancts/modules/vulkan/vk-main*
rm -rf /deqp/external/vulkancts/modules/vulkan/vk-default

rm -rf /deqp/external/openglcts/modules/cts-runner
rm -rf /deqp/modules/internal
rm -rf /deqp/execserver
rm -rf /deqp/framework
find . -depth \( -iname '*cmake*' -o -name '*ninja*' -o -name '*.o' -o -name '*.a' \) -exec rm -rf {} \;
if [ "${DEQP_API}" = 'VK' ]; then
  ${STRIP_CMD:-strip} external/vulkancts/modules/vulkan/deqp-vk
fi
if [ "${DEQP_API}" = 'GL' ]; then
  ${STRIP_CMD:-strip} external/openglcts/modules/glcts
fi
if [ "${DEQP_API}" = 'GLES' ]; then
  ${STRIP_CMD:-strip} modules/*/deqp-*
fi
du -sh ./*
rm -rf /VK-GL-CTS
popd
