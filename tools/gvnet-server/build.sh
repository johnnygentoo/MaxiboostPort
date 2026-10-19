#!/usr/bin/env bash
# build.sh [PROTOBUF_BUILD]: builds gvnet-server alone (gpu/bbnet/server), for a PC that only hosts
# the matching server: no GPU library, no game. Needs g++ and protobuf: the system's (protoc and
# pkg-config's protobuf), or PROTOBUF_BUILD, a CMake build of protobuf with abseil fetched (such as
# shadnet-p2p's build directory: externals/protobuf/protoc-*, libprotobuf.a, _deps/absl-build),
# linked statically. Writes tools/gvnet-server/out/gvnet-server.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$here/out
mkdir -p "$out/gen"
proto=$root/gpu/bbnet/shadps4/shadnet
if [[ -n ${1:-} ]]; then
    build=$(cd "$1" && pwd)
    src=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$build/CMakeCache.txt")
    protoc=$(ls "$build"/externals/protobuf/protoc-* | head -1)
    abseil=$(find "$build/_deps/absl-build" -name 'libabsl*.a')
    utf8=$build/externals/protobuf/third_party/utf8_range
    "$protoc" --proto_path="$proto" --cpp_out="$out/gen" "$proto/shadnet.proto"
    # shellcheck disable=SC2086
    g++ -std=c++20 -O2 -o "$out/gvnet-server" "$root/gpu/bbnet/server/gvnet_server.cpp" \
        "$out/gen/shadnet.pb.cc" -I"$out/gen" -I"$src/externals/protobuf/src" \
        -I"$src/externals/protobuf/third_party/utf8_range" -I"$build/_deps/absl-src" \
        "$build/externals/protobuf/libprotobuf.a" -Wl,--start-group $abseil \
        "$utf8/libutf8_validity.a" "$utf8/libutf8_range.a" -Wl,--end-group -lpthread
else
    protoc --proto_path="$proto" --cpp_out="$out/gen" "$proto/shadnet.proto"
    # shellcheck disable=SC2046
    g++ -std=c++20 -O2 -o "$out/gvnet-server" "$root/gpu/bbnet/server/gvnet_server.cpp" \
        "$out/gen/shadnet.pb.cc" -I"$out/gen" $(pkg-config --cflags --libs protobuf) -lpthread
fi
echo "Built $out/gvnet-server"
