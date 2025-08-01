// Copyright (c)      2018-2024, The Oxen Project
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

#include <functional>
#include <random>
#include <algorithm>

#include "ringct/rctSigs.h"
#include "wallet/wallet2.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_basic/tx_extra.h"
#include "int-util.h"
#include "constants.h"
#include "common/scoped_message_writer.h"
#include "common/i18n.h"
#include "full_node_quorum_cop.h"

#include "full_node_list.h"
#include "full_node_rules.h"

#undef ANTD_DEFAULT_LOG_CATEGORY
#define ANTD_DEFAULT_LOG_CATEGORY "full_nodes"

namespace full_nodes
{
  static int get_min_full_node_info_version_for_hf(int hf_version)
  {
    if (hf_version >= cryptonote::network_version_7 && hf_version <= cryptonote::network_version_9_full_nodes)
      return full_node_info::version_0;

    if (hf_version == cryptonote::network_version_10_bulletproofs)
      return full_node_info::version_1_swarms;

    return full_node_info::version_2_infinite_staking;
  }

  static uint64_t uniform_distribution_portable(std::mt19937_64& mersenne_twister, uint64_t n)
  {
    uint64_t secureMax = mersenne_twister.max() - mersenne_twister.max() % n;
    uint64_t x;
    do x = mersenne_twister(); while (x >= secureMax);
    return  x / (secureMax / n);
  }

  full_node_list::full_node_list(cryptonote::Blockchain& blockchain)
    : m_blockchain(blockchain), m_hooks_registered(false), m_db(nullptr), m_full_node_pubkey(nullptr)
  {
    m_transient_state = {};
  }

  void full_node_list::register_hooks(full_nodes::quorum_cop &quorum_cop)
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    if (!m_hooks_registered)
    {
      m_hooks_registered = true;
      m_blockchain.hook_block_added(*this);
      m_blockchain.hook_blockchain_detached(*this);
      m_blockchain.hook_init(*this);
      m_blockchain.hook_validate_miner_tx(*this);

      // NOTE: There is an implicit dependency on fullnode lists hooks
      m_blockchain.hook_init(quorum_cop);
      m_blockchain.hook_block_added(quorum_cop);
      m_blockchain.hook_blockchain_detached(quorum_cop);
    }
  }

  void full_node_list::init()
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    if (m_blockchain.get_current_hard_fork_version() < 9)
    {
      clear(true);
      return;
    }

    uint64_t current_height = m_blockchain.get_current_blockchain_height();
    bool loaded = load();
    if (loaded && m_transient_state.height == current_height) return;

    if (!loaded || m_transient_state.height > current_height) clear(true);

    LOG_PRINT_L0("Recalculating fullnodes list, scanning blockchain from height " << m_transient_state.height);
    LOG_PRINT_L0("This may take some time...");

    std::vector<std::pair<cryptonote::blobdata, cryptonote::block>> blocks;
    for (uint64_t i = 0; m_transient_state.height < current_height; i++)
    {
      if (i > 0 && i % 10 == 0)
          LOG_PRINT_L0("... scanning height " << m_transient_state.height);

      blocks.clear();
      if (!m_blockchain.get_blocks(m_transient_state.height, 1000, blocks))
      {
        MERROR("Unable to initialize fullnodes list");
        return;
      }

      std::vector<cryptonote::transaction> txs;
      std::vector<crypto::hash> missed_txs;
      for (const auto& block_pair : blocks)
      {
        txs.clear();
        missed_txs.clear();

        const cryptonote::block& block = block_pair.second;
        std::vector<cryptonote::transaction> txs;
        std::vector<crypto::hash> missed_txs;
        if (!m_blockchain.get_transactions(block.tx_hashes, txs, missed_txs))
        {
          MERROR("Unable to get transactions for block " << block.hash);
          return;
        }

        block_added_generic(block, txs);
      }
    }
    LOG_PRINT_L0("Done recalculating fullnodes list");
  }

  std::vector<crypto::public_key> full_node_list::get_full_nodes_pubkeys() const
  {
    std::vector<crypto::public_key> result;
    for (const auto& iter : m_transient_state.full_nodes_infos)
      if (iter.second.is_fully_funded())
        result.push_back(iter.first);

    std::sort(result.begin(), result.end(),
      [](const crypto::public_key &a, const crypto::public_key &b) {
        return memcmp(reinterpret_cast<const void*>(&a), reinterpret_cast<const void*>(&b), sizeof(a)) < 0;
      });
    return result;
  }

  const std::shared_ptr<const quorum_state> full_node_list::get_quorum_state(uint64_t height) const
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    const auto &it = m_transient_state.quorum_states.find(height);
    if (it != m_transient_state.quorum_states.end())
    {
      return it->second;
    }

    return nullptr;
  }

  std::vector<full_node_pubkey_info> full_node_list::get_full_node_list_state(const std::vector<crypto::public_key> &full_node_pubkeys) const
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    std::vector<full_node_pubkey_info> result;

    if (full_node_pubkeys.empty())
    {
      result.reserve(m_transient_state.full_nodes_infos.size());

      for (const auto &it : m_transient_state.full_nodes_infos)
      {
        full_node_pubkey_info entry = {};
        entry.pubkey                   = it.first;
        entry.info                     = it.second;
        result.push_back(entry);
      }
    }
    else
    {
      result.reserve(full_node_pubkeys.size());
      for (const auto &it : full_node_pubkeys)
      {
        const auto &find_it = m_transient_state.full_nodes_infos.find(it);
        if (find_it == m_transient_state.full_nodes_infos.end())
          continue;

        full_node_pubkey_info entry = {};
        entry.pubkey                   = (*find_it).first;
        entry.info                     = (*find_it).second;
        result.push_back(entry);
      }
    }

    return result;
  }

  void full_node_list::set_db_pointer(cryptonote::BlockchainDB* db)
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    m_db = db;
  }

  void full_node_list::set_my_full_node_keys(crypto::public_key const *pub_key)
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    m_full_node_pubkey = pub_key;
  }

  bool full_node_list::is_full_node(const crypto::public_key& pubkey) const
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    return m_transient_state.full_nodes_infos.find(pubkey) != m_transient_state.full_nodes_infos.end();
  }

  bool full_node_list::is_key_image_locked(crypto::key_image const &check_image, uint64_t *unlock_height, full_node_info::contribution_t *the_locked_contribution) const
  {
    for (const auto& pubkey_info : m_transient_state.full_nodes_infos)
    {
      const full_node_info &info = pubkey_info.second;
      for (const full_node_info::contributor_t &contributor : info.contributors)
      {
        for (const full_node_info::contribution_t &contribution : contributor.locked_contributions)
        {
          if (check_image == contribution.key_image)
          {
            if (the_locked_contribution) *the_locked_contribution = contribution;
            if (unlock_height) *unlock_height = info.requested_unlock_height;
            return true;
          }
        }
      }
    }
    return false;
  }

  bool reg_tx_extract_fields(const cryptonote::transaction& tx, std::vector<cryptonote::account_public_address>& addresses, uint64_t& portions_for_operator, std::vector<uint64_t>& portions, uint64_t& expiration_timestamp, crypto::public_key& full_node_key, crypto::signature& signature)
  {
    cryptonote::tx_extra_full_node_register registration;
    if (!get_full_node_register_from_tx_extra(tx.extra, registration))
      return false;
    if (!cryptonote::get_full_node_pubkey_from_tx_extra(tx.extra, full_node_key))
      return false;

    addresses.clear();
    addresses.reserve(registration.m_public_spend_keys.size());
    for (size_t i = 0; i < registration.m_public_spend_keys.size(); i++)
      addresses.push_back(cryptonote::account_public_address{ registration.m_public_spend_keys[i], registration.m_public_view_keys[i] });

    portions_for_operator = registration.m_portions_for_operator;
    portions = registration.m_portions;
    expiration_timestamp = registration.m_expiration_timestamp;
    signature = registration.m_full_node_signature;
    return true;
  }

  struct parsed_tx_contribution
  {
    cryptonote::account_public_address address;
    uint64_t transferred;
    crypto::secret_key tx_key;
    std::vector<full_node_info::contribution_t> locked_contributions;
  };

  static uint64_t get_reg_tx_staking_output_contribution(const cryptonote::transaction& tx, int i, crypto::key_derivation const &derivation, hw::device& hwdev)
  {
    if (tx.vout[i].target.type() != typeid(cryptonote::txout_to_key))
    {
      return 0;
    }

    rct::key mask;
    uint64_t money_transferred = 0;

    crypto::secret_key scalar1;
    hwdev.derivation_to_scalar(derivation, i, scalar1);
    try
    {
      switch (tx.rct_signatures.type)
      {
      case rct::RCTTypeSimple:
      case rct::RCTTypeBulletproof:
      case rct::RCTTypeBulletproof2:
      case rct::RCTTypeCLSAG:
        money_transferred = rct::decodeRctSimple(tx.rct_signatures, rct::sk2rct(scalar1), i, mask, hwdev);
        break;
      case rct::RCTTypeFull:
        money_transferred = rct::decodeRct(tx.rct_signatures, rct::sk2rct(scalar1), i, mask, hwdev);
        break;
      default:
        LOG_PRINT_L0("Unsupported rct type: " << tx.rct_signatures.type);
        return 0;
      }
    }
    catch (const std::exception &e)
    {
      LOG_PRINT_L0("Failed to decode input " << i);
      return 0;
    }

    return money_transferred;
  }

  bool full_node_list::process_deregistration_tx(const cryptonote::transaction& tx, uint64_t block_height)
  {
    if (tx.get_type() != cryptonote::transaction::type_deregister)
      return false;

    cryptonote::tx_extra_full_node_deregister deregister;
    if (!cryptonote::get_full_node_deregister_from_tx_extra(tx.extra, deregister))
    {
      MERROR("Transaction deregister did not have deregister data in tx extra, possibly corrupt tx in blockchain");
      return false;
    }

    const auto state = get_quorum_state(deregister.block_height);

    if (!state)
    {
      // TODO(antd): Not being able to find a quorum is fatal! We want better caching abilities.
      MERROR("Quorum state for height: " << deregister.block_height << ", was not stored by the daemon");
      return false;
    }

    if (deregister.full_node_index >= state->nodes_to_test.size())
    {
      MERROR("Fullnode index to vote off has become invalid, quorum rules have changed without a hardfork.");
      return false;
    }

    const crypto::public_key& key = state->nodes_to_test[deregister.full_node_index];

    auto iter = m_transient_state.full_nodes_infos.find(key);
    if (iter == m_transient_state.full_nodes_infos.end())
      return false;

    if (m_full_node_pubkey && *m_full_node_pubkey == key)
    {
      MGINFO_RED("Deregistration for fullnode (yours): " << key);
    }
    else
    {
      LOG_PRINT_L1("Deregistration for fullnode: " << key);
    }

    m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_change(block_height, key, iter->second)));

    int hard_fork_version = m_blockchain.get_hard_fork_version(block_height);
    if (hard_fork_version >= cryptonote::network_version_11_infinite_staking)
    {
      for (const auto &contributor : iter->second.contributors)
      {
        for (const auto &contribution : contributor.locked_contributions)
        {
          key_image_blacklist_entry entry = {};
          entry.key_image                 = contribution.key_image;
          entry.unlock_height             = block_height + staking_num_lock_blocks(m_blockchain.nettype());
          m_transient_state.key_image_blacklist.push_back(entry);

          const bool adding_to_blacklist = true;
          m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_key_image_blacklist(block_height, entry, adding_to_blacklist)));
        }
      }
    }

    m_transient_state.full_nodes_infos.erase(iter);
    return true;
  }

  static uint64_t get_new_swarm_id(std::mt19937_64& mt, const std::vector<swarm_id_t>& ids)
  {
    uint64_t id_new = QUEUE_SWARM_ID;

    while (id_new == QUEUE_SWARM_ID || ( std::find(ids.begin(), ids.end(), id_new) != ids.end() )) {
      id_new = uniform_distribution_portable(mt, UINT64_MAX);
    }

    return id_new;
  }

  static std::vector<swarm_id_t> get_all_swarms(const std::map<swarm_id_t, std::vector<crypto::public_key>>& swarm_to_snodes)
  {
    std::vector<swarm_id_t> all_swarms;
    all_swarms.reserve(swarm_to_snodes.size());
    for (const auto& entry : swarm_to_snodes) {
      all_swarms.push_back(entry.first);
    }
    return all_swarms;
  }

  static crypto::public_key pop_random_snode(std::mt19937_64& mt, std::vector<crypto::public_key>& vec)
  {
    const auto idx = uniform_distribution_portable(mt, vec.size());
    const auto sn_pk = vec.at(idx);
    auto it = vec.begin();
    std::advance(it, idx);
    vec.erase(it);
    return sn_pk;
  }


  static void calc_swarm_changes(std::map<swarm_id_t, std::vector<crypto::public_key>>& swarm_to_snodes, uint64_t seed)
  {
    std::mt19937_64 mersenne_twister(seed);

    std::vector<crypto::public_key> swarm_buffer = swarm_to_snodes[QUEUE_SWARM_ID];
    swarm_to_snodes.erase(QUEUE_SWARM_ID);

    auto all_swarms = get_all_swarms(swarm_to_snodes);
    std::sort(all_swarms.begin(), all_swarms.end());

    antd_shuffle(all_swarms, seed);

    const auto cmp_swarm_sizes =
      [&swarm_to_snodes](swarm_id_t lhs, swarm_id_t rhs) {
        return swarm_to_snodes.at(lhs).size() < swarm_to_snodes.at(rhs).size();
      };

    /// 1. If there are any swarms that are about to dissapear -> try to fill them first
    std::vector<swarm_id_t> starving_swarms;
    {
      std::copy_if(all_swarms.begin(),
                   all_swarms.end(),
                   std::back_inserter(starving_swarms),
                   [&swarm_to_snodes](swarm_id_t id) { return swarm_to_snodes.at(id).size() < MIN_SWARM_SIZE; });

      for (const auto swarm_id : starving_swarms) {

        const size_t needed = MIN_SWARM_SIZE - swarm_to_snodes.at(swarm_id).size();

        for (auto j = 0u; j < needed && !swarm_buffer.empty(); ++j) {
          const auto sn_pk = pop_random_snode(mersenne_twister, swarm_buffer);
          swarm_to_snodes.at(swarm_id).push_back(sn_pk);
        }

        if (swarm_buffer.empty()) break;
      }
    }

    /// 2. Any starving swarms still left? If yes, steal nodes from larger swarms
    {
      bool can_continue = true; /// whether there are still large swarms to steal from
      for (const auto swarm_id : starving_swarms) {

        if (swarm_to_snodes.at(swarm_id).size() == MIN_SWARM_SIZE) continue;

        const auto needed = MIN_SWARM_SIZE - swarm_to_snodes.at(swarm_id).size();

          for (auto i = 0u; i < needed; ++i) {

            const auto large_swarm =
              *std::max_element(all_swarms.begin(), all_swarms.end(), cmp_swarm_sizes);

            if (swarm_to_snodes.at(large_swarm).size() <= MIN_SWARM_SIZE) {
              can_continue = false;
              break;
            }

            const crypto::public_key sn_pk = pop_random_snode(mersenne_twister, swarm_to_snodes.at(large_swarm));
            swarm_to_snodes.at(swarm_id).push_back(sn_pk);
        }

        if (!can_continue) break;
      }

    }

    /// 3. Fill in "unsaturated" swarms (with fewer than max nodes) starting from smallest
    {
      while (!swarm_buffer.empty() && !all_swarms.empty()) {

        const swarm_id_t smallest_swarm = *std::min_element(all_swarms.begin(), all_swarms.end(), cmp_swarm_sizes);

        std::vector<crypto::public_key>& swarm = swarm_to_snodes.at(smallest_swarm);

        if (swarm.size() == MAX_SWARM_SIZE) break;

        const auto sn_pk = pop_random_snode(mersenne_twister, swarm_buffer);
        swarm.push_back(sn_pk);
      }
    }

    /// 4. If there are still enough nodes for MAX_SWARM_SIZE + some safety buffer, create a new swarm
    while (swarm_buffer.size() >= MAX_SWARM_SIZE + SWARM_BUFFER) {

      /// shuffle the queue and select MAX_SWARM_SIZE last elements
      const auto new_swarm_id = get_new_swarm_id(mersenne_twister, all_swarms);

      antd_shuffle(swarm_buffer, seed + new_swarm_id);

      std::vector<crypto::public_key> selected_snodes;

      for (auto i = 0u; i < MAX_SWARM_SIZE; ++i) {

        /// get next node from the buffer
        const crypto::public_key fresh_snode = swarm_buffer.back();
        swarm_buffer.pop_back();

        /// Try replacing nodes in existing swarms
        if (swarm_to_snodes.size() > 0) {
          /// a. Select a random swarm
          const uint64_t swarm_idx = uniform_distribution_portable(mersenne_twister, swarm_to_snodes.size());
          auto it = swarm_to_snodes.begin();
          std::advance(it, swarm_idx);
          std::vector<crypto::public_key>& selected_swarm = it->second;

          /// b. Select a random snode
          const crypto::public_key snode = pop_random_snode(mersenne_twister, selected_swarm);

          /// c. Swap that node with a node in the queue, the old node will form a new swarm
          selected_snodes.push_back(snode);
          selected_swarm.push_back(fresh_snode);
        } else {
          /// If there are no existing swarms, create the first swarm directly from the queue
          selected_snodes.push_back(fresh_snode);
        }
      }

      swarm_to_snodes.insert({new_swarm_id, std::move(selected_snodes)});
    }

    /// 5. If there is a swarm with less than MIN_SWARM_SIZE, decommission that swarm (should almost never happen due to the safety buffer).
    for (auto entry : swarm_to_snodes) {
      if (entry.second.size() < MIN_SWARM_SIZE) {
        LOG_PRINT_L1("swarm " << entry.first << " is DECOMMISSIONED");
        /// TODO: move data to other swarms, then put snodes back in the queue
      }
    }

    /// 6. Put nodes from the buffer back to the "buffer agnostic" data structure
    swarm_to_snodes.insert({QUEUE_SWARM_ID, std::move(swarm_buffer)});
  }

  void full_node_list::update_swarms(uint64_t height) {

    crypto::hash hash = m_blockchain.get_block_id_by_height(height);
    uint64_t seed = 0;
    std::memcpy(&seed, hash.data, sizeof(seed));

    /// Gather existing swarms from infos
    std::map<swarm_id_t, std::vector<crypto::public_key>> existing_swarms;

    for (const auto& entry : m_transient_state.full_nodes_infos) {
      const auto id = entry.second.swarm_id;
      existing_swarms[id].push_back(entry.first);
    }

    calc_swarm_changes(existing_swarms, seed);

    /// Apply changes
    for (const auto entry : existing_swarms) {

      const swarm_id_t swarm_id = entry.first;
      const std::vector<crypto::public_key>& snodes = entry.second;

      for (const auto snode : snodes) {

        auto& sn_info = m_transient_state.full_nodes_infos.at(snode);
        if (sn_info.swarm_id == swarm_id) continue; /// nothing changed for this snode

        /// modify info and record the change
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_change(height, snode, sn_info)));
        sn_info.swarm_id = swarm_id;
      }

    }
  }

  static bool get_contribution(cryptonote::network_type nettype, int hard_fork_version, const cryptonote::transaction& tx, uint64_t block_height, parsed_tx_contribution &parsed_contribution)
  {
    if (!cryptonote::get_full_node_contributor_from_tx_extra(tx.extra, parsed_contribution.address))
      return false;

    if (!cryptonote::get_tx_secret_key_from_tx_extra(tx.extra, parsed_contribution.tx_key))
    {
      LOG_PRINT_L1("Contribution TX: There was a fullnode contributor but no secret key in the tx extra on height: " << block_height << " for tx: " << get_transaction_hash(tx));
      return false;
    }

    crypto::key_derivation derivation;
    if (!crypto::generate_key_derivation(parsed_contribution.address.m_view_public_key, parsed_contribution.tx_key, derivation))
    {
      LOG_PRINT_L1("Contribution TX: Failed to generate key derivation on height: " << block_height << " for tx: " << get_transaction_hash(tx));
      return false;
    }

    hw::device& hwdev               = hw::get_device("default");
    parsed_contribution.transferred = 0;

    if (hard_fork_version >= cryptonote::network_version_11_infinite_staking)
    {
      cryptonote::tx_extra_tx_key_image_proofs key_image_proofs;
      if (!get_tx_key_image_proofs_from_tx_extra(tx.extra, key_image_proofs))
      {
        LOG_PRINT_L1("Contribution TX: Didn't have key image proofs in the tx_extra, rejected on height: " << block_height << " for tx: " << get_transaction_hash(tx));
        return false;
      }

      for (size_t output_index = 0; output_index < tx.vout.size(); ++output_index)
      {
        uint64_t transferred = get_reg_tx_staking_output_contribution(tx, output_index, derivation, hwdev);
        if (transferred == 0)
          continue;

        crypto::public_key ephemeral_pub_key;
        {
          if (!hwdev.derive_public_key(derivation, output_index, parsed_contribution.address.m_spend_public_key, ephemeral_pub_key))
          {
            LOG_PRINT_L1("Contribution TX: Could not derive TX ephemeral key on height: " << block_height << " for tx: " << get_transaction_hash(tx) << " for output: " << output_index);
            continue;
          }

          const auto& out_to_key = boost::get<cryptonote::txout_to_key>(tx.vout[output_index].target);
          if (out_to_key.key != ephemeral_pub_key)
          {
            LOG_PRINT_L1("Contribution TX: Derived TX ephemeral key did not match tx stored key on height: " << block_height << " for tx: " << get_transaction_hash(tx) << " for output: " << output_index);
            continue;
          }
        }

        crypto::public_key const *ephemeral_pub_key_ptr = &ephemeral_pub_key;
        for (auto proof = key_image_proofs.proofs.begin(); proof != key_image_proofs.proofs.end(); proof++)
        {
          if (!crypto::check_ring_signature((const crypto::hash &)(proof->key_image), proof->key_image, &ephemeral_pub_key_ptr, 1, &proof->signature))
            continue;

          full_node_info::contribution_t entry = {};
          entry.key_image_pub_key                 = ephemeral_pub_key;
          entry.key_image                         = proof->key_image;
          entry.amount                            = transferred;

          parsed_contribution.locked_contributions.push_back(entry);
          parsed_contribution.transferred += transferred;
          key_image_proofs.proofs.erase(proof);
          break;
        }
      }
    }
    else
    {
      for (size_t i = 0; i < tx.vout.size(); i++)
      {
        bool has_correct_unlock_time = false;
        {
          uint64_t unlock_time = tx.unlock_time;
          if (tx.version >= cryptonote::transaction::version_3_per_output_unlock_times)
            unlock_time = tx.output_unlock_times[i];

          uint64_t min_height = block_height + staking_num_lock_blocks(nettype);
          has_correct_unlock_time = unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER && unlock_time >= min_height;
        }

        if (has_correct_unlock_time)
          parsed_contribution.transferred += get_reg_tx_staking_output_contribution(tx, i, derivation, hwdev);
      }
    }

    return true;
  }

  bool full_node_list::is_registration_tx(const cryptonote::transaction& tx, uint64_t block_timestamp, uint64_t block_height, uint32_t index, crypto::public_key& key, full_node_info& info) const
  {
    crypto::public_key full_node_key;
    std::vector<cryptonote::account_public_address> full_node_addresses;
    std::vector<uint64_t> full_node_portions;
    uint64_t portions_for_operator;
    uint64_t expiration_timestamp;
    crypto::signature signature;

    if (!reg_tx_extract_fields(tx, full_node_addresses, portions_for_operator, full_node_portions, expiration_timestamp, full_node_key, signature))
      return false;

    if (full_node_portions.size() != full_node_addresses.size() || full_node_portions.empty())
    {
      LOG_PRINT_L1("Register TX: Extracted portions size: (" << full_node_portions.size() <<
                   ") was empty or did not match address size: (" << full_node_addresses.size() <<
                   ") on height: " << block_height <<
                   " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    int hf_version = m_blockchain.get_hard_fork_version(block_height);

    if (!check_full_node_portions(hf_version, full_node_portions)) return false;

    if (portions_for_operator > STAKING_PORTIONS)
    {
      LOG_PRINT_L1("Register TX: Operator portions: " << portions_for_operator <<
                   " exceeded staking portions: " << STAKING_PORTIONS <<
                   " on height: " << block_height <<
                   " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    // check the signature is all good

    crypto::hash hash;
    if (!get_registration_hash(full_node_addresses, portions_for_operator, full_node_portions, expiration_timestamp, hash))
    {
      LOG_PRINT_L1("Register TX: Failed to extract registration hash, on height: " << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    if (!crypto::check_key(full_node_key) || !crypto::check_signature(hash, full_node_key, signature))
    {
      LOG_PRINT_L1("Register TX: Has invalid key and/or signature, on height: " << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    if (expiration_timestamp < block_timestamp)
    {
      LOG_PRINT_L1("Register TX: Has expired. The block timestamp: " << block_timestamp <<
                   " is greater than the expiration timestamp: " << expiration_timestamp <<
                   " on height: " << block_height <<
                   " for tx:" << cryptonote::get_transaction_hash(tx));
      return false;
    }

    // check the initial contribution exists

    info.staking_requirement = get_staking_requirement(m_blockchain.nettype(), block_height, hf_version);

    cryptonote::account_public_address address;

    parsed_tx_contribution parsed_contribution = {};
    if (!get_contribution(m_blockchain.nettype(), hf_version, tx, block_height, parsed_contribution))
    {
      LOG_PRINT_L1("Register TX: Had fullnode registration fields, but could not decode contribution on height: " << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    const uint64_t min_transfer = get_min_node_contribution(hf_version, info.staking_requirement, info.total_reserved, info.total_num_locked_contributions());
    if (parsed_contribution.transferred < min_transfer)
    {
      LOG_PRINT_L1("Register TX: Contribution transferred: " << parsed_contribution.transferred << " didn't meet the minimum transfer requirement: " << min_transfer << " on height: " << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    size_t total_num_of_addr = full_node_addresses.size();
    if (std::find(full_node_addresses.begin(), full_node_addresses.end(), parsed_contribution.address) == full_node_addresses.end())
      total_num_of_addr++;

    if (total_num_of_addr > MAX_NUMBER_OF_CONTRIBUTORS)
    {
      LOG_PRINT_L1("Register TX: Number of participants: " << total_num_of_addr <<
                   " exceeded the max number of contributors: " << MAX_NUMBER_OF_CONTRIBUTORS <<
                   " on height: " << block_height <<
                   " for tx: " << cryptonote::get_transaction_hash(tx));
      return false;
    }

    // don't actually process this contribution now, do it when we fall through later.

    key = full_node_key;

    info.operator_address = full_node_addresses[0];
    info.portions_for_operator = portions_for_operator;
    info.registration_height = block_height;
    info.last_reward_block_height = block_height;
    info.last_reward_transaction_index = index;
    info.total_contributed = 0;
    info.total_reserved = 0;
    info.version = get_min_full_node_info_version_for_hf(hf_version);

    if (info.version >= full_node_info::version_1_swarms)
      info.swarm_id = QUEUE_SWARM_ID; /// new nodes go into a "queue swarm"

    info.contributors.clear();

    for (size_t i = 0; i < full_node_addresses.size(); i++)
    {
      // Check for duplicates
      auto iter = std::find(full_node_addresses.begin(), full_node_addresses.begin() + i, full_node_addresses[i]);
      if (iter != full_node_addresses.begin() + i)
      {
        LOG_PRINT_L1("Register TX: There was a duplicate participant for fullnode on height: " << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
        return false;
      }

      uint64_t hi, lo, resulthi, resultlo;
      lo = mul128(info.staking_requirement, full_node_portions[i], &hi);
      div128_64(hi, lo, STAKING_PORTIONS, &resulthi, &resultlo);

      full_node_info::contributor_t contributor = {};
      contributor.version                          = get_min_full_node_info_version_for_hf(hf_version);
      contributor.reserved                         = resultlo;
      contributor.address                          = full_node_addresses[i];
      info.contributors.push_back(contributor);
      info.total_reserved += resultlo;
    }

    return true;
  }

  bool full_node_list::process_registration_tx(const cryptonote::transaction& tx, uint64_t block_timestamp, uint64_t block_height, uint32_t index)
  {
    crypto::public_key key;
    full_node_info info = {};
    if (!is_registration_tx(tx, block_timestamp, block_height, index, key, info))
      return false;

    int hard_fork_version = m_blockchain.get_hard_fork_version(block_height);
    if (hard_fork_version >= cryptonote::network_version_11_infinite_staking)
    {
      // NOTE(antd): Grace period is not used anymore with infinite staking. So, if someone somehow reregisters, we just ignore it
      const auto iter = m_transient_state.full_nodes_infos.find(key);
      if (iter != m_transient_state.full_nodes_infos.end())
        return false;

      if (m_full_node_pubkey && *m_full_node_pubkey == key) MGINFO_GREEN("Fullnode registered (yours): " << key << " on height: " << block_height);
      else                                                        LOG_PRINT_L1("New fullnode registered: "     << key << " on height: " << block_height);
    }
    else
    {
      // NOTE: A node doesn't expire until registration_height + lock blocks excess now which acts as the grace period
      // So it is possible to find the node still in our list.
      bool registered_during_grace_period = false;
      const auto iter = m_transient_state.full_nodes_infos.find(key);
      if (iter != m_transient_state.full_nodes_infos.end())
      {
        if (hard_fork_version >= cryptonote::network_version_10_bulletproofs)
        {
          full_node_info const &old_info = iter->second;
          uint64_t expiry_height = old_info.registration_height + staking_num_lock_blocks(m_blockchain.nettype());
          if (block_height < expiry_height)
            return false;

          // NOTE: Node preserves its position in list if it reregisters during grace period.
          registered_during_grace_period = true;
          info.last_reward_block_height = old_info.last_reward_block_height;
          info.last_reward_transaction_index = old_info.last_reward_transaction_index;
        }
        else
        {
          return false;
        }
      }

      if (m_full_node_pubkey && *m_full_node_pubkey == key)
      {
        if (registered_during_grace_period)
        {
          MGINFO_GREEN("Fullnode re-registered (yours): " << key << " at block height: " << block_height);
        }
        else
        {
          MGINFO_GREEN("Fullnode registered (yours): " << key << " at block height: " << block_height);
        }
      }
      else
      {
        LOG_PRINT_L1("New fullnode registered: " << key << " at block height: " << block_height);
      }
    }

    m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_new(block_height, key)));
    m_transient_state.full_nodes_infos[key] = info;
    return true;
  }

  void full_node_list::process_contribution_tx(const cryptonote::transaction& tx, uint64_t block_height, uint32_t index)
  {
    crypto::public_key pubkey;

    if (!cryptonote::get_full_node_pubkey_from_tx_extra(tx.extra, pubkey))
      return; // Is not a contribution TX don't need to check it.

    parsed_tx_contribution parsed_contribution = {};
    const int hf_version = m_blockchain.get_hard_fork_version(block_height);
    if (!get_contribution(m_blockchain.nettype(), hf_version, tx, block_height, parsed_contribution))
    {
      LOG_PRINT_L1("Contribution TX: Could not decode contribution for fullnode: " << pubkey << " on height: " << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
      return;
    }

    /// Fullnode must be registered
    auto iter = m_transient_state.full_nodes_infos.find(pubkey);
    if (iter == m_transient_state.full_nodes_infos.end())
    {
      LOG_PRINT_L1("Contribution TX: Contribution received for fullnode: " << pubkey <<
                   ", but could not be found in the fullnode list on height: " << block_height <<
                   " for tx: " << cryptonote::get_transaction_hash(tx )<< "\n"
                   "This could mean that the fullnode was deregistered before the contribution was processed.");
      return;
    }

    full_node_info& info = iter->second;
    if (info.is_fully_funded())
    {
      LOG_PRINT_L1("Contribution TX: Fullnode: " << pubkey <<
                   " is already fully funded, but contribution received on height: "  << block_height <<
                   " for tx: " << cryptonote::get_transaction_hash(tx));
      return;
    }

    if (!cryptonote::get_tx_secret_key_from_tx_extra(tx.extra, parsed_contribution.tx_key))
    {
      LOG_PRINT_L1("Contribution TX: Failed to get tx secret key from contribution received on height: "  << block_height << " for tx: " << cryptonote::get_transaction_hash(tx));
      return;
    }

    auto& contributors = info.contributors;

    auto contrib_iter = std::find_if(contributors.begin(), contributors.end(),
        [&parsed_contribution](const full_node_info::contributor_t& contributor) { return contributor.address == parsed_contribution.address; });
    const bool new_contributor = (contrib_iter == contributors.end());

    if (new_contributor)
    {
      if (contributors.size() >= MAX_NUMBER_OF_CONTRIBUTORS)
      {
        LOG_PRINT_L1("Contribution TX: Node is full with max contributors: " << MAX_NUMBER_OF_CONTRIBUTORS <<
                     " for fullnode: " << pubkey <<
                     " on height: "  << block_height <<
                     " for tx: " << cryptonote::get_transaction_hash(tx));
        return;
      }

      /// Check that the contribution is large enough
      const uint8_t hf_version = m_blockchain.get_hard_fork_version(block_height);
      const uint64_t min_contribution = get_min_node_contribution(hf_version, info.staking_requirement, info.total_reserved, info.total_num_locked_contributions());
      if (parsed_contribution.transferred < min_contribution)
      {
        LOG_PRINT_L1("Contribution TX: Amount " << parsed_contribution.transferred <<
                     " did not meet min " << min_contribution <<
                     " for fullnode: " << pubkey <<
                     " on height: "  << block_height <<
                     " for tx: " << cryptonote::get_transaction_hash(tx));
        return;
      }
    }

    //
    // Successfully Validated
    //

    m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_change(block_height, pubkey, info)));
    if (new_contributor)
    {
      full_node_info::contributor_t new_contributor = {};
      new_contributor.version                          = get_min_full_node_info_version_for_hf(hf_version);
      new_contributor.address                          = parsed_contribution.address;
      info.contributors.push_back(new_contributor);
      contrib_iter = --contributors.end();
    }

    full_node_info::contributor_t& contributor = *contrib_iter;

    // In this action, we cannot
    // increase total_reserved so much that it is >= staking_requirement
    uint64_t can_increase_reserved_by = info.staking_requirement - info.total_reserved;
    uint64_t max_amount               = contributor.reserved + can_increase_reserved_by;
    parsed_contribution.transferred = std::min(max_amount - contributor.amount, parsed_contribution.transferred);

    contributor.amount     += parsed_contribution.transferred;
    info.total_contributed += parsed_contribution.transferred;

    if (contributor.amount > contributor.reserved)
    {
      info.total_reserved += contributor.amount - contributor.reserved;
      contributor.reserved = contributor.amount;
    }

    info.last_reward_block_height = block_height;
    info.last_reward_transaction_index = index;

    const size_t max_contributions_per_node = full_nodes::MAX_KEY_IMAGES_PER_CONTRIBUTOR * MAX_NUMBER_OF_CONTRIBUTORS;
    if (hf_version >= cryptonote::network_version_11_infinite_staking)
    {
      std::vector<full_node_info::contribution_t> &locked_contributions = contributor.locked_contributions;

      for (const full_node_info::contribution_t &contribution : parsed_contribution.locked_contributions)
      {
        if (info.total_num_locked_contributions() < max_contributions_per_node)
          contributor.locked_contributions.push_back(contribution);
        else
        {
          LOG_PRINT_L1("Contribution TX: Already hit the max number of contributions: " << max_contributions_per_node <<
                       " for contributor: " << cryptonote::get_account_address_as_str(m_blockchain.nettype(), false, contributor.address) <<
                       " on height: "  << block_height <<
                       " for tx: " << cryptonote::get_transaction_hash(tx));
          break;
        }
      }
    }

    LOG_PRINT_L1("Contribution of " << parsed_contribution.transferred << " received for fullnode " << pubkey);
  }

  void full_node_list::block_added(const cryptonote::block& block, const std::vector<cryptonote::transaction>& txs)
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    block_added_generic(block, txs);
    store();
  }


  template<typename T>
  void full_node_list::block_added_generic(const cryptonote::block& block, const T& txs)
  {
    uint64_t block_height = cryptonote::get_block_height(block);
    int hard_fork_version = m_blockchain.get_hard_fork_version(block_height);

    if (hard_fork_version < 9)
      return;

    //
    // Remove old rollback events
    //
    {
      assert(m_transient_state.height == block_height);
      ++m_transient_state.height;
      const size_t ROLLBACK_EVENT_EXPIRATION_BLOCKS = 30;
      uint64_t cull_height = (block_height < ROLLBACK_EVENT_EXPIRATION_BLOCKS) ? block_height : block_height - ROLLBACK_EVENT_EXPIRATION_BLOCKS;

      while (!m_transient_state.rollback_events.empty() && m_transient_state.rollback_events.front()->m_block_height < cull_height)
      {
        m_transient_state.rollback_events.pop_front();
      }
      m_transient_state.rollback_events.push_front(std::unique_ptr<rollback_event>(new prevent_rollback(cull_height)));
    }

    //
    // Remove expired blacklisted key images
    //
    for (auto entry = m_transient_state.key_image_blacklist.begin(); entry != m_transient_state.key_image_blacklist.end();)
    {
      if (block_height >= entry->unlock_height)
      {
        const bool adding_to_blacklist = false;
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_key_image_blacklist(block_height, (*entry), adding_to_blacklist)));
        entry = m_transient_state.key_image_blacklist.erase(entry);
      }
      else
        entry++;
    }

    //
    // Expire Nodes
    //
    size_t expired_count = 0;
    for (const crypto::public_key& pubkey : update_and_get_expired_nodes(txs, block_height))
    {
      auto i = m_transient_state.full_nodes_infos.find(pubkey);
      if (i != m_transient_state.full_nodes_infos.end())
      {
        if (m_full_node_pubkey && *m_full_node_pubkey == pubkey)
        {
          MGINFO_GREEN("Fullnode expired (yours): " << pubkey << " at block height: " << block_height);
        }
        else
        {
          LOG_PRINT_L1("Fullnode expired: " << pubkey << " at block height: " << block_height);
        }

        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_change(block_height, pubkey, i->second)));

        expired_count++;
        m_transient_state.full_nodes_infos.erase(i);
      }
    }

    //
    // Advance the list to the next candidate for a reward
    //
    {
      crypto::public_key winner_pubkey = cryptonote::get_full_node_winner_from_tx_extra(block.miner_tx.extra);
      if (m_transient_state.full_nodes_infos.count(winner_pubkey) == 1)
      {
        m_transient_state.rollback_events.push_back(
          std::unique_ptr<rollback_event>(
            new rollback_change(block_height, winner_pubkey, m_transient_state.full_nodes_infos[winner_pubkey])
          )
        );
        // set the winner as though it was re-registering at transaction index=UINT32_MAX for this block
        m_transient_state.full_nodes_infos[winner_pubkey].last_reward_block_height = block_height;
        m_transient_state.full_nodes_infos[winner_pubkey].last_reward_transaction_index = UINT32_MAX;
      }
    }

    //
    // Process TXs in the Block
    //
    size_t registrations = 0;
    size_t deregistrations = 0;
    for (uint32_t index = 0; index < txs.size(); ++index)
    {
      const cryptonote::transaction& tx             = txs[index];
      const cryptonote::transaction::type_t tx_type = tx.get_type();
      if (tx_type == cryptonote::transaction::type_standard)
      {
        if (process_registration_tx(tx, block.timestamp, block_height, index))
          registrations++;

        process_contribution_tx(tx, block_height, index);
      }
      else if (tx_type == cryptonote::transaction::type_deregister)
      {
        if (process_deregistration_tx(tx, block_height))
          deregistrations++;
      }
      else if (tx_type == cryptonote::transaction::type_key_image_unlock)
      {
        crypto::public_key snode_key;
        if (!cryptonote::get_full_node_pubkey_from_tx_extra(tx.extra, snode_key))
          continue;

        auto it = m_transient_state.full_nodes_infos.find(snode_key);
        if (it == m_transient_state.full_nodes_infos.end())
          continue;

        full_node_info &node_info = (*it).second;
        if (node_info.requested_unlock_height != KEY_IMAGE_AWAITING_UNLOCK_HEIGHT)
        {
          LOG_PRINT_L1("Unlock TX: Node already requested an unlock at height: " << node_info.requested_unlock_height << " rejected on height: " << block_height << " for tx: " << get_transaction_hash(tx));
          continue;
        }

        cryptonote::tx_extra_tx_key_image_unlock unlock;
        if (!cryptonote::get_tx_key_image_unlock_from_tx_extra(tx.extra, unlock))
        {
          LOG_PRINT_L1("Unlock TX: Didn't have key image unlock in the tx_extra, rejected on height: " << block_height << " for tx: " << get_transaction_hash(tx));
          continue;
        }

        uint64_t unlock_height = get_locked_key_image_unlock_height(m_blockchain.nettype(), node_info.registration_height, block_height);
        bool early_exit        = false;

        for (auto contributor = node_info.contributors.begin();
             contributor     != node_info.contributors.end() && !early_exit;
             contributor++)
        {
          for (auto locked_contribution = contributor->locked_contributions.begin();
               locked_contribution     != contributor->locked_contributions.end() && !early_exit;
               locked_contribution++)
          {
            if (unlock.key_image != locked_contribution->key_image)
              continue;

            // NOTE(antd): This should be checked in blockchain check_tx_inputs already
            crypto::hash const hash = full_nodes::generate_request_stake_unlock_hash(unlock.nonce);
            if (!crypto::check_signature(hash, locked_contribution->key_image_pub_key, unlock.signature))
            {
              LOG_PRINT_L1("Unlock TX: Couldn't verify key image unlock in the tx_extra, rejected on height: " << block_height << " for tx: " << get_transaction_hash(tx));
              early_exit = true;
              break;
            }

            m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_key_image_unlock(block_height, snode_key)));
            node_info.requested_unlock_height = unlock_height;
            early_exit = true;
          }
        }
      }
    }

    if (registrations || deregistrations || expired_count) {
      update_swarms(block_height);
    }

    //
    // Update Quorum
    //
    const size_t cache_state_from_height = (block_height < QUORUM_LIFETIME) ? 0 : block_height - QUORUM_LIFETIME;
    store_quorum_state_from_rewards_list(block_height);
    while (!m_transient_state.quorum_states.empty() && m_transient_state.quorum_states.begin()->first < cache_state_from_height)
    {
      m_transient_state.quorum_states.erase(m_transient_state.quorum_states.begin());
    }
  }

  void full_node_list::blockchain_detached(uint64_t height)
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    while (!m_transient_state.rollback_events.empty() && m_transient_state.rollback_events.back()->m_block_height >= height)
    {
      rollback_event *event = &(*m_transient_state.rollback_events.back());
      bool rollback_applied = true;
      switch(event->type)
      {
        case rollback_event::change_type:
        {
          auto *rollback = reinterpret_cast<rollback_change *>(event);
          m_transient_state.full_nodes_infos[rollback->m_key] = rollback->m_info;
        }
        break;

        case rollback_event::new_type:
        {
          auto *rollback = reinterpret_cast<rollback_new *>(event);

          auto iter = m_transient_state.full_nodes_infos.find(rollback->m_key);
          if (iter == m_transient_state.full_nodes_infos.end())
          {
            MERROR("Could not find fullnode pubkey in rollback new");
            rollback_applied = false;
            break;
          }

          m_transient_state.full_nodes_infos.erase(iter);
        }
        break;

        case rollback_event::prevent_type: { rollback_applied = false; } break;

        case rollback_event::key_image_blacklist_type:
        {
          auto *rollback = reinterpret_cast<rollback_key_image_blacklist *>(event);
          if (rollback->m_was_adding_to_blacklist)
          {
            auto it = std::find_if(m_transient_state.key_image_blacklist.begin(), m_transient_state.key_image_blacklist.end(),
            [rollback] (key_image_blacklist_entry const &a) {
                return (rollback->m_entry.unlock_height == a.unlock_height && rollback->m_entry.key_image == a.key_image);
            });

            if (it == m_transient_state.key_image_blacklist.end())
            {
              LOG_PRINT_L1("Could not find blacklisted key image to remove");
              rollback_applied = false;
              break;
            }

            m_transient_state.key_image_blacklist.erase(it);
          }
          else
          {
            m_transient_state.key_image_blacklist.push_back(rollback->m_entry);
          }
        }
        break;

        case rollback_event::key_image_unlock:
        {
          auto *rollback = reinterpret_cast<rollback_key_image_unlock *>(event);
          auto iter = m_transient_state.full_nodes_infos.find(rollback->m_key);
          if (iter == m_transient_state.full_nodes_infos.end())
          {
            MERROR("Could not find fullnode pubkey in rollback key image unlock");
            rollback_applied = false;
            break;
          }
          iter->second.requested_unlock_height = KEY_IMAGE_AWAITING_UNLOCK_HEIGHT;
        }
        break;

        default:
        {
          MERROR("Unhandled rollback type");
          rollback_applied = false;
        }
        break;
      }

      if (!rollback_applied)
      {
        init();
        break;
      }

      m_transient_state.rollback_events.pop_back();
    }

    while (!m_transient_state.quorum_states.empty() && (--m_transient_state.quorum_states.end())->first >= height)
      m_transient_state.quorum_states.erase(--m_transient_state.quorum_states.end());

    m_transient_state.height = height;
    store();
  }

  std::vector<crypto::public_key> full_node_list::update_and_get_expired_nodes(const std::vector<cryptonote::transaction> &txs, uint64_t block_height)
  {
    std::vector<crypto::public_key> expired_nodes;
    uint64_t const lock_blocks = staking_num_lock_blocks(m_blockchain.nettype());

    // TODO(antd): This should really use the registration height instead of getting the block and expiring nodes.
    // But there's something subtly off when using registration height causing syncing problems.
    if (m_blockchain.get_hard_fork_version(block_height) == cryptonote::network_version_9_full_nodes)
    {
      if (block_height < lock_blocks)
        return expired_nodes;

      const uint64_t expired_nodes_block_height = block_height - lock_blocks;
      std::vector<std::pair<cryptonote::blobdata, cryptonote::block>> blocks;
      if (!m_blockchain.get_blocks(expired_nodes_block_height, 1, blocks))
      {
        LOG_ERROR("Unable to get historical blocks");
        return expired_nodes;
      }

      const cryptonote::block& block = blocks.begin()->second;
      std::vector<cryptonote::transaction> txs;
      std::vector<crypto::hash> missed_txs;
      if (!m_blockchain.get_transactions(block.tx_hashes, txs, missed_txs))
      {
        LOG_ERROR("Unable to get transactions for block " << block.hash);
        return expired_nodes;
      }

      uint32_t index = 0;
      for (const cryptonote::transaction& tx : txs)
      {
        crypto::public_key key;
        full_node_info info = {};
        if (is_registration_tx(tx, block.timestamp, expired_nodes_block_height, index, key, info))
        {
          expired_nodes.push_back(key);
        }
        index++;
      }
    }
    else
    {
      for (auto it = m_transient_state.full_nodes_infos.begin(); it != m_transient_state.full_nodes_infos.end(); it++)
      {
        crypto::public_key const &snode_key = it->first;
        full_node_info &info             = it->second;
        int const hf_version                = m_blockchain.get_hard_fork_version(info.registration_height);

        if (hf_version >= cryptonote::network_version_11_infinite_staking)
        {
          if (info.requested_unlock_height != KEY_IMAGE_AWAITING_UNLOCK_HEIGHT && block_height > info.requested_unlock_height)
            expired_nodes.push_back(snode_key);
        }
        else // Version 10 Bulletproofs
        {
          /// Note: this code exhibits a sublte unintended behaviour: a snode that
          /// registered in hardfork 9 and was scheduled for deregistration in hardfork 10
          /// will have its life is slightly prolonged by the "grace period", although it might
          /// look like we use the registration height to determine the expiry height.
          uint64_t node_expiry_height = info.registration_height + lock_blocks + STAKING_REQUIREMENT_LOCK_BLOCKS_EXCESS;
          if (block_height > node_expiry_height)
            expired_nodes.push_back(snode_key);
        }
      }
    }

    return expired_nodes;
  }

  std::vector<std::pair<cryptonote::account_public_address, uint64_t>> full_node_list::get_winner_addresses_and_portions() const
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    crypto::public_key key = select_winner();
    if (key == crypto::null_pkey)
      return { std::make_pair(null_address, STAKING_PORTIONS) };

    std::vector<std::pair<cryptonote::account_public_address, uint64_t>> winners;

    const full_node_info& info = m_transient_state.full_nodes_infos.at(key);

    const uint64_t remaining_portions = STAKING_PORTIONS - info.portions_for_operator;

    // Add contributors and their portions to winners.
    for (const auto& contributor : info.contributors)
    {
      uint64_t hi, lo, resulthi, resultlo;
      lo = mul128(contributor.amount, remaining_portions, &hi);
      div128_64(hi, lo, info.staking_requirement, &resulthi, &resultlo);

      if (contributor.address == info.operator_address)
        resultlo += info.portions_for_operator;

      winners.push_back(std::make_pair(contributor.address, resultlo));
    }
    return winners;
  }

  crypto::public_key full_node_list::select_winner() const
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    auto oldest_waiting = std::pair<uint64_t, uint32_t>(std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint32_t>::max());
    crypto::public_key key = crypto::null_pkey;
    for (const auto& info : m_transient_state.full_nodes_infos)
      if (info.second.is_fully_funded())
      {
        auto waiting_since = std::make_pair(info.second.last_reward_block_height, info.second.last_reward_transaction_index);
        if (waiting_since < oldest_waiting)
        {
          oldest_waiting = waiting_since;
          key = info.first;
        }
      }
    return key;
  }

  bool full_node_list::validate_miner_tx(const crypto::hash& prev_id, const cryptonote::transaction& miner_tx, uint64_t height, int hard_fork_version, cryptonote::block_reward_parts const &reward_parts) const
  {
    std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);
    if (hard_fork_version < 9)
      return true;

    // NOTE(antd): Fullnode reward distribution is calculated from the
    // original amount, i.e. 50% of the original base reward goes to service
    // nodes not 50% of the reward after removing the control component (the
    // adjusted base reward post hardfork 10).
    uint64_t base_reward = reward_parts.original_base_reward;
    uint64_t total_full_node_reward = cryptonote::full_node_reward_formula(base_reward, hard_fork_version);

    crypto::public_key winner = select_winner();

    crypto::public_key check_winner_pubkey = cryptonote::get_full_node_winner_from_tx_extra(miner_tx.extra);
    if (check_winner_pubkey != winner)
      return false;

    const std::vector<std::pair<cryptonote::account_public_address, uint64_t>> addresses_and_portions = get_winner_addresses_and_portions();
    
    if (miner_tx.vout.size() - 1 < addresses_and_portions.size())
      return false;

    for (size_t i = 0; i < addresses_and_portions.size(); i++)
    {
      size_t vout_index = i + 1;
      uint64_t reward = cryptonote::get_portion_of_reward(addresses_and_portions[i].second, total_full_node_reward);

      if (miner_tx.vout[vout_index].amount != reward)
      {
        MERROR("Fullnode reward amount incorrect. Should be " << cryptonote::print_money(reward) << ", is: " << cryptonote::print_money(miner_tx.vout[vout_index].amount));
        return false;
      }

      if (miner_tx.vout[vout_index].target.type() != typeid(cryptonote::txout_to_key))
      {
        MERROR("Fullnode output target type should be txout_to_key");
        return false;
      }

      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);;
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
      cryptonote::keypair ctrl_key = cryptonote::get_deterministic_keypair_from_height(height);

      bool r = crypto::generate_key_derivation(addresses_and_portions[i].first.m_view_public_key, ctrl_key.sec, derivation);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << addresses_and_portions[i].first.m_view_public_key << ", " << ctrl_key.sec << ")");
      r = crypto::derive_public_key(derivation, vout_index, addresses_and_portions[i].first.m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << vout_index << ", "<< addresses_and_portions[i].first.m_spend_public_key << ")");

      if (boost::get<cryptonote::txout_to_key>(miner_tx.vout[vout_index].target).key != out_eph_public_key)
      {
        MERROR("Invalid fullnode reward output");
        return false;
      }
    }

    return true;
  }

  template<typename T>
  void antd_shuffle(std::vector<T>& a, uint64_t seed)
  {
    if (a.size() <= 1) return;
    std::mt19937_64 mersenne_twister(seed);
    for (size_t i = 1; i < a.size(); i++)
    {
      size_t j = (size_t)uniform_distribution_portable(mersenne_twister, i+1);
      if (i != j)
        std::swap(a[i], a[j]);
    }
  }

  void full_node_list::store_quorum_state_from_rewards_list(uint64_t height)
  {
    const crypto::hash block_hash = m_blockchain.get_block_id_by_height(height);
    if (block_hash == crypto::null_hash)
    {
      MERROR("Block height: " << height << " returned null hash");
      return;
    }

    std::vector<crypto::public_key> full_node_list = get_full_nodes_pubkeys();
    std::vector<size_t>                              pub_keys_indexes(full_node_list.size());
    {
      size_t index = 0;
      for (size_t i = 0; i < full_node_list.size(); i++) { pub_keys_indexes[i] = i; }

      // Shuffle indexes
      uint64_t seed = 0;
      std::memcpy(&seed, block_hash.data, std::min(sizeof(seed), sizeof(block_hash.data)));

      antd_shuffle(pub_keys_indexes, seed);
    }

    // Assign indexes from shuffled list into quorum and list of nodes to test

    auto new_state = std::make_shared<quorum_state>();

    {
      std::vector<crypto::public_key>& quorum = new_state->quorum_nodes;
      {
        quorum.resize(std::min(full_node_list.size(), QUORUM_SIZE));
        for (size_t i = 0; i < quorum.size(); i++)
        {
          size_t node_index                   = pub_keys_indexes[i];
          const crypto::public_key &key = full_node_list[node_index];
          quorum[i] = key;
        }
      }

      std::vector<crypto::public_key>& nodes_to_test = new_state->nodes_to_test;
      {
        size_t num_remaining_nodes = pub_keys_indexes.size() - quorum.size();
        size_t num_nodes_to_test   = std::max(num_remaining_nodes/NTH_OF_THE_NETWORK_TO_TEST,
                                              std::min(MIN_NODES_TO_TEST, num_remaining_nodes));

        nodes_to_test.resize(num_nodes_to_test);

        const int pub_keys_offset = quorum.size();
        for (size_t i = 0; i < nodes_to_test.size(); i++)
        {
          size_t node_index             = pub_keys_indexes[pub_keys_offset + i];
          const crypto::public_key &key = full_node_list[node_index];
          nodes_to_test[i]              = key;
        }
      }
    }

    m_transient_state.quorum_states[height] = new_state;
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  full_node_list::rollback_event::rollback_event(uint64_t block_height, rollback_type type) : m_block_height(block_height), type(type)
  {
  }

  full_node_list::rollback_change::rollback_change(uint64_t block_height, const crypto::public_key& key, const full_node_info& info)
    : full_node_list::rollback_event(block_height, change_type), m_key(key), m_info(info)
  {
  }

  full_node_list::rollback_new::rollback_new(uint64_t block_height, const crypto::public_key& key)
    : full_node_list::rollback_event(block_height, new_type), m_key(key)
  {
  }

  full_node_list::prevent_rollback::prevent_rollback(uint64_t block_height) : full_node_list::rollback_event(block_height, prevent_type) { }

  full_node_list::rollback_key_image_blacklist::rollback_key_image_blacklist(uint64_t block_height, key_image_blacklist_entry const &entry, bool is_adding_to_blacklist)
    : full_node_list::rollback_event(block_height, key_image_blacklist_type), m_entry(entry), m_was_adding_to_blacklist(is_adding_to_blacklist)
  {
  }

  full_node_list::rollback_key_image_unlock::rollback_key_image_unlock(uint64_t block_height, crypto::public_key const &key)
    : full_node_list::rollback_event(block_height, key_image_unlock), m_key(key)
  {
  }

  bool full_node_list::store()
  {
    if (m_blockchain.get_current_hard_fork_version() < cryptonote::network_version_9_full_nodes)
      return true;

    CHECK_AND_ASSERT_MES(m_db != nullptr, false, "Failed to store fullnode info, m_db == nullptr");
    data_members_for_serialization data_to_store;
    {
      std::lock_guard<boost::recursive_mutex> lock(m_sn_mutex);

      quorum_state_for_serialization quorum;
      for(const auto& kv_pair : m_transient_state.quorum_states)
      {
        quorum.height = kv_pair.first;
        quorum.state = *kv_pair.second;
        data_to_store.quorum_states.push_back(quorum);
      }

      full_node_pubkey_info info;
      for (const auto& kv_pair : m_transient_state.full_nodes_infos)
      {
        info.pubkey = kv_pair.first;
        info.info   = kv_pair.second;
        data_to_store.infos.push_back(info);
      }

      for (const auto& event_ptr : m_transient_state.rollback_events)
      {
        switch (event_ptr->type)
        {
          case rollback_event::change_type:              data_to_store.events.push_back(*reinterpret_cast<rollback_change *>(event_ptr.get())); break;
          case rollback_event::new_type:                 data_to_store.events.push_back(*reinterpret_cast<rollback_new *>(event_ptr.get())); break;
          case rollback_event::prevent_type:             data_to_store.events.push_back(*reinterpret_cast<prevent_rollback *>(event_ptr.get())); break;
          case rollback_event::key_image_blacklist_type: data_to_store.events.push_back(*reinterpret_cast<rollback_key_image_blacklist *>(event_ptr.get())); break;
          case rollback_event::key_image_unlock:         data_to_store.events.push_back(*reinterpret_cast<rollback_key_image_unlock *>(event_ptr.get())); break;
          default:
            MERROR("On storing fullnode data, unknown rollback event type encountered");
            return false;
        }
      }

      data_to_store.key_image_blacklist = m_transient_state.key_image_blacklist;
    }

    data_to_store.height  = m_transient_state.height;
    int hf_version        = m_blockchain.get_hard_fork_version(m_transient_state.height - 1);
    data_to_store.version = get_min_full_node_info_version_for_hf(hf_version);

    std::stringstream ss;
    binary_archive<true> ba(ss);

    bool r = ::serialization::serialize(ba, data_to_store);
    CHECK_AND_ASSERT_MES(r, false, "Failed to store fullnode info: failed to serialize data");

    std::string blob = ss.str();
    m_db->block_txn_start(false/*readonly*/);
    m_db->set_full_node_data(blob);
    m_db->block_txn_stop();

    return true;
  }

  void full_node_list::get_all_full_nodes_public_keys(std::vector<crypto::public_key>& keys, bool fully_funded_nodes_only) const
  {
    keys.clear();
    keys.resize(m_transient_state.full_nodes_infos.size());

    size_t i = 0;
    if (fully_funded_nodes_only)
    {
      for (const auto &it : m_transient_state.full_nodes_infos)
      {
        full_node_info const &info = it.second;
        if (info.is_fully_funded())
          keys[i++] = it.first;
      }
    }
    else
    {
      for (const auto &it : m_transient_state.full_nodes_infos)
        keys[i++] = it.first;
    }
  }


  bool full_node_list::load()
  {
    LOG_PRINT_L1("full_node_list::load()");
    clear(false);
    if (!m_db)
    {
      return false;
    }
    std::stringstream ss;

    data_members_for_serialization data_in;
    std::string blob;

    m_db->block_txn_start(true/*readonly*/);
    if (!m_db->get_full_node_data(blob))
    {
      m_db->block_txn_stop();
      return false;
    }
    m_db->block_txn_stop();

    ss << blob;
    binary_archive<false> ba(ss);
    bool r = ::serialization::serialize(ba, data_in);
    CHECK_AND_ASSERT_MES(r, false, "Failed to parse fullnode data from blob");

    m_transient_state.height = data_in.height;
    m_transient_state.key_image_blacklist = data_in.key_image_blacklist;

    for (const auto& quorum : data_in.quorum_states)
    {
      m_transient_state.quorum_states[quorum.height] = std::make_shared<quorum_state>(quorum.state);
    }

    for (const auto& info : data_in.infos)
    {
      m_transient_state.full_nodes_infos[info.pubkey] = info.info;
    }

    for (const auto& event : data_in.events)
    {
      if (event.type() == typeid(rollback_change))
      {
        const auto& from = boost::get<rollback_change>(event);
        auto *i = new rollback_change();
        *i = from;
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(i));
      }
      else if (event.type() == typeid(rollback_new))
      {
        const auto& from = boost::get<rollback_new>(event);
        auto *i = new rollback_new();
        *i = from;
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(i));
      }
      else if (event.type() == typeid(prevent_rollback))
      {
        const auto& from = boost::get<prevent_rollback>(event);
        auto *i = new prevent_rollback();
        *i = from;
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(i));
      }
      else if (event.type() == typeid(rollback_key_image_blacklist))
      {
        const auto& from = boost::get<rollback_key_image_blacklist>(event);
        auto *i = new rollback_key_image_blacklist();
        *i = from;
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(i));
      }
      else if (event.type() == typeid(rollback_key_image_unlock))
      {
        const auto& from = boost::get<rollback_key_image_unlock>(event);
        auto *i = new rollback_key_image_unlock();
        *i = from;
        m_transient_state.rollback_events.push_back(std::unique_ptr<rollback_event>(i));
      }
      else
      {
        MERROR("Unhandled rollback event type in restoring data to fullnode list.");
        return false;
      }
    }

    MGINFO("Fullnode data loaded successfully, height: " << m_transient_state.height);
    MGINFO(m_transient_state.full_nodes_infos.size() << " nodes and " << m_transient_state.rollback_events.size() << " rollback events loaded.");

    LOG_PRINT_L1("full_node_list::load() returning success");
    return true;
  }

  void full_node_list::clear(bool delete_db_entry)
  {
    m_transient_state = {};
    if (m_db && delete_db_entry)
    {
      m_db->block_txn_start(false/*readonly*/);
      m_db->clear_full_node_data();
      m_db->block_txn_stop();
    }

    uint64_t hardfork_9_from_height = 0;
    {
      uint32_t window, votes, threshold;
      uint8_t voting;
      m_blockchain.get_hard_fork_voting_info(9, window, votes, threshold, hardfork_9_from_height, voting);
    }
    m_transient_state.height = hardfork_9_from_height;
  }

  size_t full_node_info::total_num_locked_contributions() const
  {
    size_t result = 0;
    for (full_node_info::contributor_t const &contributor : this->contributors)
      result += contributor.locked_contributions.size();
    return result;
  }

  converted_registration_args convert_registration_args(cryptonote::network_type nettype,
                                                        const std::vector<std::string>& args,
                                                        uint64_t staking_requirement,
                                                        int hf_version)
  {
    converted_registration_args result = {};
    if (args.size() % 2 == 0 || args.size() < 3)
    {
      result.err_msg = tr("Usage: <operator cut> <address> <fraction> [<address> <fraction> [...]]]");
      return result;
    }

    if ((args.size()-1)/ 2 > MAX_NUMBER_OF_CONTRIBUTORS)
    {
      result.err_msg = tr("Exceeds the maximum number of contributors, which is ") + std::to_string(MAX_NUMBER_OF_CONTRIBUTORS);
      return result;
    }

    try
    {
      result.portions_for_operator = boost::lexical_cast<uint64_t>(args[0]);
      if (result.portions_for_operator > STAKING_PORTIONS)
      {
        result.err_msg = tr("Invalid portion amount: ") + args[0] + tr(". Must be between 0 and ") + std::to_string(STAKING_PORTIONS);
        return result;
      }
    }
    catch (const std::exception &e)
    {
      result.err_msg = tr("Invalid portion amount: ") + args[0] + tr(". Must be between 0 and ") + std::to_string(STAKING_PORTIONS);
      return result;
    }

    struct addr_to_portion_t
    {
      cryptonote::address_parse_info info;
      uint64_t portions;
    };

    std::vector<addr_to_portion_t> addr_to_portions;
    size_t const OPERATOR_ARG_INDEX     = 1;
    for (size_t i = OPERATOR_ARG_INDEX, num_contributions = 0;
         i < args.size();
         i += 2, ++num_contributions)
    {
      cryptonote::address_parse_info info;
      if (!cryptonote::get_account_address_from_str(info, nettype, args[i]))
      {
        result.err_msg = tr("Failed to parse address: ") + args[i];
        return result;
      }

      if (info.has_payment_id)
      {
        result.err_msg = tr("Can't use a payment id for staking tx");
        return result;
      }

      if (info.is_subaddress)
      {
        result.err_msg = tr("Can't use a subaddress for staking tx");
        return result;
      }

      try
      {
        uint64_t num_portions = boost::lexical_cast<uint64_t>(args[i+1]);
        addr_to_portions.push_back({info, num_portions});
      }
      catch (const std::exception &e)
      {
        result.err_msg = tr("Invalid amount for contributor: ") + args[i] + tr(", with portion amount that could not be converted to a number: ") + args[i+1];
        return result;
      }
    }

    //
    // FIXME(doyle): FIXME(antd) !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // This is temporary code to redistribute the insufficient portion dust
    // amounts between contributors. It should be removed in HF12.
    //
    std::array<uint64_t, MAX_NUMBER_OF_CONTRIBUTORS * full_nodes::MAX_KEY_IMAGES_PER_CONTRIBUTOR> excess_portions;
    std::array<uint64_t, MAX_NUMBER_OF_CONTRIBUTORS * full_nodes::MAX_KEY_IMAGES_PER_CONTRIBUTOR> min_contributions;
    {
      // NOTE: Calculate excess portions from each contributor
      uint64_t antd_reserved = 0;
      for (size_t index = 0; index < addr_to_portions.size(); ++index)
      {
        addr_to_portion_t const &addr_to_portion = addr_to_portions[index];
        uint64_t min_contribution_portions       = full_nodes::get_min_node_contribution_in_portions(hf_version, staking_requirement, antd_reserved, index);
        uint64_t antd_amount                     = full_nodes::portions_to_amount(staking_requirement, addr_to_portion.portions);
        antd_reserved                           += antd_amount;

        uint64_t excess = 0;
        if (addr_to_portion.portions > min_contribution_portions)
          excess = addr_to_portion.portions - min_contribution_portions;

        min_contributions[index] = min_contribution_portions;
        excess_portions[index]   = excess;
      }
    }

    uint64_t portions_left  = STAKING_PORTIONS;
    uint64_t total_reserved = 0;
    for (size_t i = 0; i < addr_to_portions.size(); ++i)
    {
      addr_to_portion_t &addr_to_portion = addr_to_portions[i];
      uint64_t min_portions = get_min_node_contribution_in_portions(hf_version, staking_requirement, total_reserved, i);

      uint64_t portions_to_steal = 0;
      if (addr_to_portion.portions < min_portions)
      {
          // NOTE: Steal dust portions from other contributor if we fall below
          // the minimum by a dust amount.
          uint64_t needed             = min_portions - addr_to_portion.portions;
          const uint64_t FUDGE_FACTOR = 10;
          const uint64_t DUST_UNIT    = (STAKING_PORTIONS / staking_requirement);
          const uint64_t DUST         = DUST_UNIT * FUDGE_FACTOR;
          if (needed > DUST)
            continue;

          for (size_t sub_index = 0; sub_index < addr_to_portions.size(); sub_index++)
          {
            if (i == sub_index) continue;
            uint64_t &contributor_excess = excess_portions[sub_index];
            if (contributor_excess > 0)
            {
              portions_to_steal = std::min(needed, contributor_excess);
              addr_to_portion.portions += portions_to_steal;
              contributor_excess -= portions_to_steal;
              needed -= portions_to_steal;
              result.portions[sub_index] -= portions_to_steal;

              if (needed == 0)
                break;
            }
          }

          // NOTE: Operator is sending in the minimum amount and it falls below
          // the minimum by dust, just increase the portions so it passes
          if (needed > 0 && addr_to_portions.size() < MAX_NUMBER_OF_CONTRIBUTORS * full_nodes::MAX_KEY_IMAGES_PER_CONTRIBUTOR)
            addr_to_portion.portions += needed;
      }

      if (addr_to_portion.portions < min_portions || (addr_to_portion.portions - portions_to_steal) > portions_left)
      {
        result.err_msg = tr("Invalid amount for contributor: ") + args[i] + tr(", with portion amount: ") + args[i+1] + tr(". The contributors must each have at least 25%, except for the last contributor which may have the remaining amount");
        return result;
      }

      if (min_portions == UINT64_MAX)
      {
        result.err_msg = tr("Too many contributors specified, you can only split a node with up to: ") + std::to_string(MAX_NUMBER_OF_CONTRIBUTORS) + tr(" people.");
        return result;
      }

      portions_left -= addr_to_portion.portions;
      portions_left += portions_to_steal;
      result.addresses.push_back(addr_to_portion.info.address);
      result.portions.push_back(addr_to_portion.portions);
      uint64_t antd_amount = full_nodes::portions_to_amount(addr_to_portion.portions, staking_requirement);
      total_reserved      += antd_amount;
    }

    result.success = true;
    return result;
  }

  bool make_registration_cmd(cryptonote::network_type nettype,
      int hf_version,
      uint64_t staking_requirement,
      const std::vector<std::string>& args,
      const crypto::public_key& full_node_pubkey,
      const crypto::secret_key &full_node_key,
      std::string &cmd,
      bool make_friendly,
      boost::optional<std::string&> err_msg)
  {

    converted_registration_args converted_args = convert_registration_args(nettype, args, staking_requirement, hf_version);
    if (!converted_args.success)
    {
      MERROR(tr("Could not convert registration args, reason: ") << converted_args.err_msg);
      return false;
    }

    uint64_t exp_timestamp = time(nullptr) + STAKING_AUTHORIZATION_EXPIRATION_WINDOW;

    crypto::hash hash;
    bool hashed = cryptonote::get_registration_hash(converted_args.addresses, converted_args.portions_for_operator, converted_args.portions, exp_timestamp, hash);
    if (!hashed)
    {
      MERROR(tr("Could not make registration hash from addresses and portions"));
      return false;
    }

    crypto::signature signature;
    crypto::generate_signature(hash, full_node_pubkey, full_node_key, signature);

    std::stringstream stream;
    if (make_friendly)
    {
      stream << tr("Run this command in the wallet that will fund this registration:\n\n");
    }

    stream << "register_full_node";
    for (size_t i = 0; i < args.size(); ++i)
    {
      stream << " " << args[i];
    }

    stream << " " << exp_timestamp << " ";
    stream << epee::string_tools::pod_to_hex(full_node_pubkey) << " ";
    stream << epee::string_tools::pod_to_hex(signature);

    if (make_friendly)
    {
      stream << "\n\n";
      time_t tt = exp_timestamp;

      struct tm tm;
      epee::misc_utils::get_gmt_time(tt, tm);

      char buffer[128];
      strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M:%S %p", &tm);
      stream << tr("This registration expires at ") << buffer << tr(".\n");
      stream << tr("This should be in about 2 weeks, or two years for autostaking.\n");
      stream << tr("If it isn't, check this computer's clock.\n");
      stream << tr("Please submit your registration into the blockchain before this time or it will be invalid.");
    }

    cmd = stream.str();
    return true;
  }
}

