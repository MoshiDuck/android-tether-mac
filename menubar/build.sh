#!/bin/sh
# Construit build/RNDISTether.app (icône de barre de menus), sans Xcode.
set -e
cd "$(dirname "$0")"
APP=build/RNDISTether.app
rm -rf build
mkdir -p "$APP/Contents/MacOS"
cp Info.plist "$APP/Contents/Info.plist"
swiftc -O -swift-version 5 RNDISTether.swift -o "$APP/Contents/MacOS/RNDISTether"
codesign --force --sign - "$APP" >/dev/null
echo "OK: $(pwd)/$APP"
