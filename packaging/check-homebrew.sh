#!/bin/bash
# An optional candidate archive replaces only the stable formula's source stanza.
# Omit it to verify the actual tagged-source formula before publishing a release.
set -euo pipefail
source_directory=${1:?source directory required}
check_directory=${2:?check directory required}
archive=${3:-}
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1

if brew list --formula | /usr/bin/grep -qx shrink; then
  echo "Refusing to replace an existing Homebrew shrink installation" >&2
  exit 1
fi
tap="alifrahman2023/shrink-check-$$"
formula="$tap/shrink"
brew tap-new --no-git "$tap"
cleanup() {
  brew uninstall --force "$formula" >/dev/null 2>&1 || true
  brew untap "$tap" >/dev/null 2>&1 || true
}
trap cleanup EXIT
tap_directory=$(brew --repository "$tap")
mkdir -p "$check_directory"
cp "$source_directory/Formula/shrink.rb" "$tap_directory/Formula/shrink.rb"
if [ -n "$archive" ]; then
  # File URI and checksum point at the exact archive used by manual installation.
  ruby -ruri -rdigest -e '
    formula, archive = ARGV
    uri = "file://" + URI::DEFAULT_PARSER.escape(File.expand_path(archive))
    text = File.read(formula).sub(/^  url .*$/, "  url #{uri.dump}\n  sha256 #{Digest::SHA256.file(archive).hexdigest.dump}")
    File.write(formula, text)
  ' "$tap_directory/Formula/shrink.rb" "$archive"
fi
cp "$tap_directory/Formula/shrink.rb" "$check_directory/verified-formula.rb"
brew install --build-from-source --verbose "$formula"
brew test "$formula"
prefix=$(brew --prefix "$formula")
cmake "-DSHRINK_EXECUTABLE=$prefix/bin/shrink" \
  "-DCHECK_DIRECTORY=$check_directory/outside-build" \
  -P "$source_directory/tests/check_information.cmake"
for demo in duplicates width; do
  cmake "-DDEMO=$demo" "-DSHRINK_EXECUTABLE=$prefix/bin/shrink" \
    "-DDEMO_DIRECTORY=$source_directory/examples/$demo" \
    "-DSOLUTION=$source_directory/build/debug/demo_${demo}_solution" \
    "-DREFERENCE=$source_directory/build/debug/demo_${demo}_reference" \
    "-DCHECK_DIRECTORY=$check_directory/outside-build/$demo" \
    -P "$source_directory/tests/check_demonstration.cmake"
done
