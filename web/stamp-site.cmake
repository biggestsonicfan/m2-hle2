# Stamp the build's version into the copied page.
#
#   cmake -DSITE=<dir> -DVERSION=<version> -P web/stamp-site.cmake
#
# index.html names every other file with ?v=<version>, and the page script does
# the same for the two files it loads itself (m2hle.wasm, the audio worklet).
# GitHub Pages serves everything with a 10 minute cache lifetime and no way to
# change it, so without this a visitor arriving just after a deploy can get the
# new m2hle.js against the old page script, or the old .wasm against the new .js
# -- and those are built as a pair. With it, one index.html always asks for one
# consistent set.
#
# The stylesheet names its fonts (fonts/*.woff2) the same way, and index.html
# preloads them under those very URLs, so both files are stamped.
foreach(page index.html m2hle.css)
  file(READ "${SITE}/${page}" text)
  string(REPLACE "@M2HLE_VERSION@" "${VERSION}" text "${text}")
  file(WRITE "${SITE}/${page}" "${text}")
endforeach()
