// Copyright (c) 2014-2025, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#pragma once

#include <list>
#include "serialization/keyvalue_serialization.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/blobdatatype.h"
#include "cryptonote_core/full_node_deregister.h"
#include "cryptonote_basic/difficulty.h"

namespace cryptonote
{


#define BC_COMMANDS_POOL_BASE 2000

  /************************************************************************/
  /* P2P connection info, serializable to json                            */
  /************************************************************************/
  struct connection_info
  {
    bool incoming;
    bool localhost;
    bool local_ip;

    std::string address;
    std::string host;
    std::string ip;
    std::string port;

    std::string peer_id;

    uint64_t recv_count;
    uint64_t recv_idle_time;

    uint64_t send_count;
    uint64_t send_idle_time;

    std::string state;

    uint64_t live_time;

	uint64_t avg_download;
	uint64_t current_download;
	
	uint64_t avg_upload;
	uint64_t current_upload;
  
	uint32_t support_flags;

	std::string connection_id;

    uint64_t height;

    uint32_t pruning_seed;

    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(incoming)
      KV_SERIALIZE(localhost)
      KV_SERIALIZE(local_ip)
      KV_SERIALIZE(address)
      KV_SERIALIZE(host)
      KV_SERIALIZE(ip)
      KV_SERIALIZE(port)
      KV_SERIALIZE(peer_id)
      KV_SERIALIZE(recv_count)
      KV_SERIALIZE(recv_idle_time)
      KV_SERIALIZE(send_count)
      KV_SERIALIZE(send_idle_time)
      KV_SERIALIZE(state)
      KV_SERIALIZE(live_time)
      KV_SERIALIZE(avg_download)
      KV_SERIALIZE(current_download)
      KV_SERIALIZE(avg_upload)
      KV_SERIALIZE(current_upload)
      KV_SERIALIZE(support_flags)
      KV_SERIALIZE(connection_id)
      KV_SERIALIZE(height)
      KV_SERIALIZE(pruning_seed)
    END_KV_SERIALIZE_MAP()
  };

  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct block_complete_entry
  {
    blobdata block;
    std::vector<blobdata> txs;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(block)
      KV_SERIALIZE(txs)
    END_KV_SERIALIZE_MAP()
  };


  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_NEW_BLOCK
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 1;

    struct request
    {
      block_complete_entry b;
      uint64_t current_blockchain_height;

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(b)
        KV_SERIALIZE(current_blockchain_height)
      END_KV_SERIALIZE_MAP()
    };
  };

  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_NEW_TRANSACTIONS
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 2;

    struct request
    {
      std::vector<blobdata>   txs;
      std::string _; // padding

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(txs)
        KV_SERIALIZE(_)
      END_KV_SERIALIZE_MAP()
    };
  };
  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_REQUEST_GET_OBJECTS
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 3;

    struct request
    {
      std::vector<crypto::hash>    txs;
      std::vector<crypto::hash>    blocks;

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(txs)
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(blocks)
      END_KV_SERIALIZE_MAP()
    };
  };

  struct NOTIFY_RESPONSE_GET_OBJECTS
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 4;

    struct request
    {
      std::vector<blobdata>              txs;
      std::vector<block_complete_entry>  blocks;
      std::vector<crypto::hash>          missed_ids;
      uint64_t                         current_blockchain_height;

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(txs)
        KV_SERIALIZE(blocks)
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(missed_ids)
        KV_SERIALIZE(current_blockchain_height)
      END_KV_SERIALIZE_MAP()
    };
  };


struct CORE_SYNC_DATA
{
    uint64_t current_height;
    uint64_t cumulative_difficulty;
    uint64_t cumulative_difficulty_top64;
    crypto::hash top_id;
    uint8_t top_version;
    uint32_t pruning_seed;

    BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(current_height)
        KV_SERIALIZE(cumulative_difficulty)
        KV_SERIALIZE(cumulative_difficulty_top64)
        KV_SERIALIZE_VAL_POD_AS_BLOB(top_id)
        KV_SERIALIZE_OPT(top_version, (uint8_t)0)
        KV_SERIALIZE_OPT(pruning_seed, (uint32_t)0)
    END_KV_SERIALIZE_MAP()
};


  struct NOTIFY_REQUEST_CHAIN
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 6;

    struct request
    {
      std::list<crypto::hash> block_ids; /*IDs of the first 10 blocks are sequential, next goes with pow(2,n) offset, like 2, 4, 8, 16, 32, 64 and so on, and the last one is always genesis block */

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(block_ids)
      END_KV_SERIALIZE_MAP()
    };
  };

  struct NOTIFY_RESPONSE_CHAIN_ENTRY
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 7;

struct request
{
    uint64_t start_height;
    uint64_t total_height;
    uint64_t cumulative_difficulty;
    uint64_t cumulative_difficulty_top64;
    std::vector<crypto::hash> m_block_ids;

    // Helper methods
   /* void set_cumulative_difficulty(const difficulty_type& diff) {
        cumulative_difficulty_str = diff.str();
    }
    difficulty_type get_cumulative_difficulty() const {
        return difficulty_type(cumulative_difficulty_str);
    }*/

    BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(start_height)
        KV_SERIALIZE(total_height)
        KV_SERIALIZE(cumulative_difficulty)
        KV_SERIALIZE(cumulative_difficulty_top64)
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(m_block_ids)
    END_KV_SERIALIZE_MAP()
};
  };
  
  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_NEW_FLUFFY_BLOCK
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 8;

    struct request
    {
      block_complete_entry b;
      uint64_t current_blockchain_height;

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(b)
        KV_SERIALIZE(current_blockchain_height)
      END_KV_SERIALIZE_MAP()
    };
  };  

  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_REQUEST_FLUFFY_MISSING_TX
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 9;

    struct request
    {
      crypto::hash block_hash;
      uint64_t current_blockchain_height;      
      std::vector<uint64_t> missing_tx_indices;
      
      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE_VAL_POD_AS_BLOB(block_hash)
        KV_SERIALIZE(current_blockchain_height)
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(missing_tx_indices)
      END_KV_SERIALIZE_MAP()
    };
  }; 

  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_NEW_DEREGISTER_VOTE
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 10;

    struct request
    {
      std::vector<full_nodes::deregister_vote> votes;
      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE_CONTAINER_POD_AS_BLOB(votes)
      END_KV_SERIALIZE_MAP()
    };
  };
    
  /************************************************************************/
  /*                                                                      */
  /************************************************************************/
  struct NOTIFY_UPTIME_PROOF
  {
    const static int ID = BC_COMMANDS_POOL_BASE + 11;

    struct request
    {
      uint16_t snode_version_major;
      uint16_t snode_version_minor;
      uint16_t snode_version_patch;

      uint64_t timestamp;
      crypto::public_key pubkey;
      crypto::signature sig;

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(snode_version_major)
        KV_SERIALIZE(snode_version_minor)
        KV_SERIALIZE(snode_version_patch)
        KV_SERIALIZE(timestamp)
        KV_SERIALIZE_VAL_POD_AS_BLOB(pubkey)
        KV_SERIALIZE_VAL_POD_AS_BLOB(sig)
      END_KV_SERIALIZE_MAP()
    };
  };
}
