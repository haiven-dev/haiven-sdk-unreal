#!/usr/bin/env bash
# Compile and run the parts of the Unreal binding that do not need Unreal.
#
# Which is the protocol and session core, deliberately: see the headers for why
# they carry no engine types. The engine glue under Private/ is NOT built here
# and is not claimed to be — `README.md` says what remains unverified.
#
# Skips loudly rather than passing quietly without a compiler, like every other
# toolchain-dependent check in this repository.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CXX="${CXX:-$(command -v g++ || command -v clang++ || true)}"

if [ -z "$CXX" ]; then
	echo "skipping C++: no g++ or clang++"
	# Hosted source jobs set this so a missing compiler is a failed check,
	# not a quiet skip. Local runs without a compiler still skip.
	if [ "${HAIVEN_REQUIRE_CXX:-}" = "1" ]; then
		exit 1
	fi
	exit 0
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# -Werror because the whole point is that this file is under a compiler at all;
# a warning nobody reads is the state it was written to avoid.
"$CXX" -std=c++17 -Wall -Wextra -Werror -I "$HERE" -o "$OUT/protocol_test" "$HERE/protocol_test.cpp"

# Windows compilers write protocol_test.exe next to the requested name.
if [ -f "$OUT/protocol_test.exe" ]; then
	"$OUT/protocol_test.exe"
else
	"$OUT/protocol_test"
fi
