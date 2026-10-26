#!/usr/bin/env bash
# ZyDeck's copies that must agree, checked against the same cases (cases.json): the C++ the hub and ZyDeck Connect
# share (src/zydek/zydeckshared.h), the phone's Python and the Mixxx mapping's JavaScript.
#   bash tools/zydeck-shared-tests/run.sh <a ZyDeck Connect build folder made with -DZC_TESTS=ON>
# (the C++ check is built with Connect: cmake --build <folder> --target zydeck-shared-tests)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
build="${1:?the ZyDeck Connect build folder (configured with -DZC_TESTS=ON)}"
mapping="$here/../../res/controllers/Zydek-Tablet.script.js"
status=0
cmake --build "$build" --config Release --target zydeck-shared-tests >/dev/null || { echo "C++: didn't build"; status=1; }
exe="$(find "$build" -name 'zydeck-shared-tests' -type f -perm -u+x -o -name 'zydeck-shared-tests.exe' -type f | head -1)"
if [ -n "$exe" ]; then "$exe" "$here/cases.json" || status=1; fi
python3 -I "$here/test_python.py" "$here/cases.json" || status=1
jsc=/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc
if command -v node >/dev/null; then
  node "$here/test_mapping.js" "$here/cases.json" "$mapping" || status=1
elif [ -x "$jsc" ]; then
  "$jsc" "$here/test_mapping.js" -- "$here/cases.json" "$mapping" || status=1
else
  echo "JavaScript: no node or jsc here (not checked)"
fi
exit $status
