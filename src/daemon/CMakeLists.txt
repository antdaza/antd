# Copyright (c) 2018-2024, The Oxen Project
# Copyright (c) 2014-2025, The Monero Project
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are
# permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this list of
#    conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice, this list
#    of conditions and the following disclaimer in the documentation and/or other
#    materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors may be
#    used to endorse or promote products derived from this software without specific
#    prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
# THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
# THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set(daemon_sources
  command_parser_executor.cpp
  command_server.cpp
  daemon.cpp
  executor.cpp
  main.cpp
  rpc_command_executor.cpp
)

set(daemon_headers)

set(daemon_private_headers
  command_parser_executor.h
  command_server.h
  core.h
  daemon.h
  executor.h
  p2p.h
  protocol.h
  rpc.h
  rpc_command_executor.h

  # cryptonote_protocol
  ../cryptonote_protocol/cryptonote_protocol_defs.h
  ../cryptonote_protocol/cryptonote_protocol_handler.h
  ../cryptonote_protocol/cryptonote_protocol_handler.inl
  ../cryptonote_protocol/cryptonote_protocol_handler_common.h

  # p2p
  ../p2p/net_node.h
  ../p2p/net_node_common.h
  ../p2p/net_peerlist.h
  ../p2p/net_peerlist_boost_serialization.h
  ../p2p/p2p_protocol_defs.h
  ../p2p/stdafx.h)

antd_private_headers(daemon
  ${daemon_private_headers})
antd_add_executable(daemon
  ${daemon_sources}
  ${daemon_headers}
  ${daemon_private_headers})
target_link_libraries(daemon
  PRIVATE
    rpc
    blockchain_db
    cryptonote_core
    cncrypto
    common
    p2p
    cryptonote_protocol
    daemonizer
    serialization
    daemon_rpc_server
    ${EPEE_READLINE}
    version
    ${Boost_CHRONO_LIBRARY}
    ${Boost_FILESYSTEM_LIBRARY}
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    ${Boost_REGEX_LIBRARY}
    ${Boost_SYSTEM_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    ${ZMQ_LIB}
    ${GNU_READLINE_LIBRARY}
    ${EXTRA_LIBRARIES}
    ${Blocks})
set_property(TARGET daemon
  PROPERTY
    OUTPUT_NAME "antdd")
install(TARGETS daemon DESTINATION bin)
