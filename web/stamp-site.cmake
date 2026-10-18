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
foreach(page index.html home.html m2hle.css)
  file(READ "${SITE}/${page}" text)
  string(REPLACE "@M2HLE_VERSION@" "${VERSION}" text "${text}")
  file(WRITE "${SITE}/${page}" "${text}")
endforeach()

# A game's own page: index.html with that game's words in its head. m2hle-game.js
# gives the page its game's title once it runs, but a link preview (Discord, a chat
# app, a search engine) reads the page without running it, so a link that should
# name its game points at sfight.html, srallyc.html, vonj.html, dynamcop.html,
# fvipers.html, hotd.html or daytona.html (m2hle-games.js picks the game
# from the file name; Pinboard #673, #881, #935). The same text as m2hle-game.js sets.
file(READ "${SITE}/index.html" text)
set(site_title "<title>Model 2 Arcade</title>")
set(site_desc "content=\"Play Sega Model 2 arcade games in your browser, with your own copy of the game.\"")
foreach(want site_title site_desc)
  string(FIND "${text}" "${${want}}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "stamp-site: index.html no longer has ${${want}}; update the game pages' words here")
  endif()
endforeach()
foreach(game "sfight|Sonic the Fighters" "srallyc|Sega Rally Championship"
             "vonj|Cyber Troopers Virtual-On" "dynamcop|Dynamite Cop"
             "fvipers|Fighting Vipers" "hotd|The House of the Dead"
             "daytona|Daytona USA")
  string(REPLACE "|" ";" game "${game}")
  list(GET game 0 id)
  list(GET game 1 title)
  string(REPLACE "${site_title}" "<title>${title}</title>" page "${text}")
  string(REPLACE "${site_desc}"
    "content=\"Play ${title} in your browser, with your own copy of the game.\"" page "${page}")
  file(WRITE "${SITE}/${id}.html" "${page}")
endforeach()
