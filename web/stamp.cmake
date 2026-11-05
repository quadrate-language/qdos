# Stamp the page with what was built, after every link.
#
# The site is served with a cache lifetime, so a browser can hold one deploy's
# page and another's code: a word the page uses is then not defined. The page
# fetches qdos.js, qdos.wasm and qdos.data under a hash of all three, so it
# only ever runs the code built with it.
#
#   cmake -Ddir=<build dir> -P stamp.cmake

set(hashes "")
foreach(file qdos.js qdos.wasm qdos.data)
	file(SHA256 "${dir}/${file}" hash)
	string(APPEND hashes "${hash}")
endforeach()
string(SHA256 stamp "${hashes}")
string(SUBSTRING "${stamp}" 0 12 stamp)

file(READ "${dir}/qdos.html" page)
string(FIND "${page}" "__QDOS_BUILD__" at)
if(at EQUAL -1)
	message(FATAL_ERROR "stamp.cmake: qdos.html has no __QDOS_BUILD__ to stamp")
endif()
string(REPLACE "__QDOS_BUILD__" "${stamp}" page "${page}")
# Emscripten writes the tag with or without quotes, depending on minifying
string(REPLACE "src=qdos.js>" "src=\"qdos.js?v=${stamp}\">" page "${page}")
string(REPLACE "src=\"qdos.js\"" "src=\"qdos.js?v=${stamp}\"" page "${page}")
string(FIND "${page}" "qdos.js?v=${stamp}" tagged)
if(tagged EQUAL -1)
	message(FATAL_ERROR "stamp.cmake: no <script src=qdos.js> in qdos.html to stamp")
endif()
file(WRITE "${dir}/qdos.html" "${page}")
