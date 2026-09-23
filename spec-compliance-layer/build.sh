#!/usr/bin/env bash
# Build VK_LAYER_steamvr_spec_compliance and its standalone fence test.
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O2 -fPIC -Wall -Wextra"

INC_DIR=/usr/include
if [[ -n "${VULKAN_SDK:-}" && -f "${VULKAN_SDK}/include/vulkan/vk_layer.h" ]]; then
  INC_DIR="${VULKAN_SDK}/include"
fi

$CXX $CXXFLAGS -I"$INC_DIR" -shared -o libVkLayer_steamvr_spec_compliance.so steamvr_spec_compliance.cpp
echo "built $(pwd)/libVkLayer_steamvr_spec_compliance.so"

$CXX $CXXFLAGS -I"$INC_DIR" -o test_reset_fences test_reset_fences.cpp -lvulkan
echo "built $(pwd)/test_reset_fences"
