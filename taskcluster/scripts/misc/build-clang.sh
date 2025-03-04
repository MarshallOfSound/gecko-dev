#!/bin/bash
set -x -e -v

# This script is for building clang.

ORIGPWD="$PWD"
CONFIGS=$(for c; do echo -n " -c $GECKO_PATH/$c"; done)

cd $GECKO_PATH

if [ -n "$TOOLTOOL_MANIFEST" ]; then
  . taskcluster/scripts/misc/tooltool-download.sh
fi

if [ -d "$MOZ_FETCHES_DIR/binutils/bin" ]; then
  export PATH="$MOZ_FETCHES_DIR/binutils/bin:$PATH"
fi

# Make the installed compiler-rt(s) available to clang.
UPLOAD_DIR= taskcluster/scripts/misc/repack-clang.sh

case "$CONFIGS" in
*macosx64*)
  # these variables are used in build-clang.py
  export CROSS_CCTOOLS_PATH=$MOZ_FETCHES_DIR/cctools
  export CROSS_SYSROOT=$(ls -d $MOZ_FETCHES_DIR/MacOSX1*.sdk)
  export PATH=$PATH:$CROSS_CCTOOLS_PATH/bin
  ;;
*win64*)
  export UPLOAD_DIR=$ORIGPWD/public/build
  # Set up all the Visual Studio paths.
  . taskcluster/scripts/misc/vs-setup.sh

  # LLVM_ENABLE_DIA_SDK is set if the directory "$ENV{VSINSTALLDIR}DIA SDK"
  # exists.
  export VSINSTALLDIR="${VSPATH}/"

  export PATH="$(cd $MOZ_FETCHES_DIR/cmake && pwd)/bin:${PATH}"
  export PATH="$(cd $MOZ_FETCHES_DIR/ninja && pwd)/bin:${PATH}"
  ;;
*linux64*|*android*)
  ;;
*)
  echo Cannot figure out build configuration for $CONFIGS
  exit 1
  ;;
esac

# gets a bit too verbose here
set +x

cd $MOZ_FETCHES_DIR/llvm-project
python3 $GECKO_PATH/build/build-clang/build-clang.py $CONFIGS

set -x

if [ -f clang*.tar.zst ]; then
    # Put a tarball in the artifacts dir
    mkdir -p $UPLOAD_DIR
    cp clang*.tar.zst $UPLOAD_DIR
fi

. $GECKO_PATH/taskcluster/scripts/misc/vs-cleanup.sh
