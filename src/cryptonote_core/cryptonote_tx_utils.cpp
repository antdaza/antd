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

#include <unordered_set>
#include <random>
#include "include_base_utils.h"
#include "string_tools.h"
using namespace epee;

#include "common/apply_permutation.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_config.h"
#include "blockchain.h"
#include "cryptonote_basic/miner.h"
#include "cryptonote_basic/tx_extra.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "ringct/rctSigs.h"
#include "multisig/multisig.h"
#include "int-util.h"
#include "cryptonote_core/full_node_list.h"

using namespace crypto;

namespace cryptonote
{
  //---------------------------------------------------------------
  static void classify_addresses(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr, size_t &num_stdaddresses, size_t &num_subaddresses, account_public_address &single_dest_subaddress)
  {
    num_stdaddresses = 0;
    num_subaddresses = 0;
    std::unordered_set<cryptonote::account_public_address> unique_dst_addresses;
    bool change_found = false;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      if (change_addr && *change_addr == dst_entr && !change_found)
      {
        change_found = true;
        continue;
      }
      if (unique_dst_addresses.count(dst_entr.addr) == 0)
      {
        unique_dst_addresses.insert(dst_entr.addr);
        if (dst_entr.is_subaddress)
        {
          ++num_subaddresses;
          single_dest_subaddress = dst_entr.addr;
        }
        else
        {
          ++num_stdaddresses;
        }
      }
    }
    LOG_PRINT_L2("destinations include " << num_stdaddresses << " standard addresses and " << num_subaddresses << " subaddresses");
  }

  keypair get_deterministic_keypair_from_height(uint64_t height)
  {
    keypair k;

    ec_scalar& sec = k.sec;

    for (int i=0; i < 8; i++)
    {
      uint64_t height_byte = height & ((uint64_t)0xFF << (i*8));
      uint8_t byte = height_byte >> i*8;
      sec.data[i] = byte;
    }
    for (int i=8; i < 32; i++)
    {
      sec.data[i] = 0x00;
    }

    generate_keys(k.pub, k.sec, k.sec, true);

    return k;
  }

  bool get_deterministic_output_key(const account_public_address& address, const keypair& tx_key, size_t output_index, crypto::public_key& output_key)
  {

    crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
    bool r = crypto::generate_key_derivation(address.m_view_public_key, tx_key.sec, derivation);
    CHECK_AND_ASSERT_MES(r, false, "failed to generate_key_derivation(" << address.m_view_public_key << ", " << tx_key.sec << ")");

    r = crypto::derive_public_key(derivation, output_index, address.m_spend_public_key, output_key);
    CHECK_AND_ASSERT_MES(r, false, "failed to derive_public_key(" << derivation << ", " << output_index << ", "<< address.m_spend_public_key << ")");

    return true;
  }

  bool validate_control_reward_key(uint64_t height, const std::string& control_wallet_address_str, size_t output_index, const crypto::public_key& output_key, const cryptonote::network_type nettype)
  {
    keypair ctrl_key = get_deterministic_keypair_from_height(height);

    cryptonote::address_parse_info control_wallet_address;
    cryptonote::get_account_address_from_str(control_wallet_address, nettype, control_wallet_address_str);
    crypto::public_key correct_key;

    if (!get_deterministic_output_key(control_wallet_address.address, ctrl_key, output_index, correct_key))
    {
      MERROR("Failed to generate deterministic output key for control wallet output validation");
      return false;
    }

    return correct_key == output_key;
  }

  const int CONTROL_BASE_REWARD_DIVISOR   = 6;
  const int FULL_NODE_BASE_REWARD_DIVISOR = 2;
  uint64_t control_reward_formula(uint64_t base_reward)
  {
    return base_reward / CONTROL_BASE_REWARD_DIVISOR;
  }

  bool block_has_control_output(network_type nettype, cryptonote::block const &block)
  {
    bool result = height_has_control_output(nettype, block.major_version, get_block_height(block));
    return result;
  }

  bool height_has_control_output(network_type nettype, int hard_fork_version, uint64_t height)
  {
    if (height == 0)
      return false;

    if (hard_fork_version <= network_version_9_full_nodes)
      return true;

    const cryptonote::config_t &network = cryptonote::get_config(nettype, hard_fork_version);
    if (height % network.CONTROL_REWARD_INTERVAL_IN_BLOCKS != 0)
    {
      return false;
    }

    return true;
  }

  uint64_t derive_control_from_block_reward(network_type nettype, const cryptonote::block &block)
  {
    uint64_t result       = 0;
    uint64_t snode_reward = 0;
    uint64_t vout_end     = block.miner_tx.vout.size();

    if (block_has_control_output(nettype, block))
      --vout_end; // skip the control output, the control may be the batched amount. we want the original base reward

    for (size_t vout_index = 1; vout_index < vout_end; ++vout_index)
    {
      tx_out const &output = block.miner_tx.vout[vout_index];
      snode_reward += output.amount;
    }

    static_assert(FULL_NODE_BASE_REWARD_DIVISOR == 2 &&
                  CONTROL_BASE_REWARD_DIVISOR == 6,
                  "Anytime this changes, you should revisit this code and "
                  "check, because we rely on the fullnode reward being 50\% "
                  "of the base reward, and does not receive any fees. This isn't "
                  "exactly intuitive and so changes to the reward structure may "
                  "make this assumption invalid.");

    uint64_t base_reward  = snode_reward * FULL_NODE_BASE_REWARD_DIVISOR;
    uint64_t control   = control_reward_formula(base_reward);
    uint64_t block_reward = base_reward - control;

    uint64_t actual_reward = 0; // sanity check
    for (tx_out const &output : block.miner_tx.vout) actual_reward += output.amount;

    CHECK_AND_ASSERT_MES(block_reward <= actual_reward, false,
        "Rederiving the base block reward from the fullnode reward "
        "exceeded the actual amount paid in the block, derived block reward: "
        << block_reward << ", actual reward: " << actual_reward);

    result = control;
    return result;
  }

  uint64_t full_node_reward_formula(uint64_t base_reward, int hard_fork_version)
  {
    return hard_fork_version >= 9 ? (base_reward / FULL_NODE_BASE_REWARD_DIVISOR) : 0;
  }

  uint64_t get_portion_of_reward(uint64_t portions, uint64_t total_full_node_reward)
  {
    uint64_t hi, lo, rewardhi, rewardlo;
    lo = mul128(total_full_node_reward, portions, &hi);
    div128_64(hi, lo, STAKING_PORTIONS, &rewardhi, &rewardlo);
    return rewardlo;
  }

  static uint64_t calculate_sum_of_portions(const std::vector<std::pair<cryptonote::account_public_address, uint64_t>>& portions, uint64_t total_full_node_reward)
  {
    uint64_t reward = 0;
    for (size_t i = 0; i < portions.size(); i++)
      reward += get_portion_of_reward(portions[i].second, total_full_node_reward);
    return reward;
  }

  antd_miner_tx_context::antd_miner_tx_context(network_type type, crypto::public_key const &winner, std::vector<std::pair<account_public_address, stake_portions>> const &winner_info)
    : nettype(type)
    , snode_winner_key(winner)
    , snode_winner_info(winner_info)
    , batched_control(0)
  {
  }

  bool construct_miner_tx(
      size_t height,
      size_t median_weight,
      uint64_t already_generated_coins,
      size_t current_block_weight,
      uint64_t fee,
      const account_public_address &miner_address,
      transaction& tx,
      const blobdata& extra_nonce,
      uint8_t hard_fork_version,
      const antd_miner_tx_context &miner_tx_context)
  {
    tx.vin.clear();
    tx.vout.clear();
    tx.extra.clear();
    tx.output_unlock_times.clear();
    tx.type = transaction::type_standard;
    tx.version = (hard_fork_version >= network_version_9_full_nodes) ? transaction::version_3_per_output_unlock_times : transaction::version_2;

    const network_type                                              nettype           = miner_tx_context.nettype;
    const crypto::public_key                                       &full_node_key  = miner_tx_context.snode_winner_key;
    const std::vector<std::pair<account_public_address, uint64_t>> &full_node_info =
      miner_tx_context.snode_winner_info.empty() ?
      full_nodes::null_winner : miner_tx_context.snode_winner_info;

    keypair txkey = keypair::generate(hw::get_device("default"));
    add_tx_pub_key_to_extra(tx, txkey.pub);
    if(!extra_nonce.empty())
      if(!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
        return false;
    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    keypair ctrl_key = get_deterministic_keypair_from_height(height); // NOTE: Always need since we use same key for fullnode
    if (already_generated_coins != 0)
    {
      add_tx_pub_key_to_extra(tx, ctrl_key.pub);
    }

    add_full_node_winner_to_tx_extra(tx.extra, full_node_key);

    txin_gen in;
    in.height = height;

    antd_block_reward_context block_reward_context = {};
    block_reward_context.fee                       = fee;
    block_reward_context.height                    = height;
    block_reward_context.snode_winner_info         = miner_tx_context.snode_winner_info;
    block_reward_context.batched_control        = miner_tx_context.batched_control;

    block_reward_parts reward_parts;
    if(!get_antd_block_reward(median_weight, current_block_weight, already_generated_coins, hard_fork_version, reward_parts, block_reward_context))
    {
      LOG_PRINT_L0("Failed to calculate block reward");
      return false;
    }

#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
    LOG_PRINT_L1("Creating block template: reward " << block_reward <<
      ", fee " << fee);
#endif

    uint64_t summary_amounts = 0;
    // Miner Reward
    {
      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
      bool r = crypto::generate_key_derivation(miner_address.m_view_public_key, txkey.sec, derivation);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << miner_address.m_view_public_key << ", " << txkey.sec << ")");

      r = crypto::derive_public_key(derivation, 0, miner_address.m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << 0 << ", "<< miner_address.m_spend_public_key << ")");

      txout_to_key tk;
      tk.key = out_eph_public_key;

      tx_out out;
      summary_amounts += out.amount = reward_parts.miner_reward();
      out.target = tk;
      tx.vout.push_back(out);
      tx.output_unlock_times.push_back(height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW);
    }

    if (hard_fork_version >= network_version_9_full_nodes) // Full Node Reward
    {
      for (size_t i = 0; i < full_node_info.size(); i++)
      {
        crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
        crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
        bool r = crypto::generate_key_derivation(full_node_info[i].first.m_view_public_key, ctrl_key.sec, derivation);
        CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << full_node_info[i].first.m_view_public_key << ", " << ctrl_key.sec << ")");
        r = crypto::derive_public_key(derivation, 1+i, full_node_info[i].first.m_spend_public_key, out_eph_public_key);
        CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << (1+i) << ", "<< full_node_info[i].first.m_spend_public_key << ")");

        txout_to_key tk;
        tk.key = out_eph_public_key;

        tx_out out;
        summary_amounts += out.amount = get_portion_of_reward(full_node_info[i].second, reward_parts.full_node_total);
        out.target = tk;
        tx.vout.push_back(out);
        tx.output_unlock_times.push_back(height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW);
      }
    }

    // Control Distribution
    if (already_generated_coins != 0)
    {
      if (reward_parts.control == 0)
      {
        CHECK_AND_ASSERT_MES(hard_fork_version >= network_version_10_bulletproofs, false, "Control reward can NOT be 0 before hardfork 10, hard_fork_version: " << hard_fork_version);
      }
      else
      {
        cryptonote::address_parse_info control_wallet_address;
        cryptonote::get_account_address_from_str(control_wallet_address, nettype, *cryptonote::get_config(nettype, hard_fork_version).CONTROL_WALLET_ADDRESS);
        crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);

        if (!get_deterministic_output_key(control_wallet_address.address, ctrl_key, tx.vout.size(), out_eph_public_key))
        {
          MERROR("Failed to generate deterministic output key for control wallet output creation");
          return false;
        }

        txout_to_key tk;
        tk.key = out_eph_public_key;

        tx_out out;
        summary_amounts += out.amount = reward_parts.control;
        out.target = tk;
        tx.vout.push_back(out);
        tx.output_unlock_times.push_back(height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW);
      }
    }

    uint64_t expected_amount = reward_parts.miner_reward() + reward_parts.control + reward_parts.full_node_paid;
    CHECK_AND_ASSERT_MES(summary_amounts == expected_amount, false, "Failed to construct miner tx, summary_amounts = " << summary_amounts << " not equal total block_reward = " << expected_amount);

    //lock
    tx.unlock_time = height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;
    tx.vin.push_back(in);
    tx.invalidate_hashes();

    //LOG_PRINT("MINER_TX generated ok, block_reward=" << print_money(block_reward) << "("  << print_money(block_reward - fee) << "+" << print_money(fee)
    //  << "), current_block_size=" << current_block_size << ", already_generated_coins=" << already_generated_coins << ", tx_id=" << get_transaction_hash(tx), LOG_LEVEL_2);
    return true;
  }

  bool get_antd_block_reward(size_t median_weight, size_t current_block_weight, uint64_t already_generated_coins, int hard_fork_version, block_reward_parts &result, const antd_block_reward_context &antd_context)
  {
    result = {};
    uint64_t base_reward;
    if (!get_base_block_reward(median_weight, current_block_weight, already_generated_coins, base_reward, hard_fork_version, antd_context.height))
    {
      MERROR("Failed to calculate base block reward");
      return false;
    }

    if (base_reward == 0)
    {
      MERROR("Unexpected base reward of 0");
      return false;
    }

    if (already_generated_coins == 0)
    {
      result.original_base_reward = result.adjusted_base_reward = result.base_miner = base_reward;
      return true;
    }

    //TODO: declining control reward schedule
    result.original_base_reward = base_reward;
    result.full_node_total   = full_node_reward_formula(base_reward, hard_fork_version);
    if (antd_context.snode_winner_info.empty()) result.full_node_paid = calculate_sum_of_portions(full_nodes::null_winner,     result.full_node_total);
    else                                        result.full_node_paid = calculate_sum_of_portions(antd_context.snode_winner_info, result.full_node_total);

    result.adjusted_base_reward = result.original_base_reward;
    if (hard_fork_version >= network_version_10_bulletproofs)
    {
      // NOTE: After hardfork 10, remove the control component in the base
      // reward as they are not included and batched into a later block. If we
      // calculated a (control reward > 0), then this is the batched height,
      // add it to the adjusted base reward afterwards
      result.control            = antd_context.batched_control;
      result.adjusted_base_reward -= control_reward_formula(result.original_base_reward);

      if (result.control > 0)
        result.adjusted_base_reward += result.control;
    }
    else
    {
      result.control = control_reward_formula(result.original_base_reward);
    }

    result.base_miner     = result.adjusted_base_reward - (result.control + result.full_node_paid);
    result.base_miner_fee = antd_context.fee;
    return true;
  }

  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr)
  {
    account_public_address addr = {null_pkey, null_pkey};
    size_t count = 0;
    bool found_change = false;
    for (const auto &i : destinations)
    {
      if (i.amount == 0)
        continue;
      if (change_addr && *change_addr == i && !found_change)
      {
        found_change = true;
        continue;
      }
      if (i.addr == addr)
        continue;
      if (count > 0)
        return null_pkey;
      addr = i.addr;
      ++count;
    }
    if (count == 0 && change_addr)
      return change_addr->addr.m_view_public_key;
    return addr.m_view_public_key;
  }
  //---------------------------------------------------------------
  bool construct_tx_with_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<tx_destination_entry>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key, const std::vector<crypto::secret_key> &additional_tx_keys, const rct::RCTConfig &rct_config, rct::multisig_out *msout, bool shuffle_outs, const antd_construct_tx_params tx_params)
  {
    hw::device &hwdev = sender_account_keys.get_device();

    if (sources.empty())
    {
      LOG_ERROR("Empty sources");
      return false;
    }

    std::vector<rct::key> amount_keys;
    tx.set_null();
    amount_keys.clear();
    if (msout)
    {
      msout->c.clear();
    }

    if (tx_params.v4_allow_tx_types)
    {
      tx.version = transaction::version_4_tx_types;
      tx.type    = transaction::type_standard;
    }
    else
    {
      if (tx_params.v3_per_output_unlock)
      {
        tx.version = transaction::version_3_per_output_unlock_times;
      }
      else
      {
        tx.version     = tx_params.v2_rct ? 2 : 1;
        tx.unlock_time = unlock_time;
      }
    }


    tx.extra = extra;
    crypto::public_key txkey_pub;

    if (tx_params.v3_is_staking_tx)
      add_tx_secret_key_to_tx_extra(tx.extra, tx_key);

    // if we have a stealth payment id, find it and encrypt it with the tx key now
    std::vector<tx_extra_field> tx_extra_fields;
    if (parse_tx_extra(tx.extra, tx_extra_fields))
    {
      // TODO(doyle): FIXME(doyle): LOOK AT ME. Introduced in commmit
      // c6d387184e05437d8f68a4227d739ad28568aa5e on Monero as part of the
      // deprecating process of payment IDs. I've set it to false, but it was
      // actually true before. If we want to take this route as the way to
      // deprecate payment ID's by including it in every transaction, we should
      // make this true.

      // But if we have a better way, this may not be necessary.
      //   - Jan 30, 2019
      bool add_dummy_payment_id = false;

      tx_extra_nonce extra_nonce;
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash payment_id = null_hash;
        crypto::hash8 payment_id8 = null_hash8;
        if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
        {
          LOG_PRINT_L2("Encrypting payment id " << payment_id8);
          crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
          if (view_key_pub == null_pkey)
          {
            LOG_ERROR("Destinations have to have exactly one output to support encrypted payment ids");
            return false;
          }

          if (!hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key))
          {
            LOG_ERROR("Failed to encrypt payment id");
            return false;
          }

          std::string extra_nonce;
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          remove_field_from_tx_extra(tx.extra, typeid(tx_extra_nonce));
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add encrypted payment id to tx extra");
            return false;
          }
          LOG_PRINT_L1("Encrypted payment ID: " << payment_id8);
          add_dummy_payment_id = false;
        }
        else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          add_dummy_payment_id = false;
        }
      }

      // we don't add one if we've got more than the usual 1 destination plus change
      if (destinations.size() > 2)
        add_dummy_payment_id = false;

      if (add_dummy_payment_id)
      {
        // if we have neither long nor short payment id, add a dummy short one,
        // this should end up being the vast majority of txes as time goes on
        std::string extra_nonce;
        crypto::hash8 payment_id8 = null_hash8;
        crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
        if (view_key_pub == null_pkey)
        {
          LOG_ERROR("Failed to get key to encrypt dummy payment id with");
        }
        else
        {
          hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key);
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add dummy encrypted payment id to tx extra");
            // continue anyway
          }
        }
      }
    }
    else
    {
      MWARNING("Failed to parse tx extra");
      tx_extra_fields.clear();
    }

    struct input_generation_context_data
    {
      keypair in_ephemeral;
    };
    std::vector<input_generation_context_data> in_contexts;

    uint64_t summary_inputs_money = 0;
    //fill inputs
    int idx = -1;
    for(const tx_source_entry& src_entr:  sources)
    {
      ++idx;
      if(src_entr.real_output >= src_entr.outputs.size())
      {
        LOG_ERROR("real_output index (" << src_entr.real_output << ")bigger than output_keys.size()=" << src_entr.outputs.size());
        return false;
      }
      summary_inputs_money += src_entr.amount;

      //key_derivation recv_derivation;
      in_contexts.push_back(input_generation_context_data());
      keypair& in_ephemeral = in_contexts.back().in_ephemeral;
      crypto::key_image img;
      const auto& out_key = reinterpret_cast<const crypto::public_key&>(src_entr.outputs[src_entr.real_output].second.dest);
      if(!generate_key_image_helper(sender_account_keys, subaddresses, out_key, src_entr.real_out_tx_key, src_entr.real_out_additional_tx_keys, src_entr.real_output_in_tx_index, in_ephemeral,img, hwdev))
      {
        LOG_ERROR("Key image generation failed!");
        return false;
      }

      //check that derivated key is equal with real output key (if non multisig)
      if(!msout && !(in_ephemeral.pub == src_entr.outputs[src_entr.real_output].second.dest) )
      {
        LOG_ERROR("derived public key mismatch with output public key at index " << idx << ", real out " << src_entr.real_output << "! "<< ENDL << "derived_key:"
          << string_tools::pod_to_hex(in_ephemeral.pub) << ENDL << "real output_public_key:"
          << string_tools::pod_to_hex(src_entr.outputs[src_entr.real_output].second.dest) );
        LOG_ERROR("amount " << src_entr.amount << ", rct " << src_entr.rct);
        LOG_ERROR("tx pubkey " << src_entr.real_out_tx_key << ", real_output_in_tx_index " << src_entr.real_output_in_tx_index);
        return false;
      }

      //put key image into tx input
      txin_to_key input_to_key;
      input_to_key.amount = src_entr.amount;
      input_to_key.k_image = msout ? rct::rct2ki(src_entr.multisig_kLRki.ki) : img;

      //fill outputs array and use relative offsets
      for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
        input_to_key.key_offsets.push_back(out_entry.first);

      input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
      tx.vin.push_back(input_to_key);
    }

    if (shuffle_outs)
    {
      std::shuffle(destinations.begin(), destinations.end(), std::default_random_engine(crypto::rand<unsigned int>()));
    }

    // sort ins by their key image
    std::vector<size_t> ins_order(sources.size());
    for (size_t n = 0; n < sources.size(); ++n)
      ins_order[n] = n;
    std::sort(ins_order.begin(), ins_order.end(), [&](const size_t i0, const size_t i1) {
      const txin_to_key &tk0 = boost::get<txin_to_key>(tx.vin[i0]);
      const txin_to_key &tk1 = boost::get<txin_to_key>(tx.vin[i1]);
      return memcmp(&tk0.k_image, &tk1.k_image, sizeof(tk0.k_image)) > 0;
    });
    tools::apply_permutation(ins_order, [&] (size_t i0, size_t i1) {
      std::swap(tx.vin[i0], tx.vin[i1]);
      std::swap(in_contexts[i0], in_contexts[i1]);
      std::swap(sources[i0], sources[i1]);
    });

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress;
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);

    // if this is a single-destination transfer to a subaddress, we set the tx pubkey to R=s*D
    if (num_stdaddresses == 0 && num_subaddresses == 1)
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(single_dest_subaddress.m_spend_public_key), rct::sk2rct(tx_key)));
    }
    else
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(tx_key)));
    }
    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_pub_key));
    add_tx_pub_key_to_extra(tx, txkey_pub);

    std::vector<crypto::public_key> additional_tx_public_keys;

    // we don't need to include additional tx keys if:
    //   - all the destinations are standard addresses
    //   - there's only one destination which is a subaddress
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
      CHECK_AND_ASSERT_MES(destinations.size() == additional_tx_keys.size(), false, "Wrong amount of additional tx keys");

    uint64_t summary_outs_money = 0;
    //fill outputs
    size_t output_index = 0;

    tx_extra_tx_key_image_proofs key_image_proofs;
    bool found_change_already = false;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      CHECK_AND_ASSERT_MES(dst_entr.amount > 0 || tx.version > 1, false, "Destination with wrong amount: " << dst_entr.amount);
      crypto::public_key out_eph_public_key;

      bool this_dst_is_change_addr = false;
      hwdev.generate_output_ephemeral_keys(tx.version, this_dst_is_change_addr, sender_account_keys, txkey_pub, tx_key,
                                           dst_entr, change_addr, output_index,
                                           need_additional_txkeys, additional_tx_keys,
                                           additional_tx_public_keys, amount_keys, out_eph_public_key);

      if (tx.version > 2)
      {
        if (change_addr && *change_addr == dst_entr && this_dst_is_change_addr && !found_change_already)
        {
          found_change_already = true;
          tx.output_unlock_times.push_back(0); // 0 unlock time for change
        }
        else
        {
          tx.output_unlock_times.push_back(unlock_time); // for now, all non-change have same unlock time
        }
      }

      if (tx_params.v3_is_staking_tx)
      {
        CHECK_AND_ASSERT_MES(dst_entr.addr == sender_account_keys.m_account_address, false, "A staking contribution must return back to the original sendee otherwise the pre-calculated key image is incorrect");
        CHECK_AND_ASSERT_MES(dst_entr.is_subaddress == false, false, "Staking back to a subaddress is not allowed"); // TODO(antd): Maybe one day, revisit this
        CHECK_AND_ASSERT_MES(need_additional_txkeys == false, false, "Staking TX's can not required additional TX Keys"); // TODO(antd): Maybe one day, revisit this

        if (!(change_addr && *change_addr == dst_entr))
        {
          tx_extra_tx_key_image_proofs::proof proof = {};
          keypair                ephemeral_keys = {};
          const subaddress_index zeroth_address = {};
          if(!generate_key_image_helper(sender_account_keys, subaddresses, out_eph_public_key, txkey_pub, additional_tx_public_keys, output_index, ephemeral_keys, proof.key_image, hwdev))
          {
            LOG_ERROR("Key image generation failed for staking TX!");
            return false;
          }

          crypto::public_key const *out_eph_public_key_ptr = &out_eph_public_key;
          crypto::generate_ring_signature((const crypto::hash&)proof.key_image, proof.key_image, &out_eph_public_key_ptr, 1, ephemeral_keys.sec, 0, &proof.signature);
          key_image_proofs.proofs.push_back(proof);
        }
      }

      tx_out out;
      out.amount = dst_entr.amount;
      txout_to_key tk;
      tk.key = out_eph_public_key;
      out.target = tk;
      tx.vout.push_back(out);
      output_index++;
      summary_outs_money += dst_entr.amount;
    }
    CHECK_AND_ASSERT_MES(additional_tx_public_keys.size() == additional_tx_keys.size(), false, "Internal error creating additional public keys");

    if (tx_params.v3_is_staking_tx)
    {
      CHECK_AND_ASSERT_MES(key_image_proofs.proofs.size() >= 1, false, "No key image proofs were generated for staking tx");
      add_tx_key_image_proofs_to_tx_extra(tx.extra, key_image_proofs);
    }

    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_additional_pub_keys));

    LOG_PRINT_L2("tx pubkey: " << txkey_pub);
    if (need_additional_txkeys)
    {
      LOG_PRINT_L2("additional tx pubkeys: ");
      for (size_t i = 0; i < additional_tx_public_keys.size(); ++i)
        LOG_PRINT_L2(additional_tx_public_keys[i]);
      add_additional_tx_pub_keys_to_extra(tx.extra, additional_tx_public_keys);
    }

    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    //check money
    if(summary_outs_money > summary_inputs_money )
    {
      LOG_ERROR("Transaction inputs money ("<< summary_inputs_money << ") less than outputs money (" << summary_outs_money << ")");
      return false;
    }

    // check for watch only wallet
    bool zero_secret_key = true;
    for (size_t i = 0; i < sizeof(sender_account_keys.m_spend_secret_key); ++i)
      zero_secret_key &= (sender_account_keys.m_spend_secret_key.data[i] == 0);
    if (zero_secret_key)
    {
      MDEBUG("Null secret key, skipping signatures");
    }

    if (tx.version == 1)
    {
      //generate ring signatures
      crypto::hash tx_prefix_hash;
      get_transaction_prefix_hash(tx, tx_prefix_hash);

      std::stringstream ss_ring_s;
      size_t i = 0;
      for(const tx_source_entry& src_entr:  sources)
      {
        ss_ring_s << "pub_keys:" << ENDL;
        std::vector<const crypto::public_key*> keys_ptrs;
        std::vector<crypto::public_key> keys(src_entr.outputs.size());
        size_t ii = 0;
        for(const tx_source_entry::output_entry& o: src_entr.outputs)
        {
          keys[ii] = rct2pk(o.second.dest);
          keys_ptrs.push_back(&keys[ii]);
          ss_ring_s << o.second.dest << ENDL;
          ++ii;
        }

        tx.signatures.push_back(std::vector<crypto::signature>());
        std::vector<crypto::signature>& sigs = tx.signatures.back();
        sigs.resize(src_entr.outputs.size());
        if (!zero_secret_key)
          crypto::generate_ring_signature(tx_prefix_hash, boost::get<txin_to_key>(tx.vin[i]).k_image, keys_ptrs, in_contexts[i].in_ephemeral.sec, src_entr.real_output, sigs.data());
        ss_ring_s << "signatures:" << ENDL;
        std::for_each(sigs.begin(), sigs.end(), [&](const crypto::signature& s){ss_ring_s << s << ENDL;});
        ss_ring_s << "prefix_hash:" << tx_prefix_hash << ENDL << "in_ephemeral_key: " << in_contexts[i].in_ephemeral.sec << ENDL << "real_output: " << src_entr.real_output << ENDL;
        i++;
      }

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL << ss_ring_s.str());
    }
    else
    {
      size_t n_total_outs = sources[0].outputs.size(); // only for non-simple rct

      // the non-simple version is slightly smaller, but assumes all real inputs
      // are on the same index, so can only be used if there just one ring.
      bool use_simple_rct = sources.size() > 1 || rct_config.range_proof_type != rct::RangeProofBorromean;

      if (!use_simple_rct)
      {
        // non simple ringct requires all real inputs to be at the same index for all inputs
        for(const tx_source_entry& src_entr:  sources)
        {
          if(src_entr.real_output != sources.begin()->real_output)
          {
            LOG_ERROR("All inputs must have the same index for non-simple ringct");
            return false;
          }
        }

        // enforce same mixin for all outputs
        for (size_t i = 1; i < sources.size(); ++i) {
          if (n_total_outs != sources[i].outputs.size()) {
            LOG_ERROR("Non-simple ringct transaction has varying ring size");
            return false;
          }
        }
      }

      uint64_t amount_in = 0, amount_out = 0;
      rct::ctkeyV inSk;
      inSk.reserve(sources.size());
      // mixRing indexing is done the other way round for simple
      rct::ctkeyM mixRing(use_simple_rct ? sources.size() : n_total_outs);
      rct::keyV destinations;
      std::vector<uint64_t> inamounts, outamounts;
      std::vector<unsigned int> index;
      std::vector<rct::multisig_kLRki> kLRki;
      for (size_t i = 0; i < sources.size(); ++i)
      {
        rct::ctkey ctkey;
        amount_in += sources[i].amount;
        inamounts.push_back(sources[i].amount);
        index.push_back(sources[i].real_output);
        // inSk: (secret key, mask)
        ctkey.dest = rct::sk2rct(in_contexts[i].in_ephemeral.sec);
        ctkey.mask = sources[i].mask;
        inSk.push_back(ctkey);
        memwipe(&ctkey, sizeof(rct::ctkey));
        // inPk: (public key, commitment)
        // will be done when filling in mixRing
        if (msout)
        {
          kLRki.push_back(sources[i].multisig_kLRki);
        }
      }
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        destinations.push_back(rct::pk2rct(boost::get<txout_to_key>(tx.vout[i].target).key));
        outamounts.push_back(tx.vout[i].amount);
        amount_out += tx.vout[i].amount;
      }

      if (use_simple_rct)
      {
        // mixRing indexing is done the other way round for simple
        for (size_t i = 0; i < sources.size(); ++i)
        {
          mixRing[i].resize(sources[i].outputs.size());
          for (size_t n = 0; n < sources[i].outputs.size(); ++n)
          {
            mixRing[i][n] = sources[i].outputs[n].second;
          }
        }
      }
      else
      {
        for (size_t i = 0; i < n_total_outs; ++i) // same index assumption
        {
          mixRing[i].resize(sources.size());
          for (size_t n = 0; n < sources.size(); ++n)
          {
            mixRing[i][n] = sources[n].outputs[i].second;
          }
        }
      }

      // fee
      if (!use_simple_rct && amount_in > amount_out)
        outamounts.push_back(amount_in - amount_out);

      // zero out all amounts to mask rct outputs, real amounts are now encrypted
      for (size_t i = 0; i < tx.vin.size(); ++i)
      {
        if (sources[i].rct)
          boost::get<txin_to_key>(tx.vin[i]).amount = 0;
      }
      for (size_t i = 0; i < tx.vout.size(); ++i)
        tx.vout[i].amount = 0;

      crypto::hash tx_prefix_hash;
      get_transaction_prefix_hash(tx, tx_prefix_hash);
      rct::ctkeyV outSk;
      if (use_simple_rct)
        tx.rct_signatures = rct::genRctSimple(rct::hash2rct(tx_prefix_hash), inSk, destinations, inamounts, outamounts, amount_in - amount_out, mixRing, amount_keys, msout ? &kLRki : NULL, msout, index, outSk, rct_config, hwdev);
      else
        tx.rct_signatures = rct::genRct(rct::hash2rct(tx_prefix_hash), inSk, destinations, outamounts, mixRing, amount_keys, msout ? &kLRki[0] : NULL, msout, sources[0].real_output, outSk, rct_config, hwdev); // same index assumption
      memwipe(inSk.data(), inSk.size() * sizeof(rct::ctkey));

      CHECK_AND_ASSERT_MES(tx.vout.size() == outSk.size(), false, "outSk size does not match vout");

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL);
    }

    tx.invalidate_hashes();

    return true;
  }
  //---------------------------------------------------------------
  bool construct_tx_and_get_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, crypto::secret_key &tx_key, std::vector<crypto::secret_key> &additional_tx_keys, const rct::RCTConfig &rct_config, rct::multisig_out *msout, antd_construct_tx_params const tx_params)
  {
    hw::device &hwdev = sender_account_keys.get_device();
    hwdev.open_tx(tx_key);

    try {
      // figure out if we need to make additional tx pubkeys
      size_t num_stdaddresses = 0;
      size_t num_subaddresses = 0;
      account_public_address single_dest_subaddress;
      classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);
      bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
      if (need_additional_txkeys)
      {
        additional_tx_keys.clear();
        for (const auto &d: destinations)
          additional_tx_keys.push_back(keypair::generate(sender_account_keys.get_device()).sec);
      }

      bool r = construct_tx_with_tx_key(sender_account_keys, subaddresses, sources, destinations, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys, rct_config, msout, true /*shuffle_outs*/, tx_params);
      hwdev.close_tx();
      return r;
    } catch(...) {
      hwdev.close_tx();
      throw;
    }
  }
  //---------------------------------------------------------------
  bool construct_tx(const account_keys& sender_account_keys, std::vector<tx_source_entry> &sources, const std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::tx_destination_entry>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, uint8_t hf_version, bool is_staking)
  {
     std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
     subaddresses[sender_account_keys.m_account_address.m_spend_public_key] = {0,0};
     crypto::secret_key tx_key;
     std::vector<crypto::secret_key> additional_tx_keys;
     std::vector<tx_destination_entry> destinations_copy = destinations;

     rct::RCTConfig rct_config    = {};
     rct_config.range_proof_type  = (hf_version < network_version_10_bulletproofs) ?  rct::RangeProofBorromean : rct::RangeProofPaddedBulletproof;
     rct_config.bp_version        = (hf_version < HF_VERSION_SMALLER_BP) ? 1 : 0;

     antd_construct_tx_params tx_params(hf_version);
     tx_params.v3_is_staking_tx = is_staking;

     return construct_tx_and_get_tx_key(sender_account_keys, subaddresses, sources, destinations_copy, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys, rct_config, NULL, tx_params);
  }
  //---------------------------------------------------------------
  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    )
  {
    //genesis block
    bl = boost::value_initialized<block>();

    blobdata tx_bl;
    bool r = string_tools::parse_hexstr_to_binbuff(genesis_tx, tx_bl);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    r = parse_and_validate_tx_from_blob(tx_bl, bl.miner_tx);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    bl.major_version = 7;
    bl.minor_version = 7;
    bl.timestamp = 0;
    bl.nonce = nonce;
    miner::find_nonce_for_given_block(NULL, bl, 1, 0);
    bl.invalidate_hashes();
    return true;
  }
  //---------------------------------------------------------------
  void get_altblock_longhash(const block& b, crypto::hash& res, const uint64_t main_height, const uint64_t height, const uint64_t seed_height, const crypto::hash& seed_hash)
  {
    blobdata bd = get_block_hashing_blob(b);
    rx_slow_hash(main_height, seed_height, seed_hash.data, bd.data(), bd.size(), res.data, 0, 1);
  }

  bool get_block_longhash(const Blockchain *pbc, const block& b, crypto::hash& res, const uint64_t height, const int miners)
  {
    blobdata bd = get_block_hashing_blob(b);
    if (b.major_version >= RX_BLOCK_VERSION)
    {
      uint64_t seed_height, main_height;
      crypto::hash hash;
      if (pbc != NULL)
      {
        seed_height = rx_seedheight(height);
        hash = pbc->get_pending_block_id_by_height(seed_height);
        main_height = pbc->get_current_blockchain_height();
      } else
      {
        memset(&hash, 0, sizeof(hash));  // only happens when generating genesis block
        seed_height = 0;
        main_height = 0;
      }
      rx_slow_hash(main_height, seed_height, hash.data, bd.data(), bd.size(), res.data, miners, 0);
    } else {
      const int pow_variant = b.major_version >= 7 ? b.major_version - 6 : 0;
      crypto::cn_slow_hash(bd.data(), bd.size(), res, pow_variant, height);
    }
    return true;
  }

  crypto::hash get_block_longhash(const Blockchain *pbc, const block& b, const uint64_t height, const int miners)
  {
    crypto::hash p = crypto::null_hash;
    get_block_longhash(pbc, b, p, height, miners);
    return p;
  }

  void get_block_longhash_reorg(const uint64_t split_height)
  {
    rx_reorg(split_height);
  }

}
