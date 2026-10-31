#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/tmp/swift-cache
xcrun swiftc -swift-version 5 -target arm64-apple-macosx11 -module-cache-path out/tmp/swift-cache \
    launcher/SaveArchive.swift launcher/SaveStore.swift tests/save_manager.swift -o out/tmp/save_manager_test
out/tmp/save_manager_test
