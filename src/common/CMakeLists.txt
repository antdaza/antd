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

include_directories(SYSTEM ${OPENSSL_INCLUDE_DIR})

set(common_sources
  base58.cpp
  command_line.cpp
  dns_utils.cpp
  download.cpp
  error.cpp
  expect.cpp
  antd.cpp
  util.cpp
  i18n.cpp
  rules.cpp
  notify.cpp
  password.cpp
  perf_timer.cpp
  pruning.cpp
  scoped_message_writer.cpp
  spawn.cpp
  threadpool.cpp
  updates.cpp
  aligned.c
  timings.cc
  combinator.cpp)

if (STACK_TRACE)
  list(APPEND common_sources stack_trace.cpp)
endif()

if (BACKCOMPAT)
  list(APPEND common_sources compat/glibc_compat.cpp)
endif()

set(common_headers)

set(common_private_headers
  apply_permutation.h
  base58.h
  boost_serialization_helper.h
  command_line.h
  common_fwd.h
  dns_utils.h
  download.h
  error.h
  expect.h
  antd.h
  http_connection.h
  antd_integration_test_hooks.h
  notify.h
  pod-class.h
  pruning.h
  rpc_client.h
  scoped_message_writer.h
  unordered_containers_boost_serialization.h
  util.h
  varint.h
  i18n.h
  password.h
  perf_timer.h
  rules.h
  spawn.h
  stack_trace.h
  threadpool.h
  updates.h
  aligned.h
  timings.h
  combinator.h)

antd_private_headers(common
  ${common_private_headers})
antd_add_library(common
  ${common_sources}
  ${common_headers}
  ${common_private_headers}
  DEPENDS generate_translations_header)
target_link_libraries(common
  PUBLIC
    cncrypto
    ${UNBOUND_LIBRARIES}
    ${LIBUNWIND_LIBRARIES}
    ${Boost_DATE_TIME_LIBRARY}
    ${Boost_FILESYSTEM_LIBRARY}
    ${Boost_SYSTEM_LIBRARY}
    ${Boost_THREAD_LIBRARY}
    ${Boost_REGEX_LIBRARY}
    ${Boost_CHRONO_LIBRARY}
  PRIVATE
    ${OPENSSL_LIBRARIES}
    ${EPEE_READLINE}
    ${EXTRA_LIBRARIES})

#monero_install_headers(common
#  ${common_headers})
