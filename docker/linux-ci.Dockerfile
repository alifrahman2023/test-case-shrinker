# The Linux half of CI, runnable on a Mac: GCC 13 and clang-format 18 on Ubuntu 24.04, the
# same toolchain as the GitHub ubuntu-latest runner. GCC has warnings clang does not, such as
# -Wdangling-reference, so a clean local clang build is not evidence that CI will pass.
#
# Used by scripts/check-linux.sh. Not the demo image the plan describes for M8.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++ cmake ninja-build git ca-certificates clang-format rsync \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
