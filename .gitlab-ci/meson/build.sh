#!/usr/bin/env bash
# shellcheck disable=SC1003 # works for us now...
# shellcheck disable=SC2086 # we want word splitting

section_switch meson-cross-file "meson: cross file generate"

set -e
set -o xtrace

CROSS_FILE=/cross_file-"$CROSS".txt

export PATH=$PATH:$PWD/.gitlab-ci/build

touch native.file
printf > native.file "%s\n" \
  "[binaries]" \
  "c = 'compiler-wrapper-${CC:-gcc}.sh'" \
  "cpp = 'compiler-wrapper-${CXX:-g++}.sh'"

# We need to control the version of llvm-config we're using, so we'll
# tweak the cross file or generate a native file to do so.
if test -n "$LLVM_VERSION"; then
    LLVM_CONFIG="llvm-config-${LLVM_VERSION}"
    echo "llvm-config = '$(which "$LLVM_CONFIG")'" >> native.file
    if [ -n "$CROSS" ]; then
      sed -i -e '/\[binaries\]/a\' -e "llvm-config = '$(which "$LLVM_CONFIG")'" $CROSS_FILE
    fi
    $LLVM_CONFIG --version
fi

# cross-xfail-$CROSS, if it exists, contains a list of tests that are expected
# to fail for the $CROSS configuration, one per line. you can then mark those
# tests in their meson.build with:
#
# test(...,
#      should_fail: meson.get_external_property('xfail', '').contains(t),
#     )
#
# where t is the name of the test, and the '' is the string to search when
# not cross-compiling (which is empty, because for amd64 everything is
# expected to pass).
if [ -n "$CROSS" ]; then
    CROSS_XFAIL=.gitlab-ci/cross-xfail-"$CROSS"
    if [ -s "$CROSS_XFAIL" ]; then
        sed -i \
            -e '/\[properties\]/a\' \
            -e "xfail = '$(tr '\n' , < $CROSS_XFAIL)'" \
            "$CROSS_FILE"
    fi
fi

if [ -n "$HOST_BUILD_OPTIONS" ]; then
    section_switch meson-host-configure "meson: host configure"

    # Stash the PKG_CONFIG_LIBDIR so that we can use the base x86_64 image
    # libraries.
    tmp_pkg_config_libdir=$PKG_CONFIG_LIBDIR
    unset PKG_CONFIG_LIBDIR

    # Compile a host version for the few tools we need for a cross build (for
    # now just intel-clc)
    rm -rf _host_build
    meson setup _host_build \
          --native-file=native.file \
          -D prefix=/usr \
          -D libdir=lib \
          ${HOST_BUILD_OPTIONS}

    pushd _host_build

    section_switch meson-host-build "meson: host build"

    meson configure
    ninja
    ninja install
    popd

    # Restore PKG_CONFIG_LIBDIR
    if [ -n "$tmp_pkg_config_libdir" ]; then
        export PKG_CONFIG_LIBDIR=$tmp_pkg_config_libdir
    fi
fi

# Only use GNU time if available, not any shell built-in command
case $CI_JOB_NAME in
    # ASAN leak detection is incompatible with strace
    *-asan*)
        if test -f /usr/bin/time; then
            MESON_TEST_ARGS+=--wrapper=$PWD/.gitlab-ci/meson/time.sh
        fi
        Xvfb :0 -screen 0 1024x768x16 &
        export DISPLAY=:0.0
        ;;
    *)
        if test -f /usr/bin/time -a -f /usr/bin/strace; then
            MESON_TEST_ARGS+=--wrapper=$PWD/.gitlab-ci/meson/time-strace.sh
        fi
        ;;
esac

# LTO handling
case $CI_PIPELINE_SOURCE in
    schedule)
      # run builds with LTO only for nightly
      if [ "$CI_JOB_NAME" == "debian-ppc64el" ]; then
	      # /tmp/ccWlDCPV.s: Assembler messages:
	      # /tmp/ccWlDCPV.s:15250880: Error: operand out of range (0xfffffffffdd4e688 is not between 0xfffffffffe000000 and 0x1fffffc)
	      LTO=false
      # enable one by one for now
      elif [ "$CI_JOB_NAME" == "fedora-release" ] || [ "$CI_JOB_NAME" == "debian-build-testing" ]; then
	      LTO=true
      else
	      LTO=false
      fi
      ;;
    *)
      # run Fedora with LTO in pre-merge for now
      if [ "$CI_JOB_NAME" == "fedora-release" ]; then
	      LTO=true
      else
	      LTO=false
      fi
      ;;
esac

if [ "$LTO" == "true" ]; then
    MAX_LD=2
else
    MAX_LD=${FDO_CI_CONCURRENT:-4}
fi

section_switch meson-configure "meson: configure"

rm -rf _build
meson setup _build \
      --native-file=native.file \
      --wrap-mode=nofallback \
      --force-fallback-for perfetto,syn,paste,pest,pest_derive,pest_generator,pest_meta,roxmltree,indexmap \
      ${CROSS+--cross "$CROSS_FILE"} \
      -D prefix=$PWD/install \
      -D libdir=lib \
      -D buildtype=${BUILDTYPE:?} \
      -D build-tests=true \
      -D c_args="$(echo -n $C_ARGS)" \
      -D c_link_args="$(echo -n $C_LINK_ARGS)" \
      -D cpp_args="$(echo -n $CPP_ARGS)" \
      -D cpp_link_args="$(echo -n $CPP_LINK_ARGS)" \
      -D enable-glcpp-tests=false \
      -D libunwind=${UNWIND} \
      ${DRI_LOADERS} \
      ${GALLIUM_ST} \
      -D gallium-opencl=disabled \
      -D gallium-drivers=${GALLIUM_DRIVERS:-[]} \
      -D vulkan-drivers=${VULKAN_DRIVERS:-[]} \
      -D video-codecs=all \
      -D werror=true \
      -D b_lto=${LTO} \
      -D backend_max_links=${MAX_LD} \
      ${EXTRA_OPTION}
cd _build
meson configure

uncollapsed_section_switch meson-build "meson: build"

if command -V mold &> /dev/null ; then
    mold --run ninja
else
    ninja
fi


uncollapsed_section_switch meson-test "meson: test"
LC_ALL=C.UTF-8 meson test --num-processes "${FDO_CI_CONCURRENT:-4}" --print-errorlogs ${MESON_TEST_ARGS}
section_switch meson-install "meson: install"
if command -V mold &> /dev/null ; then
    mold --run ninja install
else
    ninja install
fi
cd ..
section_end meson-install

