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
#include "cryptonote_basic/cryptonote_format_utils.h"
#include <boost/serialization/vector.hpp>
#include <boost/serialization/utility.hpp>
#include "ringct/rctOps.h"

namespace cryptonote
{
  //---------------------------------------------------------------
  keypair  get_deterministic_keypair_from_height(uint64_t height);
  bool     get_deterministic_output_key         (const account_public_address& address, const keypair& tx_key, size_t output_index, crypto::public_key& output_key);
  bool     validate_control_reward_key       (uint64_t height, const std::string& control_wallet_address_str, size_t output_index, const crypto::public_key& output_key, const cryptonote::network_type nettype);

  uint64_t control_reward_formula            (uint64_t base_reward);
  bool     block_has_control_output          (network_type nettype, cryptonote::block const &block);
  bool     height_has_control_output         (network_type nettype, int hard_fork_version, uint64_t height);
  uint64_t derive_control_from_block_reward  (network_type nettype, const cryptonote::block &block);

  uint64_t get_portion_of_reward                (uint64_t portions, uint64_t total_full_node_reward);
  uint64_t full_node_reward_formula          (uint64_t base_reward, int hard_fork_version);

  struct antd_miner_tx_context // NOTE(antd): All the custom fields required by Antd to use construct_miner_tx
  {
    using stake_portions = uint64_t;

    antd_miner_tx_context(network_type type                = MAINNET,
                          crypto::public_key const &winner = crypto::null_pkey,
                          std::vector<std::pair<account_public_address, stake_portions>> const &winner_info = {});

    network_type                                                   nettype;
    crypto::public_key                                             snode_winner_key;
    std::vector<std::pair<account_public_address, stake_portions>> snode_winner_info;  // NOTE: If empty we use full_nodes::null_winner
    uint64_t                                                       batched_control; // NOTE: 0 until hardfork v10, then use blockchain::calc_batched_control_reward
  };

  bool construct_miner_tx(
      size_t height,
      size_t median_weight,
      uint64_t already_generated_coins,
      size_t current_block_weight,
      uint64_t fee,
      const account_public_address &miner_address,
      transaction& tx,
      const blobdata& extra_nonce = blobdata(),
      uint8_t hard_fork_version = 1,
      const antd_miner_tx_context &miner_context = {});

  struct block_reward_parts
  {
    uint64_t full_node_total;
    uint64_t full_node_paid;

    uint64_t control;
    uint64_t base_miner;
    uint64_t base_miner_fee;

    // NOTE: Post hardfork 10, adjusted base reward is the block reward with the
    // control amount removed. We still need the original base reward, so
    // that we can calculate the 50% on the whole base amount, that should be
    // allocated for the fullnode and fees.

    // If this block contains the batched control payment, this is
    // included in the adjusted base reward.

    // Before hardfork 10, this is the same value as original_base_reward
    uint64_t adjusted_base_reward;
    uint64_t original_base_reward;

    uint64_t miner_reward() { return base_miner + base_miner_fee; }
  };

  struct antd_block_reward_context
  {
    using portions = uint64_t;
    uint64_t                                                 height;
    uint64_t                                                 fee;
    uint64_t                                                 batched_control; // Optional: 0 hardfork v10, then must be calculated using blockchain::calc_batched_control_reward
    std::vector<std::pair<account_public_address, portions>> snode_winner_info;  // Optional: Check contributor portions add up, else set empty to use full_nodes::null_winner
  };

  // NOTE(antd): I would combine this into get_base_block_reward, but
  // cryptonote_basic as a library is to be able to trivially link with
  // cryptonote_core since it would have a circular dependency on Blockchain

  // NOTE: Block reward function that should be called after hard fork v10
  bool get_antd_block_reward(size_t median_weight, size_t current_block_weight, uint64_t already_generated_coins, int hard_fork_version, block_reward_parts &result, const antd_block_reward_context &antd_context);

  struct tx_source_entry
  {
    typedef std::pair<uint64_t, rct::ctkey> output_entry;

    std::vector<output_entry> outputs;  //index + key + optional ringct commitment
    size_t real_output;                 //index in outputs vector of real output_entry
    crypto::public_key real_out_tx_key; //incoming real tx public key
    std::vector<crypto::public_key> real_out_additional_tx_keys; //incoming real tx additional public keys
    size_t real_output_in_tx_index;     //index in transaction outputs vector
    uint64_t amount;                    //money
    bool rct;                           //true if the output is rct
    rct::key mask;                      //ringct amount mask
    rct::multisig_kLRki multisig_kLRki; //multisig info

    void push_output(uint64_t idx, const crypto::public_key &k, uint64_t amount) { outputs.push_back(std::make_pair(idx, rct::ctkey({rct::pk2rct(k), rct::zeroCommit(amount)}))); }

    BEGIN_SERIALIZE_OBJECT()
      FIELD(outputs)
      FIELD(real_output)
      FIELD(real_out_tx_key)
      FIELD(real_out_additional_tx_keys)
      FIELD(real_output_in_tx_index)
      FIELD(amount)
      FIELD(rct)
      FIELD(mask)
      FIELD(multisig_kLRki)

      if (real_output >= outputs.size())
        return false;
    END_SERIALIZE()
  };

  struct tx_destination_entry
  {
    std::string original;
    uint64_t amount;                    //money
    account_public_address addr;        //destination address
    bool is_subaddress;
    bool is_integrated;

    tx_destination_entry() : amount(0), addr(AUTO_VAL_INIT(addr)), is_subaddress(false), is_integrated(false) { }
    tx_destination_entry(uint64_t a, const account_public_address &ad, bool is_subaddress) : amount(a), addr(ad), is_subaddress(is_subaddress), is_integrated(false) { }
    tx_destination_entry(const std::string &o, uint64_t a, const account_public_address &ad, bool is_subaddress) : original(o), amount(a), addr(ad), is_subaddress(is_subaddress), is_integrated(false) { }

    bool operator==(const tx_destination_entry& other) const
    {
      return amount == other.amount && addr == other.addr;
    }

    BEGIN_SERIALIZE_OBJECT()
      FIELD(original)
      VARINT_FIELD(amount)
      FIELD(addr)
      FIELD(is_subaddress)
      FIELD(is_integrated)
    END_SERIALIZE()
  };

  //---------------------------------------------------------------
  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr);
  bool construct_tx(const account_keys& sender_account_keys, std::vector<tx_source_entry> &sources, const std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, uint8_t hf_version = cryptonote::network_version_7, bool is_staking = false);

  struct antd_construct_tx_params
  {
    bool                v4_allow_tx_types;
    bool                v3_per_output_unlock;
    bool                v3_is_staking_tx;     // NOTE: Set to true manually if you need a staking transaction
    bool                v2_rct;

    antd_construct_tx_params() = default;
    antd_construct_tx_params(int hf_version)
    {
      *this = {};
      v4_allow_tx_types    = (hf_version >= cryptonote::network_version_11_infinite_staking);
      v3_per_output_unlock = (hf_version >= cryptonote::network_version_9_full_nodes);
      v2_rct               = (hf_version >= cryptonote::network_version_7);
    }
  };

  bool construct_tx_with_tx_key   (const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key, const std::vector<crypto::secret_key> &additional_tx_keys, const rct::RCTConfig &rct_config = { rct::RangeProofBorromean, 0}, rct::multisig_out *msout = NULL, bool shuffle_outs = true, antd_construct_tx_params const tx_params = {});
  bool construct_tx_and_get_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time,       crypto::secret_key &tx_key,       std::vector<crypto::secret_key> &additional_tx_keys, const rct::RCTConfig &rct_config = { rct::RangeProofBorromean, 0}, rct::multisig_out *msout = NULL, antd_construct_tx_params const tx_params = {});
  bool generate_output_ephemeral_keys(const size_t tx_version, bool &found_change,
                                      const cryptonote::account_keys &sender_account_keys, const crypto::public_key &txkey_pub,  const crypto::secret_key &tx_key,
                                      const cryptonote::tx_destination_entry &dst_entr, const boost::optional<cryptonote::tx_destination_entry> &change_addr, const size_t output_index,
                                      const bool &need_additional_txkeys, const std::vector<crypto::secret_key> &additional_tx_keys,
                                      std::vector<crypto::public_key> &additional_tx_public_keys,
                                      std::vector<rct::key> &amount_keys,
                                      crypto::public_key &out_eph_public_key);

  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    );

  class Blockchain;
  bool get_block_longhash(const Blockchain *pb, const block& b, crypto::hash& res, const uint64_t height, const int miners);
  void get_altblock_longhash(const block& b, crypto::hash& res, const uint64_t main_height, const uint64_t height,
    const uint64_t seed_height, const crypto::hash& seed_hash);
  crypto::hash get_block_longhash(const Blockchain *pb, const block& b, const uint64_t height, const int miners);
  void get_block_longhash_reorg(const uint64_t split_height);
}

BOOST_CLASS_VERSION(cryptonote::tx_source_entry, 1)
BOOST_CLASS_VERSION(cryptonote::tx_destination_entry, 2)

namespace boost
{
  namespace serialization
  {
    template <class Archive>
    inline void serialize(Archive &a, cryptonote::tx_source_entry &x, const boost::serialization::version_type ver)
    {
      a & x.outputs;
      a & x.real_output;
      a & x.real_out_tx_key;
      a & x.real_output_in_tx_index;
      a & x.amount;
      a & x.rct;
      a & x.mask;
      if (ver < 1)
        return;
      a & x.multisig_kLRki;
      a & x.real_out_additional_tx_keys;
    }

    template <class Archive>
    inline void serialize(Archive& a, cryptonote::tx_destination_entry& x, const boost::serialization::version_type ver)
    {
      a & x.amount;
      a & x.addr;
      if (ver < 1)
        return;
      a & x.is_subaddress;
      if (ver < 2)
      {
        x.is_integrated = false;
        return;
      }
      a & x.original;
      a & x.is_integrated;
    }
  }
}
