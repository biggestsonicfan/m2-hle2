# ---------------------------------------------------------------------------
# GGPO (vendor/ggpo, pond3r/ggpo, MIT): rollback netplay, the second way to play
# online beside RPCN lockstep (src/core/emu_ggpo.h, Pinboard #575).
#
# Upstream is a Windows library and the submodule stays as published, so:
#   - src/net/ggpo_port/ggpo_compat.h is force-included into every GGPO file in
#     place of its platform headers and the MSVC CRT names it uses;
#   - src/net/ggpo_port/ggpo_port.cpp replaces platform_*.cpp, poll.cpp and
#     network/udp.cpp (a socket through net_socket.h, or a host's transport);
#   - two lines only MSVC accepts are rewritten in copies of network/ under the
#     build tree (an extra qualifier on a constructor, in_addr.S_un). Those
#     copies come first on the include path, so every file sees the same ones.
#
# The desktop, the web build and the libretro core. A browser has no UDP, so
# the web build's match rides the lobby's WebSocket (net/ggpo_lobby.h, through
# ggpo_port_set_transport); the others use UDP, or the WebSocket on --ggpo-ws.
# ---------------------------------------------------------------------------
set(GGPO_SRC ${VENDOR}/ggpo/src/lib/ggpo)
set(GGPO_PATCHED ${CMAKE_BINARY_DIR}/ggpo-patched)

function(m2hle_ggpo_patch name from to)
    file(READ ${GGPO_SRC}/network/${name} _text)
    string(FIND "${_text}" "${from}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "ggpo.cmake: '${from}' is no longer in network/${name}; check the patch against the submodule")
    endif()
    string(REPLACE "${from}" "${to}" _text "${_text}")
    file(WRITE ${GGPO_PATCHED}/network/${name}.tmp "${_text}")
    configure_file(${GGPO_PATCHED}/network/${name}.tmp ${GGPO_PATCHED}/network/${name} COPYONLY)
endfunction()

foreach(_h udp.h udp_msg.h)
    configure_file(${GGPO_SRC}/network/${_h} ${GGPO_PATCHED}/network/${_h} COPYONLY)
endforeach()
m2hle_ggpo_patch(udp_proto.h "UdpProtocol::Event(Type t" "Event(Type t")
m2hle_ggpo_patch(udp_proto.cpp ".sin_addr.S_un.S_addr" ".sin_addr.s_addr")

add_library(ggpo STATIC
    ${SRC}/net/ggpo_port/ggpo_port.cpp
    ${GGPO_SRC}/bitvector.cpp
    ${GGPO_SRC}/game_input.cpp
    ${GGPO_SRC}/input_queue.cpp
    ${GGPO_SRC}/log.cpp
    ${GGPO_SRC}/main.cpp
    ${GGPO_SRC}/sync.cpp
    ${GGPO_SRC}/timesync.cpp
    ${GGPO_PATCHED}/network/udp_proto.cpp
    ${GGPO_SRC}/backends/p2p.cpp
    ${GGPO_SRC}/backends/synctest.cpp
    ${GGPO_SRC}/backends/spectator.cpp
)
set_target_properties(ggpo PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
target_include_directories(ggpo PRIVATE ${GGPO_PATCHED} ${GGPO_SRC})
target_include_directories(ggpo PUBLIC ${VENDOR}/ggpo/src/include ${SRC}/net/ggpo_port)
target_compile_definitions(ggpo PUBLIC M2HLE_GGPO=1)
if(MSVC)
    target_compile_options(ggpo PRIVATE /FI${SRC}/net/ggpo_port/ggpo_compat.h /W1
                                        /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ggpo PRIVATE -include ${SRC}/net/ggpo_port/ggpo_compat.h -w)
endif()
if(WIN32)
    target_link_libraries(ggpo PUBLIC ws2_32)
endif()
