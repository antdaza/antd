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

#include <vector>
#include <iostream>
#include <sstream>

#include "include_base_utils.h"

#include "console_handler.h"
#include "common/rules.h"

#include "p2p/net_node.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_core/constants.h"
#include "cryptonote_basic/miner.h"

#include "chaingen.h"
#include "device/device.hpp"
using namespace std;

using namespace epee;
using namespace crypto;
using namespace cryptonote;

bool operator<(const last_reward_point& lhs, const last_reward_point& rhs) {
  if (lhs.height != rhs.height) {
    return lhs.height < rhs.height;
  }

  return lhs.priority < rhs.priority;
}

dereg_tx_builder linear_chain_generator::build_deregister(const crypto::public_key& pk, bool commit)
{
  return dereg_tx_builder(*this, pk, commit);
}

cryptonote::account_base linear_chain_generator::create_account()
{
  cryptonote::account_base account;
  account.generate();
  events_.push_back(account);
  return account;
}

void linear_chain_generator::create_genesis_block()
{
  constexpr uint64_t ts_start = 1338224400;
  first_miner_.generate();
  cryptonote::block gen_block;
  gen_.construct_block(gen_block, first_miner_, ts_start);
  events_.push_back(gen_block);
  blocks_.push_back(gen_block);
}

void linear_chain_generator::create_block(const std::vector<cryptonote::transaction>& txs)
{
  const auto blk = create_block_on_fork(blocks_.back(), txs);
  blocks_.push_back(blk);
}

uint8_t linear_chain_generator::get_hf_version_at(uint64_t height) const {

  uint8_t cur_hf_ver = 0;

  for (auto i = 0u; i < hard_forks_.size(); ++i)
  {
    if (height < hard_forks_[i].second) break;
    cur_hf_ver = hard_forks_[i].first;
  }

  assert(cur_hf_ver != 0);
  return cur_hf_ver;
}

void linear_chain_generator::rewind_until_version(int hard_fork_version)
{
  assert(gen_.m_hf_version < hard_fork_version);

  if (blocks_.size() == 0)
    create_genesis_block();

  size_t start_index;
  for (start_index = 0; start_index < hard_forks_.size(); ++start_index)
  {
    const uint8_t version = hard_forks_[start_index].first;
    if (version > gen_.m_hf_version) break;
  }

  for (size_t i = start_index; i < hard_forks_.size() && gen_.m_hf_version < hard_fork_version; ++i)
  {
    auto cur_height                    = blocks_.size();
    uint64_t next_fork_height          = hard_forks_[i].second;
    uint64_t blocks_till_next_hardfork = next_fork_height - cur_height;

    rewind_blocks_n(blocks_till_next_hardfork);
    gen_.m_hf_version = hard_forks_[i].first;
    create_block();

  }

  assert(gen_.m_hf_version >= hard_fork_version);
}

int linear_chain_generator::get_hf_version() const {
  return gen_.m_hf_version;
}


void linear_chain_generator::rewind_blocks_n(int n)
{
  for (auto i = 0; i < n; ++i) {
    create_block();
  }
}

void linear_chain_generator::rewind_blocks()
{
  rewind_blocks_n(CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW);
}

cryptonote::block linear_chain_generator::create_block_on_fork(const cryptonote::block& prev,
                                                               const std::vector<cryptonote::transaction>& txs)
{

  const auto height = get_block_height(prev) + 1;

  const auto& winner_pk = sn_list_.get_winner_pk(height);

  const auto& sn_pk = winner_pk ? *winner_pk : crypto::null_pkey;

  std::vector<sn_contributor_t> contribs = { { { crypto::null_pkey, crypto::null_pkey }, STAKING_PORTIONS } };

  if (winner_pk) {
    const auto& reg = sn_list_.find_registration(*winner_pk);
    if (reg) {
      contribs = { reg->contribution };
    }
  }

  cryptonote::block blk;
  gen_.construct_block(blk, prev, first_miner_, { txs.begin(), txs.end() }, sn_pk, contribs);
  events_.push_back(blk);

  /// now we can add sn from the buffer to be used in consequent nodes
  sn_list_.add_registrations(registration_buffer_);
  registration_buffer_.clear();

  sn_list_.handle_deregistrations(deregistration_buffer_);
  deregistration_buffer_.clear();

  /// Note: depending on whether we check in hf9 or later, antd assignes different meaning to
  /// "expiration height": in hf9 it expires nodes at their expiration height; after hf9 --
  /// a the expiration height + 1.
  if (get_hf_version() == network_version_9_full_nodes) {
    sn_list_.expire_old(height);
  } else {
    sn_list_.expire_old(height - 1);
  }

  return blk;
}

QuorumState linear_chain_generator::get_quorum_idxs(const cryptonote::block& block) const
{
  if (sn_list_.size() <= full_nodes::QUORUM_SIZE) {
    std::cerr << "Not enough fullnodes\n";
    return {};
  }

  std::vector<size_t> pub_keys_indexes;
  {
    uint64_t seed = 0;
    const crypto::hash block_hash = cryptonote::get_block_hash(block);
    std::memcpy(&seed, block_hash.data, std::min(sizeof(seed), sizeof(block_hash.data)));

    pub_keys_indexes.resize(sn_list_.size());
    for (size_t i = 0; i < pub_keys_indexes.size(); i++) {
      pub_keys_indexes[i] = i;
    }

    full_nodes::antd_shuffle(pub_keys_indexes, seed);
  }

  QuorumState quorum;

  for (auto i = 0u; i < full_nodes::QUORUM_SIZE; ++i) {
    quorum.voters.push_back({ sn_list_.at(pub_keys_indexes[i]).keys.pub, i });
  }

  for (auto i = full_nodes::QUORUM_SIZE; i < pub_keys_indexes.size(); ++i) {
    quorum.to_test.push_back({ sn_list_.at(pub_keys_indexes[i]).keys.pub, i });
  }

  return quorum;
}

QuorumState linear_chain_generator::get_quorum_idxs(uint64_t height) const
{
  const auto block = blocks_.at(height);
  return get_quorum_idxs(block);
}

cryptonote::transaction linear_chain_generator::create_tx(const cryptonote::account_base& miner,
                                                          const cryptonote::account_base& acc,
                                                          uint64_t amount,
                                                          uint64_t fee)
{
  cryptonote::transaction t;
  TxBuilder(events_, t, blocks_.back(), miner, acc, amount, gen_.m_hf_version).with_fee(fee).build();
  events_.push_back(t);
  return t;
}

cryptonote::transaction linear_chain_generator::create_registration_tx(const cryptonote::account_base& acc,
                                                                       const cryptonote::keypair& sn_keys)
{
  const sn_contributor_t contr = { acc.get_keys().m_account_address, STAKING_PORTIONS };
  uint32_t expires = height() + full_nodes::staking_num_lock_blocks(cryptonote::FAKECHAIN);

  /// Account for some inconsistency in full_nodes::staking_num_lock_blocks
  /// on the boundary between hardforks 9 and 10
  if (get_hf_version() == cryptonote::network_version_9_full_nodes &&
      get_hf_version_at(expires) == cryptonote::network_version_10_bulletproofs)
  {
    expires += STAKING_REQUIREMENT_LOCK_BLOCKS_EXCESS;
  }

  const auto reg_idx = registration_buffer_.size();
  registration_buffer_.push_back({ expires, sn_keys, contr, { height(), reg_idx } });
  return make_default_registration_tx(events_, acc, sn_keys, blocks_.back(), gen_.m_hf_version);
}

cryptonote::transaction linear_chain_generator::create_registration_tx()
{
  const auto sn_keys = keypair::generate(hw::get_device("default"));

  return create_registration_tx(first_miner_, sn_keys);
}

cryptonote::transaction linear_chain_generator::create_deregister_tx(const crypto::public_key& pk,
                                                                     uint64_t height,
                                                                     const std::vector<sn_idx>& voters,
                                                                     uint64_t fee,
                                                                     bool commit)
{

  cryptonote::tx_extra_full_node_deregister deregister;
  deregister.block_height = height;

  const auto idx = get_idx_in_tested(pk, height);

  if (!idx) { MERROR("fullnode could not be found in the servcie node list"); throw std::exception(); }

  deregister.full_node_index = *idx; /// idx inside nodes to test

  /// need to create MIN_VOTES_TO_KICK_FULL_NODE (7) votes
  for (const auto voter : voters) {

    const auto reg = sn_list_.find_registration(voter.sn_pk);

    if (!reg) return {};

    const auto pk = reg->keys.pub;
    const auto sk = reg->keys.sec;
    const auto signature =
      full_nodes::deregister_vote::sign_vote(deregister.block_height, deregister.full_node_index, pk, sk);

    deregister.votes.push_back({ signature, (uint32_t)voter.idx_in_quorum });
  }

  if (commit) deregistration_buffer_.push_back(pk);

  const auto deregister_tx = make_deregistration_tx(events_, first_miner_, blocks_.back(), deregister, gen_.m_hf_version, fee);

  events_.push_back(deregister_tx);

  return deregister_tx;
}

crypto::public_key linear_chain_generator::get_test_pk(uint32_t idx) const
{
  const auto& to_test = get_quorum_idxs(height()).to_test;

  return to_test.at(idx).sn_pk;
}

boost::optional<uint32_t> linear_chain_generator::get_idx_in_tested(const crypto::public_key& pk, uint64_t height) const
{
  const auto& to_test = get_quorum_idxs(height).to_test;

  for (const auto& sn : to_test) {
    if (sn.sn_pk == pk) return sn.idx_in_quorum - full_nodes::QUORUM_SIZE;
  }

  return boost::none;
}

void linear_chain_generator::deregister(const crypto::public_key& pk) {
  sn_list_.remove_node(pk);
}

inline void sn_list::remove_node(const crypto::public_key& pk)
{
  const auto it =
    std::find_if(sn_owners_.begin(), sn_owners_.end(), [pk](const sn_registration& sn) { return sn.keys.pub == pk; });
  if (it != sn_owners_.end()) sn_owners_.erase(it); else abort();
}

inline void sn_list::add_registrations(const std::vector<sn_registration>& regs)
{
  sn_owners_.insert(sn_owners_.begin(), regs.begin(), regs.end());

  std::sort(sn_owners_.begin(), sn_owners_.end(),
  [](const sn_registration &a, const sn_registration &b) {
    return memcmp(reinterpret_cast<const void*>(&a.keys.pub), reinterpret_cast<const void*>(&b.keys.pub),
    sizeof(a.keys.pub)) < 0;
  });
}

void sn_list::handle_deregistrations(const std::vector<crypto::public_key>& dereg_buffer)
{
  const auto size_before = sn_owners_.size();
  auto end_it = sn_owners_.end();

  for (const auto pk : dereg_buffer) {
    end_it = std::remove_if(sn_owners_.begin(), end_it, [&pk](const sn_registration& sn) {
      return sn.keys.pub == pk;
    });
  }

  sn_owners_.erase(end_it, sn_owners_.end());
  assert(sn_owners_.size() == size_before - dereg_buffer.size());
}

inline void sn_list::expire_old(uint64_t height)
{
  /// remove_if is stable, no need for re-sorting
  const auto new_end = std::remove_if(
    sn_owners_.begin(), sn_owners_.end(), [height](const sn_registration& reg) { return height > reg.valid_until; });

  sn_owners_.erase(new_end, sn_owners_.end());
}

inline const boost::optional<sn_registration> sn_list::find_registration(const crypto::public_key& pk) const
{
  const auto it =
    std::find_if(sn_owners_.begin(), sn_owners_.end(), [pk](const sn_registration& sn) { return sn.keys.pub == pk; });

  if (it == sn_owners_.end()) return boost::none;

  return *it;
}

inline const boost::optional<crypto::public_key> sn_list::get_winner_pk(uint64_t height)
{
  if (sn_owners_.empty()) return boost::none;

  auto it =
    std::min_element(sn_owners_.begin(), sn_owners_.end(), [](const sn_registration& lhs, const sn_registration& rhs) {
      return lhs.last_reward < rhs.last_reward;
    });

  it->last_reward.height = height;

  return it->keys.pub;
}
/// --------------------------------------------------------------
void test_generator::get_block_chain(std::vector<block_info>& blockchain, const crypto::hash& head, size_t n) const
{
  crypto::hash curr = head;
  while (null_hash != curr && blockchain.size() < n)
  {
    auto it = m_blocks_info.find(curr);
    if (m_blocks_info.end() == it)
    {
      throw std::runtime_error("block hash wasn't found");
    }

    blockchain.push_back(it->second);
    curr = it->second.prev_id;
  }

  std::reverse(blockchain.begin(), blockchain.end());
}

// TODO(antd): Copypasta
void test_generator::get_block_chain(std::vector<cryptonote::block>& blockchain, const crypto::hash& head, size_t n) const
{
  crypto::hash curr = head;
  while (null_hash != curr && blockchain.size() < n)
  {
    auto it = m_blocks_info.find(curr);
    if (m_blocks_info.end() == it)
    {
      throw std::runtime_error("block hash wasn't found");
    }

    blockchain.push_back(it->second.block);
    curr = it->second.prev_id;
  }

  std::reverse(blockchain.begin(), blockchain.end());
}

void test_generator::get_last_n_block_weights(std::vector<uint64_t>& block_weights, const crypto::hash& head, size_t n) const
{
  std::vector<block_info> blockchain;
  get_block_chain(blockchain, head, n);
  BOOST_FOREACH(auto& bi, blockchain)
  {
    block_weights.push_back(bi.block_weight);
  }
}

uint64_t test_generator::get_already_generated_coins(const crypto::hash& blk_id) const
{
  auto it = m_blocks_info.find(blk_id);
  if (it == m_blocks_info.end())
    throw std::runtime_error("block hash wasn't found");

  return it->second.already_generated_coins;
}

uint64_t test_generator::get_already_generated_coins(const cryptonote::block& blk) const
{
  crypto::hash blk_hash;
  get_block_hash(blk, blk_hash);
  return get_already_generated_coins(blk_hash);
}

void test_generator::add_block(const cryptonote::block& blk, size_t txs_weight, std::vector<uint64_t>& block_weights, uint64_t already_generated_coins)
{
  const size_t block_weight = txs_weight + get_transaction_weight(blk.miner_tx);

  uint64_t block_reward;
  cryptonote::get_base_block_reward(misc_utils::median(block_weights), block_weight, already_generated_coins, block_reward, m_hf_version, 0);

  m_blocks_info.insert({get_block_hash(blk), block_info(blk.prev_id, already_generated_coins + block_reward, block_weight, blk)});
}

static void manual_calc_batched_control(const test_generator &generator, const crypto::hash &head, antd_miner_tx_context &miner_tx_context, int hard_fork_version, uint64_t height)
{
  miner_tx_context.batched_control = 0;

  if (hard_fork_version >= cryptonote::network_version_10_bulletproofs &&
      cryptonote::height_has_control_output(cryptonote::FAKECHAIN, hard_fork_version, height))
  {
    const cryptonote::config_t &network = cryptonote::get_config(cryptonote::FAKECHAIN, hard_fork_version);
    uint64_t num_blocks                 = network.CONTROL_REWARD_INTERVAL_IN_BLOCKS;
    uint64_t start_height               = height - num_blocks;

    if (height < num_blocks)
    {
      start_height = 0;
      num_blocks   = height;
    }

    std::vector<block> blockchain;
    blockchain.reserve(num_blocks);
    generator.get_block_chain(blockchain, head, num_blocks);

    for (const block &entry : blockchain)
    {
      uint64_t block_height = cryptonote::get_block_height(entry);
      if (block_height < start_height)
        continue;

      if (entry.major_version >= network_version_10_bulletproofs)
        miner_tx_context.batched_control += cryptonote::derive_control_from_block_reward(cryptonote::FAKECHAIN, entry);
    }
  }

}

bool test_generator::construct_block(cryptonote::block& blk, uint64_t height, const crypto::hash& prev_id,
                                     const cryptonote::account_base& miner_acc, uint64_t timestamp, uint64_t already_generated_coins,
                                     std::vector<uint64_t>& block_weights, const std::list<cryptonote::transaction>& tx_list,
                                     const crypto::public_key& sn_pub_key /* = crypto::null_key */, const std::vector<sn_contributor_t>& sn_infos)
{
  /// a temporary workaround
  blk.major_version = m_hf_version;
  blk.minor_version = m_hf_version;
  blk.timestamp = timestamp;
  blk.prev_id = prev_id;

  blk.tx_hashes.reserve(tx_list.size());
  BOOST_FOREACH(const transaction &tx, tx_list)
  {
    crypto::hash tx_hash;
    get_transaction_hash(tx, tx_hash);
    blk.tx_hashes.push_back(tx_hash);
  }

  uint64_t total_fee = 0;
  size_t txs_weight = 0;
  BOOST_FOREACH(auto& tx, tx_list)
  {
    uint64_t fee = 0;
    bool r = get_tx_fee(tx, fee);
    CHECK_AND_ASSERT_MES(r, false, "wrong transaction passed to construct_block");
    total_fee += fee;
    txs_weight += get_transaction_weight(tx);
  }

  blk.miner_tx = AUTO_VAL_INIT(blk.miner_tx);
  size_t target_block_weight = txs_weight + get_transaction_weight(blk.miner_tx);

  cryptonote::antd_miner_tx_context miner_tx_context(cryptonote::FAKECHAIN, sn_pub_key, sn_infos);
  manual_calc_batched_control(*this, prev_id, miner_tx_context, m_hf_version, height);

  while (true)
  {
    if (!construct_miner_tx(height, misc_utils::median(block_weights), already_generated_coins, target_block_weight, total_fee, miner_acc.get_keys().m_account_address, blk.miner_tx, blobdata(), m_hf_version, miner_tx_context))
      return false;

    size_t actual_block_weight = txs_weight + get_transaction_weight(blk.miner_tx);
    if (target_block_weight < actual_block_weight)
    {
      target_block_weight = actual_block_weight;
    }
    else if (actual_block_weight < target_block_weight)
    {
      size_t delta = target_block_weight - actual_block_weight;
      blk.miner_tx.extra.resize(blk.miner_tx.extra.size() + delta, 0);
      actual_block_weight = txs_weight + get_transaction_weight(blk.miner_tx);
      if (actual_block_weight == target_block_weight)
      {
        break;
      }
      else
      {
        CHECK_AND_ASSERT_MES(target_block_weight < actual_block_weight, false, "Unexpected block size");
        delta = actual_block_weight - target_block_weight;
        blk.miner_tx.extra.resize(blk.miner_tx.extra.size() - delta);
        actual_block_weight = txs_weight + get_transaction_weight(blk.miner_tx);
        if (actual_block_weight == target_block_weight)
        {
          break;
        }
        else
        {
          CHECK_AND_ASSERT_MES(actual_block_weight < target_block_weight, false, "Unexpected block size");
          blk.miner_tx.extra.resize(blk.miner_tx.extra.size() + delta, 0);
          target_block_weight = txs_weight + get_transaction_weight(blk.miner_tx);
        }
      }
    }
    else
    {
      break;
    }
  }

  //blk.tree_root_hash = get_tx_tree_hash(blk);

  // Nonce search...
  blk.nonce = 0;
  while (!miner::find_nonce_for_given_block(nullptr, blk, get_test_difficulty(), height))
    blk.timestamp++;

  add_block(blk, txs_weight, block_weights, already_generated_coins);

  return true;
}

bool test_generator::construct_block(cryptonote::block& blk, const cryptonote::account_base& miner_acc, uint64_t timestamp)
{
  std::vector<uint64_t> block_weights;
  std::list<cryptonote::transaction> tx_list;
  return construct_block(blk, 0, null_hash, miner_acc, timestamp, 0, block_weights, tx_list);
}

bool test_generator::construct_block(cryptonote::block& blk, const cryptonote::block& blk_prev,
                                     const cryptonote::account_base& miner_acc,
                                     const std::list<cryptonote::transaction>& tx_list/* = {}*/,
                                     const crypto::public_key& sn_pub_key /* = crypto::null_key */, const std::vector<sn_contributor_t>& sn_infos)
{
  uint64_t height = boost::get<txin_gen>(blk_prev.miner_tx.vin.front()).height + 1;
  crypto::hash prev_id = get_block_hash(blk_prev);
  // Keep difficulty unchanged
  uint64_t timestamp = blk_prev.timestamp + DIFFICULTY_BLOCKS_ESTIMATE_TIMESPAN;
  uint64_t already_generated_coins = get_already_generated_coins(prev_id);
  std::vector<uint64_t> block_weights;
  get_last_n_block_weights(block_weights, prev_id, CRYPTONOTE_REWARD_BLOCKS_WINDOW);

  return construct_block(blk, height, prev_id, miner_acc, timestamp, already_generated_coins, block_weights, tx_list, sn_pub_key, sn_infos);
}

bool test_generator::construct_block_manually(block& blk, const block& prev_block, const account_base& miner_acc,
                                              int actual_params/* = bf_none*/, uint8_t major_ver/* = 0*/,
                                              uint8_t minor_ver/* = 0*/, uint64_t timestamp/* = 0*/,
                                              const crypto::hash& prev_id/* = crypto::hash()*/, const difficulty_type& diffic/* = 1*/,
                                              const transaction& miner_tx/* = transaction()*/,
                                              const std::vector<crypto::hash>& tx_hashes/* = std::vector<crypto::hash>()*/,
                                              size_t txs_weight/* = 0*/)
{
  blk.major_version = actual_params & bf_major_ver ? major_ver : CURRENT_BLOCK_MAJOR_VERSION;
  blk.minor_version = actual_params & bf_minor_ver ? minor_ver : CURRENT_BLOCK_MINOR_VERSION;
  blk.timestamp     = actual_params & bf_timestamp ? timestamp : prev_block.timestamp + DIFFICULTY_BLOCKS_ESTIMATE_TIMESPAN; // Keep difficulty unchanged
  blk.prev_id       = actual_params & bf_prev_id   ? prev_id   : get_block_hash(prev_block);
  blk.tx_hashes     = actual_params & bf_tx_hashes ? tx_hashes : std::vector<crypto::hash>();

  size_t height = get_block_height(prev_block) + 1;
  uint64_t already_generated_coins = get_already_generated_coins(prev_block);
  std::vector<uint64_t> block_weights;
  get_last_n_block_weights(block_weights, get_block_hash(prev_block), CRYPTONOTE_REWARD_BLOCKS_WINDOW);
  if (actual_params & bf_miner_tx)
  {
    blk.miner_tx = miner_tx;
  }
  else
  {
    // TODO: This will work, until size of constructed block is less then CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE
    cryptonote::antd_miner_tx_context miner_tx_context(cryptonote::FAKECHAIN);
    manual_calc_batched_control(*this, prev_id, miner_tx_context, m_hf_version, height);

    size_t current_block_weight = txs_weight + get_transaction_weight(blk.miner_tx);
    if (!construct_miner_tx(height, misc_utils::median(block_weights), already_generated_coins, current_block_weight, 0, miner_acc.get_keys().m_account_address, blk.miner_tx, blobdata(), m_hf_version, miner_tx_context))
      return false;
  }

  //blk.tree_root_hash = get_tx_tree_hash(blk);

  difficulty_type a_diffic = actual_params & bf_diffic ? diffic : get_test_difficulty();
  fill_nonce(blk, a_diffic, height);

  add_block(blk, txs_weight, block_weights, already_generated_coins);

  return true;
}

bool test_generator::construct_block_manually_tx(cryptonote::block& blk, const cryptonote::block& prev_block,
                                                 const cryptonote::account_base& miner_acc,
                                                 const std::vector<crypto::hash>& tx_hashes, size_t txs_weight)
{
  return construct_block_manually(blk, prev_block, miner_acc, bf_tx_hashes, 0, 0, 0, crypto::hash(), 0, transaction(), tx_hashes, txs_weight);
}

cryptonote::transaction make_registration_tx(std::vector<test_event_entry>& events,
                                             const cryptonote::account_base& account,
                                             const cryptonote::keypair& full_node_keys,
                                             uint64_t operator_cut,
                                             const std::vector<cryptonote::account_public_address>& addresses,
                                             const std::vector<uint64_t>& portions,
                                             const cryptonote::block& head,
                                             uint8_t hf_version)
{
    const auto new_height = cryptonote::get_block_height(head) + 1;
    const auto staking_requirement = full_nodes::get_staking_requirement(cryptonote::FAKECHAIN, new_height, hf_version);

    uint64_t amount = full_nodes::portions_to_amount(portions[0], staking_requirement);

    cryptonote::transaction tx;
    const auto unlock_time = new_height + full_nodes::staking_num_lock_blocks(cryptonote::FAKECHAIN);

    std::vector<uint8_t> extra;
    add_full_node_pubkey_to_tx_extra(extra, full_node_keys.pub);

    const uint64_t exp_timestamp = time(nullptr) + STAKING_AUTHORIZATION_EXPIRATION_WINDOW;

    crypto::hash hash;
    if (!cryptonote::get_registration_hash(addresses, operator_cut, portions, exp_timestamp, hash))
    {
      MERROR("Could not make registration hash from addresses and portions");
      return {};
    }

    crypto::signature signature;
    crypto::generate_signature(hash, full_node_keys.pub, full_node_keys.sec, signature);

    add_full_node_register_to_tx_extra(extra, addresses, operator_cut, portions, exp_timestamp, signature);
    add_full_node_contributor_to_tx_extra(extra, addresses.at(0));

    TxBuilder(events, tx, head, account, account, amount, hf_version).is_staking(true).with_extra(extra).with_unlock_time(unlock_time).with_per_output_unlock(true).build();
    events.push_back(tx);
    return tx;
}

cryptonote::transaction make_deregistration_tx(const std::vector<test_event_entry>& events,
                                               const cryptonote::account_base& account,
                                               const cryptonote::block& head,
                                               const cryptonote::tx_extra_full_node_deregister& deregister,
                                               uint8_t hf_version,
                                               uint64_t fee)
{
  cryptonote::transaction tx;

  std::vector<uint8_t> extra;
  const bool full_tx_deregister_made = cryptonote::add_full_node_deregister_to_tx_extra(tx.extra, deregister);

  if (!full_tx_deregister_made) {
    MERROR("Could not add deregister to extra");
    return {};
  }

  const uint64_t amount = 0;

  if (fee) TxBuilder(events, tx, head, account, account, amount, hf_version).with_fee(fee).with_extra(extra).with_per_output_unlock(true).build();

  tx.version = cryptonote::transaction::get_max_version_for_hf(hf_version, cryptonote::FAKECHAIN);
  tx.type = cryptonote::transaction::type_deregister;

  return tx;
}

cryptonote::transaction make_default_registration_tx(std::vector<test_event_entry>& events,
                                             const cryptonote::account_base& account,
                                             const cryptonote::keypair& full_node_keys,
                                             const cryptonote::block& head,
                                             uint8_t hf_version)
{
  return make_registration_tx(events, account, full_node_keys, 0, { account.get_keys().m_account_address }, { STAKING_PORTIONS }, head, hf_version);
}

struct output_index {
    const cryptonote::txout_target_v out;
    uint64_t amount;
    rct::key mask;
    size_t blk_height; // block height
    uint64_t unlock_time;
    size_t tx_no; // index of transaction in block
    size_t out_no; // index of out in transaction
    size_t idx;
    bool spent;
    bool is_sn_reward = false;
    const cryptonote::block *p_blk;
    const cryptonote::transaction *p_tx;

    output_index(const cryptonote::txout_target_v &_out, uint64_t _a, size_t _h, uint64_t ut, size_t tno, size_t ono, const cryptonote::block *_pb, const cryptonote::transaction *_pt)
        : out(_out), amount(_a), blk_height(_h), unlock_time(ut), tx_no(tno), out_no(ono), idx(0), spent(false), p_blk(_pb), p_tx(_pt) { }

    output_index(const output_index &other) = default;

    const std::string toString() const {
        std::stringstream ss;

        ss << "output_index{blk_height=" << blk_height
           << " tx_no=" << tx_no
           << " out_no=" << out_no
           << " amount=" << amount
           << " mask=" << mask
           << " idx=" << idx
           << " spent=" << spent
           << "}";

        return ss.str();
    }

    output_index& operator=(const output_index& other)
    {
      new(this) output_index(other);
      return *this;
    }
};

typedef std::map<uint64_t, std::vector<size_t> > map_output_t;
typedef std::map<uint64_t, std::vector<output_index> > map_output_idx_t;
typedef std::vector<output_index> output_index_vec;

typedef std::vector<size_t> output_vec;
typedef pair<uint64_t, size_t>  outloc_t;

namespace
{
  uint64_t get_inputs_amount(const vector<tx_source_entry> &s)
  {
    uint64_t r = 0;
    BOOST_FOREACH(const tx_source_entry &e, s)
    {
      r += e.amount;
    }

    return r;
  }
}

uint64_t get_amount(const cryptonote::account_base& account, const cryptonote::transaction& tx, rct::key& mask, int i)
{
  crypto::public_key tx_pub_key = get_tx_pub_key_from_extra(tx);
  crypto::key_derivation derivation;
  if (!crypto::generate_key_derivation(tx_pub_key, account.get_keys().m_view_secret_key, derivation))
    return 0;

  if (tx.vout[i].target.type() != typeid(cryptonote::txout_to_key))
    return 0;

  hw::device& hwdev = hw::get_device("default");

  uint64_t money_transferred = 0;

  crypto::secret_key scalar1;
  hwdev.derivation_to_scalar(derivation, i, scalar1);
  try
  {
    switch (tx.rct_signatures.type)
    {
    case rct::RCTTypeSimple:
    case rct::RCTTypeBulletproof:
      money_transferred = rct::decodeRctSimple(tx.rct_signatures, rct::sk2rct(scalar1), i, mask, hwdev);
      break;
    case rct::RCTTypeFull:
      money_transferred = rct::decodeRct(tx.rct_signatures, rct::sk2rct(scalar1), i, hwdev);
      break;
    case rct::RCTTypeNull:
      money_transferred = tx.vout[i].amount;
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

uint64_t get_amount(const cryptonote::account_base& account, const cryptonote::transaction& tx, int i)
{
  rct::key mask_unused;
  return get_amount(account, tx, mask_unused, i);
}

bool init_output_indices(output_index_vec& outs, output_vec& outs_mine, const std::vector<cryptonote::block>& blockchain, const map_hash2tx_t& mtx, const cryptonote::account_base& from) {

    for (const block& blk : blockchain) {
        vector<const transaction*> vtx;
        vtx.push_back(&blk.miner_tx);

        for(const crypto::hash &h : blk.tx_hashes) {
            const auto cit = mtx.find(h);
            if (mtx.end() == cit)
                throw std::runtime_error("block contains an unknown tx hash");

            vtx.push_back(cit->second);
        }

        for (size_t i = 0; i < vtx.size(); i++) {
            const transaction &tx = *vtx[i];

            for (size_t j = 0; j < tx.vout.size(); ++j) {
                const tx_out &out = tx.vout[j];

                if (out.target.which() == 2) { // out_to_key

                    const auto height = boost::get<txin_gen>(*blk.miner_tx.vin.begin()).height; /// replace with front?

                    const auto unlock_time = (tx.version < 3) ? tx.unlock_time : tx.output_unlock_times[j];

                    outs.push_back({out.target, out.amount, height, unlock_time, i, j, &blk, vtx[i]});
                    size_t tx_global_idx = outs.size() - 1;
                    outs[tx_global_idx].idx = tx_global_idx;
                    outs[tx_global_idx].mask = rct::zeroCommit(out.amount);
                    // Is out to me?
                    const auto gov_key = cryptonote::get_deterministic_keypair_from_height(height);

                    const bool to_acc_regular = is_out_to_acc(from.get_keys(), boost::get<txout_to_key>(out.target), get_tx_pub_key_from_extra(tx), get_additional_tx_pub_keys_from_extra(tx), j);
                    const bool to_acc_sn_reward = to_acc_regular ? false : is_out_to_acc(from.get_keys(), boost::get<txout_to_key>(out.target), gov_key.pub, {}, j);

                    if (to_acc_regular || to_acc_sn_reward) {
                      outs_mine.push_back(tx_global_idx);
                        auto& oi = outs.back();
                        oi.is_sn_reward = to_acc_sn_reward;
                        if (oi.amount == 0) {
                          oi.amount = get_amount(from, tx, j);
                          oi.mask = tx.rct_signatures.outPk[j].mask;
                        }

                    }
                }
            }
        }
    }

    return true;
}

bool init_spent_output_indices(output_index_vec& outs,
                               const output_vec& outs_mine,
                               const std::vector<cryptonote::block>& blockchain,
                               const map_hash2tx_t& mtx,
                               const cryptonote::account_base& from)
{

    for (size_t out_idx : outs_mine) {
        output_index& oi = outs[out_idx];

        // construct key image for this output
        crypto::key_image img;
        keypair in_ephemeral;
        crypto::public_key out_key = boost::get<txout_to_key>(oi.out).key;
        std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
        subaddresses[from.get_keys().m_account_address.m_spend_public_key] = {0,0};

        const auto tx_pk = oi.is_sn_reward ? get_deterministic_keypair_from_height(oi.blk_height).pub
                                           : get_tx_pub_key_from_extra(*oi.p_tx);

        generate_key_image_helper(from.get_keys(),
                                  subaddresses,
                                  out_key,
                                  tx_pk,
                                  get_additional_tx_pub_keys_from_extra(*oi.p_tx),
                                  oi.out_no,
                                  in_ephemeral,
                                  img,
                                  hw::get_device(("default")));

        // lookup for this key image in the events vector
        BOOST_FOREACH(auto& tx_pair, mtx) {
            const transaction& tx = *tx_pair.second;
            BOOST_FOREACH(const txin_v &in, tx.vin) {
                if (typeid(txin_to_key) == in.type()) {
                    const txin_to_key &itk = boost::get<txin_to_key>(in);
                    if (itk.k_image == img) {
                        oi.spent = true;
                    }
                }
            }
        }
    }

    return true;
}

static bool fill_output_entries(const std::vector<output_index>& out_indices, size_t sender_out, size_t nmix, size_t& real_entry_idx, std::vector<tx_source_entry::output_entry>& output_entries)
{
  if (out_indices.size() <= nmix)
    return false;

  bool sender_out_found = false;
  size_t rest = nmix;
  for (size_t i = 0; i < out_indices.size() && (0 < rest || !sender_out_found); ++i)
  {
    const output_index& oi = out_indices[i];
    if (oi.spent)
      continue;

    bool append = false;
    if (i == sender_out)
    {
      append = true;
      sender_out_found = true;
      real_entry_idx = output_entries.size();
    }
    else if (0 < rest)
    {
      --rest;
      append = true;
    }

    if (append)
    {
      const txout_to_key& otk = boost::get<txout_to_key>(oi.out);
      output_entries.push_back(tx_source_entry::output_entry(oi.idx, rct::ctkey({rct::pk2rct(otk.key), oi.mask})));
    }
  }

  return 0 == rest && sender_out_found;
}

bool fill_tx_sources(std::vector<tx_source_entry>& sources, const std::vector<test_event_entry>& events,
                     const block& blk_head, const cryptonote::account_base& from, uint64_t amount, size_t nmix)
{
    /// Don't fill up sources if the amount is zero
    if (amount == 0) return true;

    output_index_vec outs;
    output_vec outs_mine;

    std::vector<cryptonote::block> blockchain;
    map_hash2tx_t mtx;
    if (!find_block_chain(events, blockchain, mtx, get_block_hash(blk_head)))
        return false;

    if (!init_output_indices(outs, outs_mine, blockchain, mtx, from))
        return false;

    if (!init_spent_output_indices(outs, outs_mine, blockchain, mtx, from))
        return false;

    // Iterate in reverse is more efficiency
    uint64_t sources_amount = 0;
    bool sources_found = false;

    for (const size_t sender_out : outs_mine) {

        const output_index& oi = outs[sender_out];
        if (oi.spent) continue;

        if (!cryptonote::rules::is_output_unlocked(oi.unlock_time, get_block_height(blk_head))) continue;

        cryptonote::tx_source_entry ts;

        const auto& tx = *oi.p_tx;
        ts.amount = oi.amount;
        ts.real_output_in_tx_index = oi.out_no;
        ts.real_out_tx_key = get_tx_pub_key_from_extra(tx); // incoming tx public key
        ts.real_out_additional_tx_keys = get_additional_tx_pub_keys_from_extra(tx);
        ts.mask = rct::identity();
        ts.rct = true;

        /// Filling in the mask
        {
            crypto::key_derivation derivation;
            bool r = crypto::generate_key_derivation(ts.real_out_tx_key, from.get_keys().m_view_secret_key, derivation);
            CHECK_AND_ASSERT_MES(r, false, "Failed to generate key derivation");
            crypto::secret_key amount_key;
            crypto::derivation_to_scalar(derivation, oi.out_no, amount_key);
            if (tx.rct_signatures.type == rct::RCTTypeSimple || tx.rct_signatures.type == rct::RCTTypeBulletproof)
                rct::decodeRctSimple(
                  tx.rct_signatures, rct::sk2rct(amount_key), oi.out_no, ts.mask, hw::get_device("default"));
            else if (tx.rct_signatures.type == rct::RCTTypeFull)
                rct::decodeRct(
                  tx.rct_signatures, rct::sk2rct(amount_key), oi.out_no, ts.mask, hw::get_device("default"));
        }

        if (!fill_output_entries(outs, sender_out, nmix, ts.real_output, ts.outputs)) continue;

        sources.push_back(ts);

        sources_amount += ts.amount;

        sources_found = amount <= sources_amount;
        if (sources_found) return true;
    }

    return false;
}

bool fill_tx_destination(tx_destination_entry &de, const cryptonote::account_base &to, uint64_t amount) {
    de.addr = to.get_keys().m_account_address;
    de.amount = amount;
    return true;
}

void fill_tx_sources_and_multi_destinations(const std::vector<test_event_entry>& events, const block& blk_head,
                                            const cryptonote::account_base& from, const cryptonote::account_base& to,
                                            uint64_t const *amount, int num_amounts, uint64_t fee, size_t nmix, std::vector<tx_source_entry>& sources,
                                            std::vector<tx_destination_entry>& destinations, uint64_t *change_amount)
{
  sources.clear();
  destinations.clear();

  uint64_t total_amount = fee;
  for (int i = 0; i < num_amounts; ++i)
    total_amount += amount[i];

  if (!fill_tx_sources(sources, events, blk_head, from, total_amount, nmix))
    throw std::runtime_error("couldn't fill transaction sources");

  for (int i = 0; i < num_amounts; ++i)
  {
    tx_destination_entry de;
    if (!fill_tx_destination(de, to, amount[i]))
      throw std::runtime_error("couldn't fill transaction destination");
    destinations.push_back(de);
  }

  tx_destination_entry de_change;
  uint64_t cash_back = get_inputs_amount(sources) - (total_amount);
  if (0 < cash_back)
  {
    if (!fill_tx_destination(de_change, from, cash_back))
      throw std::runtime_error("couldn't fill transaction cache back destination");
    destinations.push_back(de_change);
  }

  if (change_amount) *change_amount = (cash_back > 0) ? cash_back : 0;
}

void fill_tx_sources_and_destinations(const std::vector<test_event_entry>& events, const block& blk_head,
                                      const cryptonote::account_base& from, const cryptonote::account_base& to,
                                      uint64_t amount, uint64_t fee, size_t nmix, std::vector<tx_source_entry>& sources,
                                      std::vector<tx_destination_entry>& destinations, uint64_t *change_amount)
{
  uint64_t *amounts = &amount;
  int num_amounts   = 1;
  fill_tx_sources_and_multi_destinations(events, blk_head, from, to, amounts, num_amounts, fee, nmix, sources, destinations, change_amount);
}

void fill_nonce(cryptonote::block& blk, const difficulty_type& diffic, uint64_t height)
{
  blk.nonce = 0;
  while (!miner::find_nonce_for_given_block(NULL, blk, diffic, height))
    blk.timestamp++;
}

crypto::public_key get_output_key(const keypair& txkey,
                                  const cryptonote::account_public_address& addr,
                                  size_t output_index)
{
    crypto::key_derivation derivation;
    crypto::generate_key_derivation(addr.m_view_public_key, txkey.sec, derivation);
    crypto::public_key out_eph_public_key;
    crypto::derive_public_key(derivation, output_index, addr.m_spend_public_key, out_eph_public_key);
    return out_eph_public_key;
}

transaction construct_tx_with_fee(std::vector<test_event_entry>& events, const block& blk_head,
                                  const account_base& acc_from, const account_base& acc_to, uint64_t amount, uint64_t fee)
{
  transaction tx;
  TxBuilder(events, tx, blk_head, acc_from, acc_to, amount, cryptonote::network_version_7).with_fee(fee).build();
  events.push_back(tx);
  return tx;
}

uint64_t get_balance(const cryptonote::account_base& addr, const std::vector<cryptonote::block>& blockchain, const map_hash2tx_t& mtx) {
    uint64_t res = 0;
    output_index_vec outs;
    output_vec outs_mine;

    map_hash2tx_t confirmed_txs;
    get_confirmed_txs(blockchain, mtx, confirmed_txs);

    if (!init_output_indices(outs, outs_mine, blockchain, confirmed_txs, addr))
        return false;

    if (!init_spent_output_indices(outs, outs_mine, blockchain, confirmed_txs, addr))
        return false;

    for (const size_t out_idx : outs_mine) {
            if (outs[out_idx].spent) continue;
            res += outs[out_idx].amount;
    }

    return res;
}

uint64_t get_unlocked_balance(const cryptonote::account_base& addr, const std::vector<cryptonote::block>& blockchain, const map_hash2tx_t& mtx) {

    if (blockchain.empty()) return 0;

    uint64_t res = 0;
    output_index_vec outs;
    output_vec outs_mine;

    map_hash2tx_t confirmed_txs;
    get_confirmed_txs(blockchain, mtx, confirmed_txs);

    if (!init_output_indices(outs, outs_mine, blockchain, confirmed_txs, addr))
        return false;

    if (!init_spent_output_indices(outs, outs_mine, blockchain, confirmed_txs, addr))
        return false;

    for (const size_t out_idx : outs_mine) {
        const auto unlocked = rules::is_output_unlocked(outs[out_idx].unlock_time, get_block_height(blockchain.back()));
        if (outs[out_idx].spent || !unlocked) continue;
        res += outs[out_idx].amount;
    }

    return res;
}

void get_confirmed_txs(const std::vector<cryptonote::block>& blockchain, const map_hash2tx_t& mtx, map_hash2tx_t& confirmed_txs)
{
  std::unordered_set<crypto::hash> confirmed_hashes;
  BOOST_FOREACH(const block& blk, blockchain)
  {
    BOOST_FOREACH(const crypto::hash& tx_hash, blk.tx_hashes)
    {
      confirmed_hashes.insert(tx_hash);
    }
  }

  BOOST_FOREACH(const auto& tx_pair, mtx)
  {
    if (0 != confirmed_hashes.count(tx_pair.first))
    {
      confirmed_txs.insert(tx_pair);
    }
  }
}

bool find_block_chain(const std::vector<test_event_entry>& events, std::vector<cryptonote::block>& blockchain, map_hash2tx_t& mtx, const crypto::hash& head) {
    std::unordered_map<crypto::hash, const block*> block_index;
    BOOST_FOREACH(const test_event_entry& ev, events)
    {
        if (typeid(block) == ev.type())
        {
            const block* blk = &boost::get<block>(ev);
            block_index[get_block_hash(*blk)] = blk;
        }
        else if (typeid(transaction) == ev.type())
        {
            const transaction& tx = boost::get<transaction>(ev);
            mtx[get_transaction_hash(tx)] = &tx;
        }
    }

    bool b_success = false;
    crypto::hash id = head;
    for (auto it = block_index.find(id); block_index.end() != it; it = block_index.find(id))
    {
        blockchain.push_back(*it->second);
        id = it->second->prev_id;
        if (null_hash == id)
        {
            b_success = true;
            break;
        }
    }
    reverse(blockchain.begin(), blockchain.end());

    return b_success;
}


void test_chain_unit_base::register_callback(const std::string& cb_name, verify_callback cb)
{
  m_callbacks[cb_name] = cb;
}
bool test_chain_unit_base::verify(const std::string& cb_name, cryptonote::core& c, size_t ev_index, const std::vector<test_event_entry> &events)
{
  auto cb_it = m_callbacks.find(cb_name);
  if(cb_it == m_callbacks.end())
  {
    LOG_ERROR("Failed to find callback " << cb_name);
    return false;
  }
  return cb_it->second(c, ev_index, events);
}
