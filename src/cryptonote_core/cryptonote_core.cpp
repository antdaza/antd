// Copyright (c) 2014-2025, The Monero Project
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
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <boost/algorithm/string.hpp>

#include "string_tools.h"
using namespace epee;

#include <unordered_set>
#include <iomanip>

#include "cryptonote_core.h"
#include "common/util.h"
#include "common/updates.h"
#include "common/download.h"
#include "common/threadpool.h"
#include "common/command_line.h"
#include "warnings.h"
#include "crypto/crypto.h"
#include "cryptonote_config.h"
#include "misc_language.h"
#include "file_io_utils.h"
#include <csignal>
#include "constants.h"
#include "checkpoints/checkpoints.h"
#include "ringct/rctTypes.h"
#include "hardforks/hardforks.h"
#include "blockchain_db/blockchain_db.h"
#include "ringct/rctSigs.h"
#include "common/notify.h"
#include "version.h"
#include "wipeable_string.h"
#include "common/i18n.h"

#include "common/antd_integration_test_hooks.h"

#undef ANTD_DEFAULT_LOG_CATEGORY
#define ANTD_DEFAULT_LOG_CATEGORY "cn"

DISABLE_VS_WARNINGS(4355)

#define MERROR_VER(x) MCERROR("verify", x)

#define BAD_SEMANTICS_TXES_MAX_SIZE 100

namespace cryptonote
{
  const command_line::arg_descriptor<bool, false> arg_testnet_on  = {
    "testnet"
  , "Run on testnet. The wallet must be launched with --testnet flag."
  , false
  };
  const command_line::arg_descriptor<bool, false> arg_stagenet_on  = {
    "stagenet"
  , "Run on stagenet. The wallet must be launched with --stagenet flag."
  , false
  };
  const command_line::arg_descriptor<bool> arg_regtest_on  = {
    "regtest"
  , "Run in a regression testing mode."
  , false
  };
  const command_line::arg_descriptor<difficulty_type> arg_fixed_difficulty  = {
    "fixed-difficulty"
  , "Fixed difficulty used for testing."
  , 0
  };
  const command_line::arg_descriptor<std::string, false, true, 2> arg_data_dir = {
    "data-dir"
  , "Specify data directory"
  , tools::get_default_data_dir()
  , {{ &arg_testnet_on, &arg_stagenet_on }}
  , [](std::array<bool, 2> testnet_stagenet, bool defaulted, std::string val)->std::string {
      if (testnet_stagenet[0])
        return (boost::filesystem::path(val) / "testnet").string();
      else if (testnet_stagenet[1])
        return (boost::filesystem::path(val) / "stagenet").string();
      return val;
    }
  };
  const command_line::arg_descriptor<bool> arg_offline = {
    "offline"
  , "Do not listen for peers, nor connect to any"
  };
  const command_line::arg_descriptor<bool> arg_disable_dns_checkpoints = {
    "disable-dns-checkpoints"
  , "Do not retrieve checkpoints from DNS"
  };
  const command_line::arg_descriptor<size_t> arg_block_download_max_size  = {
    "block-download-max-size"
  , "Set maximum size of block download queue in bytes (0 for default)"
  , 0
  };

  static const command_line::arg_descriptor<bool> arg_test_drop_download = {
    "test-drop-download"
  , "For net tests: in download, discard ALL blocks instead checking/saving them (very fast)"
  };
  static const command_line::arg_descriptor<uint64_t> arg_test_drop_download_height = {
    "test-drop-download-height"
  , "Like test-drop-download but discards only after around certain height"
  , 0
  };
  static const command_line::arg_descriptor<int> arg_test_dbg_lock_sleep = {
    "test-dbg-lock-sleep"
  , "Sleep time in ms, defaults to 0 (off), used to debug before/after locking mutex. Values 100 to 1000 are good for tests."
  , 0
  };
  static const command_line::arg_descriptor<bool> arg_dns_checkpoints  = {
    "enforce-dns-checkpointing"
  , "checkpoints from DNS server will be enforced"
  , false
  };
  static const command_line::arg_descriptor<uint64_t> arg_fast_block_sync = {
    "fast-block-sync"
  , "Sync up most of the way by using embedded, known block hashes."
  , 1
  };
  static const command_line::arg_descriptor<uint64_t> arg_prep_blocks_threads = {
    "prep-blocks-threads"
  , "Max number of threads to use when preparing block hashes in groups."
  , 4
  };
  static const command_line::arg_descriptor<uint64_t> arg_show_time_stats  = {
    "show-time-stats"
  , "Show time-stats when processing blocks/txs and disk synchronization."
  , 0
  };
  static const command_line::arg_descriptor<size_t> arg_block_sync_size  = {
    "block-sync-size"
  , "How many blocks to sync at once during chain synchronization (0 = adaptive)."
  , 0
  };
  static const command_line::arg_descriptor<std::string> arg_check_updates = {
    "check-updates"
  , "Check for new versions of antd: [disabled|notify|download|update]"
  , "notify"
  };
  static const command_line::arg_descriptor<bool> arg_fluffy_blocks  = {
    "fluffy-blocks"
  , "Relay blocks as fluffy blocks (obsolete, now default)"
  , true
  };
  static const command_line::arg_descriptor<bool> arg_no_fluffy_blocks  = {
    "no-fluffy-blocks"
  , "Relay blocks as normal blocks"
  , false
  };
  static const command_line::arg_descriptor<bool> arg_pad_transactions  = {
    "pad-transactions"
  , "Pad relayed transactions to help defend against traffic volume analysis"
  , false
  };
  static const command_line::arg_descriptor<size_t> arg_max_txpool_weight  = {
    "max-txpool-weight"
  , "Set maximum txpool weight in bytes."
  , DEFAULT_TXPOOL_MAX_WEIGHT
  };
  static const command_line::arg_descriptor<bool> arg_full_node  = {
    "fullnode"
  , "Run as a fullnode"
  };
  static const command_line::arg_descriptor<std::string> arg_block_notify = {
    "block-notify"
  , "Run a program for each new block, '%s' will be replaced by the block hash"
  , ""
  };
  static const command_line::arg_descriptor<bool> arg_prune_blockchain  = {
    "prune-blockchain"
  , "Prune blockchain"
  , false
  };
  static const command_line::arg_descriptor<std::string> arg_reorg_notify = {
    "reorg-notify"
  , "Run a program for each reorg, '%s' will be replaced by the split height, "
    "'%h' will be replaced by the new blockchain height, and '%n' will be "
    "replaced by the number of new blocks in the new chain"
  , ""
  };
  static const command_line::arg_descriptor<std::string> arg_block_rate_notify = {
    "block-rate-notify"
  , "Run a program when the block rate undergoes large fluctuations. This might "
    "be a sign of large amounts of hash rate going on and off the Monero network, "
    "and thus be of potential interest in predicting attacks. %t will be replaced "
    "by the number of minutes for the observation window, %b by the number of "
    "blocks observed within that window, and %e by the number of blocks that was "
    "expected in that window. It is suggested that this notification is used to "
    "automatically increase the number of confirmations required before a payment "
    "is acted upon."
  , ""
  };

  const command_line::arg_descriptor<uint64_t> arg_recalculate_difficulty = {
    "recalculate-difficulty",
    "Recalculate per-block difficulty starting from the height specified",
    0};

  //-----------------------------------------------------------------------------------------------
  core::core(i_cryptonote_protocol* pprotocol):
              m_mempool(m_blockchain_storage),
              m_full_node_list(m_blockchain_storage),
              m_blockchain_storage(m_mempool, m_full_node_list, m_deregister_vote_pool),
              m_quorum_cop(*this),
              m_miner(this, &m_blockchain_storage),
              m_miner_address(boost::value_initialized<account_public_address>()),
              m_starter_message_showed(false),
              m_target_blockchain_height(0),
              m_checkpoints_path(""),
              m_last_dns_checkpoints_update(0),
              m_last_json_checkpoints_update(0),
              m_disable_dns_checkpoints(false),
              m_update_download(0),
              m_nettype(UNDEFINED),
              m_update_available(false),
              m_pad_transactions(false)
  {
    m_checkpoints_updating.clear();
    set_cryptonote_protocol(pprotocol);
  }
  void core::set_cryptonote_protocol(i_cryptonote_protocol* pprotocol)
  {
    if(pprotocol)
      m_pprotocol = pprotocol;
    else
      m_pprotocol = &m_protocol_stub;
  }
  //-----------------------------------------------------------------------------------
  void core::set_checkpoints(checkpoints&& chk_pts)
  {
    m_blockchain_storage.set_checkpoints(std::move(chk_pts));
  }
  //-----------------------------------------------------------------------------------
  void core::set_checkpoints_file_path(const std::string& path)
  {
    m_checkpoints_path = path;
  }
  //-----------------------------------------------------------------------------------
  void core::set_enforce_dns_checkpoints(bool enforce_dns)
  {
    m_blockchain_storage.set_enforce_dns_checkpoints(enforce_dns);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::update_checkpoints()
  {
    if (m_nettype != MAINNET || m_disable_dns_checkpoints) return true;

    if (m_checkpoints_updating.test_and_set()) return true;

    bool res = true;
    if (time(NULL) - m_last_dns_checkpoints_update >= 3600)
    {
      res = m_blockchain_storage.update_checkpoints(m_checkpoints_path, true);
      m_last_dns_checkpoints_update = time(NULL);
      m_last_json_checkpoints_update = time(NULL);
    }
    else if (time(NULL) - m_last_json_checkpoints_update >= 600)
    {
      res = m_blockchain_storage.update_checkpoints(m_checkpoints_path, false);
      m_last_json_checkpoints_update = time(NULL);
    }

    m_checkpoints_updating.clear();

    // if anything fishy happened getting new checkpoints, bring down the house
    if (!res)
    {
      graceful_exit();
    }
    return res;
  }
  //-----------------------------------------------------------------------------------
  void core::stop()
  {
    m_miner.stop();
    m_blockchain_storage.cancel();

    tools::download_async_handle handle;
    {
      boost::lock_guard<boost::mutex> lock(m_update_mutex);
      handle = m_update_download;
      m_update_download = 0;
    }
    if (handle)
      tools::download_cancel(handle);
  }
  //-----------------------------------------------------------------------------------
  void core::init_options(boost::program_options::options_description& desc)
  {
    command_line::add_arg(desc, arg_data_dir);

    command_line::add_arg(desc, arg_test_drop_download);
    command_line::add_arg(desc, arg_test_drop_download_height);

    command_line::add_arg(desc, arg_testnet_on);
    command_line::add_arg(desc, arg_stagenet_on);
    command_line::add_arg(desc, arg_regtest_on);
    command_line::add_arg(desc, arg_fixed_difficulty);
    command_line::add_arg(desc, arg_dns_checkpoints);
    command_line::add_arg(desc, arg_prep_blocks_threads);
    command_line::add_arg(desc, arg_fast_block_sync);
    command_line::add_arg(desc, arg_show_time_stats);
    command_line::add_arg(desc, arg_block_sync_size);
    command_line::add_arg(desc, arg_check_updates);
    command_line::add_arg(desc, arg_fluffy_blocks);
    command_line::add_arg(desc, arg_no_fluffy_blocks);
    command_line::add_arg(desc, arg_test_dbg_lock_sleep);
    command_line::add_arg(desc, arg_offline);
    command_line::add_arg(desc, arg_disable_dns_checkpoints);
    command_line::add_arg(desc, arg_block_download_max_size);
    command_line::add_arg(desc, arg_max_txpool_weight);
    command_line::add_arg(desc, arg_full_node);
    command_line::add_arg(desc, arg_pad_transactions);
    command_line::add_arg(desc, arg_block_notify);
    command_line::add_arg(desc, arg_prune_blockchain);
    command_line::add_arg(desc, arg_reorg_notify);
    command_line::add_arg(desc, arg_block_rate_notify);

    command_line::add_arg(desc, arg_recalculate_difficulty);
#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    command_line::add_arg(desc, antd::arg_integration_test_hardforks_override);
    command_line::add_arg(desc, antd::arg_integration_test_shared_mem_name);
#endif

    miner::init_options(desc);
    BlockchainDB::init_options(desc);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_command_line(const boost::program_options::variables_map& vm)
  {
    if (m_nettype != FAKECHAIN)
    {
      const bool testnet = command_line::get_arg(vm, arg_testnet_on);
      const bool stagenet = command_line::get_arg(vm, arg_stagenet_on);
      m_nettype = testnet ? TESTNET : stagenet ? STAGENET : MAINNET;
      m_deregister_vote_pool.m_nettype = m_nettype;
    }

    m_config_folder = command_line::get_arg(vm, arg_data_dir);

    auto data_dir = boost::filesystem::path(m_config_folder);

    if (m_nettype == MAINNET)
    {
      cryptonote::checkpoints checkpoints;
      if (!checkpoints.init_default_checkpoints(m_nettype))
      {
        throw std::runtime_error("Failed to initialize checkpoints");
      }
      set_checkpoints(std::move(checkpoints));

      boost::filesystem::path json(JSON_HASH_FILE_NAME);
      boost::filesystem::path checkpoint_json_hashfile_fullpath = data_dir / json;

      set_checkpoints_file_path(checkpoint_json_hashfile_fullpath.string());
    }


    set_enforce_dns_checkpoints(command_line::get_arg(vm, arg_dns_checkpoints));
    test_drop_download_height(command_line::get_arg(vm, arg_test_drop_download_height));
    m_fluffy_blocks_enabled = !get_arg(vm, arg_no_fluffy_blocks);
    m_pad_transactions = get_arg(vm, arg_pad_transactions);
    m_offline = get_arg(vm, arg_offline);
    m_disable_dns_checkpoints = get_arg(vm, arg_disable_dns_checkpoints);
    if (!command_line::is_arg_defaulted(vm, arg_fluffy_blocks))
      MWARNING(arg_fluffy_blocks.name << " is obsolete, it is now default");

    if (command_line::get_arg(vm, arg_test_drop_download) == true)
      test_drop_download();

    m_full_node = command_line::get_arg(vm, arg_full_node);

    epee::debug::g_test_dbg_lock_sleep() = command_line::get_arg(vm, arg_test_dbg_lock_sleep);

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_current_blockchain_height() const
  {
    return m_blockchain_storage.get_current_blockchain_height();
  }
  //-----------------------------------------------------------------------------------------------
  void core::get_blockchain_top(uint64_t& height, crypto::hash& top_id) const
  {
    top_id = m_blockchain_storage.get_tail_id(height);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_blocks(uint64_t start_offset, size_t count, std::vector<std::pair<cryptonote::blobdata,block>>& blocks, std::vector<cryptonote::blobdata>& txs) const
  {
    return m_blockchain_storage.get_blocks(start_offset, count, blocks, txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_blocks(uint64_t start_offset, size_t count, std::vector<std::pair<cryptonote::blobdata,block>>& blocks) const
  {
    return m_blockchain_storage.get_blocks(start_offset, count, blocks);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_blocks(uint64_t start_offset, size_t count, std::vector<block>& blocks) const
  {
    std::vector<std::pair<cryptonote::blobdata, cryptonote::block>> bs;
    if (!m_blockchain_storage.get_blocks(start_offset, count, bs))
      return false;
    for (const auto &b: bs)
      blocks.push_back(b.second);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_transactions(const std::vector<crypto::hash>& txs_ids, std::vector<cryptonote::blobdata>& txs, std::vector<crypto::hash>& missed_txs) const
  {
    return m_blockchain_storage.get_transactions_blobs(txs_ids, txs, missed_txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_split_transactions_blobs(const std::vector<crypto::hash>& txs_ids, std::vector<std::tuple<crypto::hash, cryptonote::blobdata, crypto::hash, cryptonote::blobdata>>& txs, std::vector<crypto::hash>& missed_txs) const
  {
    return m_blockchain_storage.get_split_transactions_blobs(txs_ids, txs, missed_txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_txpool_backlog(std::vector<tx_backlog_entry>& backlog) const
  {
    m_mempool.get_transaction_backlog(backlog);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_transactions(const std::vector<crypto::hash>& txs_ids, std::vector<transaction>& txs, std::vector<crypto::hash>& missed_txs) const
  {
    return m_blockchain_storage.get_transactions(txs_ids, txs, missed_txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_alternative_blocks(std::vector<block>& blocks) const
  {
    return m_blockchain_storage.get_alternative_blocks(blocks);
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_alternative_blocks_count() const
  {
    return m_blockchain_storage.get_alternative_blocks_count();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::init(const boost::program_options::variables_map& vm, const cryptonote::test_options *test_options, const GetCheckpointsCallback& get_checkpoints/* = nullptr */)
  {
    start_time = std::time(nullptr);

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    const std::string arg_integration_test_override_hardforks = command_line::get_arg(vm, antd::arg_integration_test_hardforks_override);

    std::vector<std::pair<uint8_t, uint64_t>> integration_test_hardforks;
    if (!arg_integration_test_override_hardforks.empty())
    {
      // Expected format: <fork_version>:<fork_height>, ...
      // Example: 7:0, 8:10, 9:20, 10:100
      char const *ptr = arg_integration_test_override_hardforks.c_str();
      while (ptr[0])
      {
        int hf_version = atoi(ptr);
        while(ptr[0] != ':') ptr++;
        ++ptr;

        int hf_height = atoi(ptr);
        while(ptr[0] && ptr[0] != ',') ptr++;
        integration_test_hardforks.push_back(std::make_pair(static_cast<uint8_t>(hf_version), static_cast<uint64_t>(hf_height)));

        if (!ptr[0]) break;
        ptr++;
      }
    }

    cryptonote::test_options integration_hardfork_override = {integration_test_hardforks};
    if (!arg_integration_test_override_hardforks.empty())
      test_options = &integration_hardfork_override;

    {
      const std::string arg_shared_mem_name = command_line::get_arg(vm, antd::arg_integration_test_shared_mem_name);
      antd::init_integration_test_context(arg_shared_mem_name);
    }
#endif

    const bool regtest = command_line::get_arg(vm, arg_regtest_on);
    if (test_options != NULL || regtest)
    {
      m_nettype = FAKECHAIN;
    }

    bool r = handle_command_line(vm);

    std::string db_type = command_line::get_arg(vm, cryptonote::arg_db_type);
    std::string db_sync_mode = command_line::get_arg(vm, cryptonote::arg_db_sync_mode);
    bool db_salvage = command_line::get_arg(vm, cryptonote::arg_db_salvage) != 0;
    bool fast_sync = command_line::get_arg(vm, arg_fast_block_sync) != 0;
    uint64_t blocks_threads = command_line::get_arg(vm, arg_prep_blocks_threads);
    std::string check_updates_string = command_line::get_arg(vm, arg_check_updates);
    size_t max_txpool_weight = command_line::get_arg(vm, arg_max_txpool_weight);
    bool prune_blockchain = command_line::get_arg(vm, arg_prune_blockchain);

    if (m_full_node)
    {
      r = init_full_node_key();
      CHECK_AND_ASSERT_MES(r, false, "Failed to create or load fullnode key");
      m_full_node_list.set_my_full_node_keys(&m_full_node_pubkey);
    }

    boost::filesystem::path folder(m_config_folder);
    if (m_nettype == FAKECHAIN)
      folder /= "fake";

    // make sure the data directory exists, and try to lock it
    CHECK_AND_ASSERT_MES (boost::filesystem::exists(folder) || boost::filesystem::create_directories(folder), false,
      std::string("Failed to create directory ").append(folder.string()).c_str());

    // check for blockchain.bin
    try
    {
      const boost::filesystem::path old_files = folder;
      if (boost::filesystem::exists(old_files / "blockchain.bin"))
      {
        MWARNING("Found old-style blockchain.bin in " << old_files.string());
        MWARNING("Antd now uses a new format. You can either remove blockchain.bin to start syncing");
        MWARNING("the blockchain anew, or use antd-blockchain-export and antd-blockchain-import to");
        MWARNING("convert your existing blockchain.bin to the new format. See README.md for instructions.");
        return false;
      }
    }
    // folder might not be a directory, etc, etc
    catch (...) { }

    std::unique_ptr<BlockchainDB> db(new_db(db_type));
    if (db == NULL)
    {
      LOG_ERROR("Attempted to use non-existent database type");
      return false;
    }

    folder /= db->get_db_name();
    MGINFO("Loading blockchain from folder " << folder.string() << " ...");

    const std::string filename = folder.string();
    // default to fast:async:1 if overridden
    blockchain_db_sync_mode sync_mode = db_defaultsync;
    bool sync_on_blocks = true;
    uint64_t sync_threshold = 1;

    if (m_nettype == FAKECHAIN)
    {
#if !defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS) // In integration mode, don't delete the DB. This should be explicitly done in the tests. Otherwise the more likely behaviour is persisting the DB across multiple daemons in the same test.
      // reset the db by removing the database file before opening it
      if (!db->remove_data_file(filename))
      {
        MERROR("Failed to remove data file in " << filename);
        return false;
      }
#endif
    }

    try
    {
      uint64_t db_flags = 0;

      std::vector<std::string> options;
      boost::trim(db_sync_mode);
      boost::split(options, db_sync_mode, boost::is_any_of(" :"));
      const bool db_sync_mode_is_default = command_line::is_arg_defaulted(vm, cryptonote::arg_db_sync_mode);

      for(const auto &option : options)
        MDEBUG("option: " << option);

      // default to fast:async:1
      uint64_t DEFAULT_FLAGS = DBF_FAST;

      if(options.size() == 0)
      {
        // default to fast:async:1
        db_flags = DEFAULT_FLAGS;
      }

      bool safemode = false;
      if(options.size() >= 1)
      {
        if(options[0] == "safe")
        {
          safemode = true;
          db_flags = DBF_SAFE;
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_nosync;
        }
        else if(options[0] == "fast")
        {
          db_flags = DBF_FAST;
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_async;
        }
        else if(options[0] == "fastest")
        {
          db_flags = DBF_FASTEST;
          sync_threshold = 1000; // default to fastest:async:1000
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_async;
        }
        else
          db_flags = DEFAULT_FLAGS;
      }

      if(options.size() >= 2 && !safemode)
      {
        if(options[1] == "sync")
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_sync;
        else if(options[1] == "async")
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_async;
      }

      if(options.size() >= 3 && !safemode)
      {
        char *endptr;
        uint64_t threshold = strtoull(options[2].c_str(), &endptr, 0);
        if (*endptr == '\0' || !strcmp(endptr, "blocks"))
        {
          sync_on_blocks = true;
          sync_threshold = threshold;
        }
        else if (!strcmp(endptr, "bytes"))
        {
          sync_on_blocks = false;
          sync_threshold = threshold;
        }
        else
        {
          LOG_ERROR("Invalid db sync mode: " << options[2]);
          return false;
        }
      }

      if (db_salvage)
        db_flags |= DBF_SALVAGE;

      db->open(filename, db_flags);
      if(!db->m_open)
        return false;
    }
    catch (const DB_ERROR& e)
    {
      LOG_ERROR("Error opening database: " << e.what());
      return false;
    }

    m_blockchain_storage.set_user_options(blocks_threads,
        sync_on_blocks, sync_threshold, sync_mode, fast_sync);

    try
    {
      if (!command_line::is_arg_defaulted(vm, arg_block_notify))
        m_blockchain_storage.set_block_notify(std::shared_ptr<tools::Notify>(new tools::Notify(command_line::get_arg(vm, arg_block_notify).c_str())));
    }
    catch (const std::exception &e)
    {
      MERROR("Failed to parse block notify spec");
    }

    try
    {
      if (!command_line::is_arg_defaulted(vm, arg_reorg_notify))
        m_blockchain_storage.set_reorg_notify(std::shared_ptr<tools::Notify>(new tools::Notify(command_line::get_arg(vm, arg_reorg_notify).c_str())));
    }
    catch (const std::exception &e)
    {
      MERROR("Failed to parse reorg notify spec");
    }

    try
    {
      if (!command_line::is_arg_defaulted(vm, arg_block_rate_notify))
        m_block_rate_notify.reset(new tools::Notify(command_line::get_arg(vm, arg_block_rate_notify).c_str()));
    }
    catch (const std::exception &e)
    {
      MERROR("Failed to parse block rate notify spec");
    }

    const std::pair<uint8_t, uint64_t> regtest_hard_forks[3] = {
  std::make_pair(1, 0),
  std::make_pair(mainnet_hard_forks[num_mainnet_hard_forks - 1].version, 1),
  std::make_pair(0, 0)
   };

    const difficulty_type fixed_difficulty = command_line::get_arg(vm, arg_fixed_difficulty);

    BlockchainDB *initialized_db = db.release();
    m_full_node_list.set_db_pointer(initialized_db);
    m_full_node_list.register_hooks(m_quorum_cop);
    r = m_blockchain_storage.init(initialized_db, m_nettype, m_offline, test_options, fixed_difficulty, get_checkpoints);

    if (!command_line::is_arg_defaulted(vm, arg_recalculate_difficulty))
    {
      uint64_t recalc_diff_from_block = command_line::get_arg(vm, arg_recalculate_difficulty);
      cryptonote::BlockchainDB::fixup_context context  = {};
      context.type                                     = cryptonote::BlockchainDB::fixup_type::calculate_difficulty;
      context.calculate_difficulty_params.start_height = recalc_diff_from_block;
      initialized_db->fixup(context);
    }

    r = m_mempool.init(max_txpool_weight);
    CHECK_AND_ASSERT_MES(r, false, "Failed to initialize memory pool");

    // now that we have a valid m_blockchain_storage, we can clean out any
    // transactions in the pool that do not conform to the current fork
    m_mempool.validate(m_blockchain_storage.get_current_hard_fork_version());

    bool show_time_stats = command_line::get_arg(vm, arg_show_time_stats) != 0;
    m_blockchain_storage.set_show_time_stats(show_time_stats);
    CHECK_AND_ASSERT_MES(r, false, "Failed to initialize blockchain storage");

    block_sync_size = command_line::get_arg(vm, arg_block_sync_size);
    if (block_sync_size > BLOCKS_SYNCHRONIZING_MAX_COUNT)
      MERROR("Error --block-sync-size cannot be greater than " << BLOCKS_SYNCHRONIZING_MAX_COUNT);

    MGINFO("Loading checkpoints");

    // load json & DNS checkpoints, and verify them
    // with respect to what blocks we already have
    CHECK_AND_ASSERT_MES(update_checkpoints(), false, "One or more checkpoints loaded from json or dns conflicted with existing checkpoints.");

   // DNS versions checking
    if (check_updates_string == "disabled")
      check_updates_level = UPDATES_DISABLED;
    else if (check_updates_string == "notify")
      check_updates_level = UPDATES_NOTIFY;
    else if (check_updates_string == "download")
      check_updates_level = UPDATES_DOWNLOAD;
    else if (check_updates_string == "update")
      check_updates_level = UPDATES_UPDATE;
    else {
      MERROR("Invalid argument to --dns-versions-check: " << check_updates_string);
      return false;
    }

    r = m_miner.init(vm, m_nettype);
    CHECK_AND_ASSERT_MES(r, false, "Failed to initialize miner instance");

    if (prune_blockchain)
    {
      // display a message if the blockchain is not pruned yet
      if (m_blockchain_storage.get_current_blockchain_height() > 1 && !m_blockchain_storage.get_blockchain_pruning_seed())
        MGINFO("Pruning blockchain...");
      CHECK_AND_ASSERT_MES(m_blockchain_storage.prune_blockchain(), false, "Failed to prune blockchain");
    }

    return load_state_data();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::init_full_node_key()
  {
    std::string keypath = m_config_folder + "/key";
    if (epee::file_io_utils::is_file_exist(keypath))
    {
      std::string keystr;
      bool r = epee::file_io_utils::load_file_to_string(keypath, keystr);
      memcpy(&unwrap(unwrap(m_full_node_key)), keystr.data(), sizeof(m_full_node_key));
      wipeable_string wipe(keystr);
      CHECK_AND_ASSERT_MES(r, false, "failed to load fullnode key from file");

      r = crypto::secret_key_to_public_key(m_full_node_key, m_full_node_pubkey);
      CHECK_AND_ASSERT_MES(r, false, "failed to generate pubkey from secret key");
    }
    else
    {
      cryptonote::keypair keypair = keypair::generate(hw::get_device("default"));
      m_full_node_pubkey = keypair.pub;
      m_full_node_key = keypair.sec;

      std::string keystr(reinterpret_cast<const char *>(&m_full_node_key), sizeof(m_full_node_key));
      bool r = epee::file_io_utils::save_string_to_file(keypath, keystr);
      wipeable_string wipe(keystr);
      CHECK_AND_ASSERT_MES(r, false, "failed to save fullnode key to file");

      using namespace boost::filesystem;
      permissions(keypath, owner_read);
    }

    MGINFO_YELLOW("Fullnode pubkey is " << epee::string_tools::pod_to_hex(m_full_node_pubkey));

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::set_genesis_block(const block& b)
  {
    return m_blockchain_storage.reset_and_set_genesis_block(b);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::load_state_data()
  {
    // may be some code later
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::deinit()
  {
    m_full_node_list.store();
    m_full_node_list.set_db_pointer(nullptr);
    m_miner.stop();
    m_mempool.deinit();
    m_blockchain_storage.deinit();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  void core::test_drop_download()
  {
    m_test_drop_download = false;
  }
  //-----------------------------------------------------------------------------------------------
  void core::test_drop_download_height(uint64_t height)
  {
    m_test_drop_download_height = height;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_test_drop_download() const
  {
    return m_test_drop_download;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_test_drop_download_height() const
  {
    if (m_test_drop_download_height == 0)
      return true;

    if (get_blockchain_storage().get_current_blockchain_height() <= m_test_drop_download_height)
      return true;

    return false;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx_pre(const blobdata& tx_blob, tx_verification_context& tvc, cryptonote::transaction &tx, crypto::hash &tx_hash, crypto::hash &tx_prefixt_hash, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    tvc = boost::value_initialized<tx_verification_context>();

    if(tx_blob.size() > get_max_tx_size())
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, too big size " << tx_blob.size() << ", rejected");
      tvc.m_verifivation_failed = true;
      tvc.m_too_big = true;
      return false;
    }

    tx_hash = crypto::null_hash;
    tx_prefixt_hash = crypto::null_hash;

    if(!parse_tx_from_blob(tx, tx_hash, tx_prefixt_hash, tx_blob))
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, Failed to parse, rejected");
      tvc.m_verifivation_failed = true;
      return false;
    }
    //std::cout << "!"<< tx.vin.size() << std::endl;

    bad_semantics_txes_lock.lock();
    for (int idx = 0; idx < 2; ++idx)
    {
      if (bad_semantics_txes[idx].find(tx_hash) != bad_semantics_txes[idx].end())
      {
        bad_semantics_txes_lock.unlock();
        LOG_PRINT_L1("Transaction already seen with bad semantics, rejected");
        tvc.m_verifivation_failed = true;
        return false;
      }
    }
    bad_semantics_txes_lock.unlock();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx_post(const blobdata& tx_blob, tx_verification_context& tvc, cryptonote::transaction &tx, crypto::hash &tx_hash, crypto::hash &tx_prefixt_hash, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    if(!check_tx_syntax(tx))
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, Failed to check tx " << tx_hash << " syntax, rejected");
      tvc.m_verifivation_failed = true;
      return false;
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  void core::set_semantics_failed(const crypto::hash &tx_hash)
  {
    LOG_PRINT_L1("WRONG TRANSACTION BLOB, Failed to check tx " << tx_hash << " semantic, rejected");
    bad_semantics_txes_lock.lock();
    bad_semantics_txes[0].insert(tx_hash);
    if (bad_semantics_txes[0].size() >= BAD_SEMANTICS_TXES_MAX_SIZE)
    {
      std::swap(bad_semantics_txes[0], bad_semantics_txes[1]);
      bad_semantics_txes[0].clear();
    }
    bad_semantics_txes_lock.unlock();
  }
  //-----------------------------------------------------------------------------------------------
  static bool is_canonical_bulletproof_layout(const std::vector<rct::Bulletproof> &proofs)
  {
    if (proofs.size() != 1)
      return false;
    const size_t sz = proofs[0].V.size();
    if (sz == 0 || sz > BULLETPROOF_MAX_OUTPUTS)
      return false;
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx_accumulated_batch(std::vector<tx_verification_batch_info> &tx_info, bool keeped_by_block)
  {
    bool ret = true;
    if (keeped_by_block && get_blockchain_storage().is_within_compiled_block_hash_area())
    {
      MTRACE("Skipping semantics check for tx kept by block in embedded hash area");
      return true;
    }

    std::vector<const rct::rctSig*> rvv;
    for (size_t n = 0; n < tx_info.size(); ++n)
    {
      if (!check_tx_semantic(*tx_info[n].tx, keeped_by_block))
      {
        set_semantics_failed(tx_info[n].tx_hash);
        tx_info[n].tvc.m_verifivation_failed = true;
        tx_info[n].result = false;
        continue;
      }

      if (tx_info[n].tx->get_type() != transaction::type_standard)
        continue;
      const rct::rctSig &rv = tx_info[n].tx->rct_signatures;
      switch (rv.type) {
        case rct::RCTTypeNull:
          // coinbase should not come here, so we reject for all other types
          MERROR_VER("Unexpected Null rctSig type");
          set_semantics_failed(tx_info[n].tx_hash);
          tx_info[n].tvc.m_verifivation_failed = true;
          tx_info[n].result = false;
          break;
        case rct::RCTTypeSimple:
          if (!rct::verRctSemanticsSimple(rv))
          {
            MERROR_VER("rct signature semantics check failed");
            set_semantics_failed(tx_info[n].tx_hash);
            tx_info[n].tvc.m_verifivation_failed = true;
            tx_info[n].result = false;
            break;
          }
          break;
        case rct::RCTTypeFull:
          if (!rct::verRct(rv, true))
          {
            MERROR_VER("rct signature semantics check failed");
            set_semantics_failed(tx_info[n].tx_hash);
            tx_info[n].tvc.m_verifivation_failed = true;
            tx_info[n].result = false;
            break;
          }
          break;
        case rct::RCTTypeBulletproof:
        case rct::RCTTypeBulletproof2:
        case rct::RCTTypeCLSAG:
          if (!is_canonical_bulletproof_layout(rv.p.bulletproofs))
          {
            MERROR_VER("Bulletproof does not have canonical form");
            set_semantics_failed(tx_info[n].tx_hash);
            tx_info[n].tvc.m_verifivation_failed = true;
            tx_info[n].result = false;
            break;
          }
          rvv.push_back(&rv); // delayed batch verification
          break;
        default:
          MERROR_VER("Unknown rct type: " << rv.type);
          set_semantics_failed(tx_info[n].tx_hash);
          tx_info[n].tvc.m_verifivation_failed = true;
          tx_info[n].result = false;
          break;
      }
    }
    if (!rvv.empty() && !rct::verRctSemanticsSimple(rvv))
    {
      LOG_PRINT_L1("One transaction among this group has bad semantics, verifying one at a time");
      ret = false;
      const bool assumed_bad = rvv.size() == 1; // if there's only one tx, it must be the bad one
      for (size_t n = 0; n < tx_info.size(); ++n)
      {
        if (!tx_info[n].result)
          continue;
        if (tx_info[n].tx->rct_signatures.type != rct::RCTTypeBulletproof && tx_info[n].tx->rct_signatures.type != rct::RCTTypeBulletproof2 && tx_info[n].tx->rct_signatures.type != rct::RCTTypeCLSAG)
          continue;
        if (assumed_bad || !rct::verRctSemanticsSimple(tx_info[n].tx->rct_signatures))
        {
          set_semantics_failed(tx_info[n].tx_hash);
          tx_info[n].tvc.m_verifivation_failed = true;
          tx_info[n].result = false;
        }
      }
    }

    return ret;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_txs(const std::vector<blobdata>& tx_blobs, std::vector<tx_verification_context>& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    TRY_ENTRY();
    CRITICAL_REGION_LOCAL(m_incoming_tx_lock);

    struct result { bool res; cryptonote::transaction tx; crypto::hash hash; crypto::hash prefix_hash; };
    std::vector<result> results(tx_blobs.size());

    tvc.resize(tx_blobs.size());
    tools::threadpool& tpool = tools::threadpool::getInstance();
    tools::threadpool::waiter waiter;
    std::vector<blobdata>::const_iterator it = tx_blobs.begin();
    for (size_t i = 0; i < tx_blobs.size(); i++, ++it) {
      tpool.submit(&waiter, [&, i, it] {
        try
        {
          results[i].res = handle_incoming_tx_pre(*it, tvc[i], results[i].tx, results[i].hash, results[i].prefix_hash, keeped_by_block, relayed, do_not_relay);
        }
        catch (const std::exception &e)
        {
          MERROR_VER("Exception in handle_incoming_tx_pre: " << e.what());
          results[i].res = false;
        }
      });
    }
    waiter.wait(&tpool);
    it = tx_blobs.begin();
    std::vector<bool> already_have(tx_blobs.size(), false);
    for (size_t i = 0; i < tx_blobs.size(); i++, ++it) {
      if (!results[i].res)
        continue;
      if(m_mempool.have_tx(results[i].hash))
      {
        LOG_PRINT_L2("tx " << results[i].hash << "already have transaction in tx_pool");
        already_have[i] = true;
      }
      else if(m_blockchain_storage.have_tx(results[i].hash))
      {
        LOG_PRINT_L2("tx " << results[i].hash << " already have transaction in blockchain");
        already_have[i] = true;
      }
      else
      {
        tpool.submit(&waiter, [&, i, it] {
          try
          {
            results[i].res = handle_incoming_tx_post(*it, tvc[i], results[i].tx, results[i].hash, results[i].prefix_hash, keeped_by_block, relayed, do_not_relay);
          }
          catch (const std::exception &e)
          {
            MERROR_VER("Exception in handle_incoming_tx_post: " << e.what());
            results[i].res = false;
          }
        });
      }
    }
    waiter.wait(&tpool);

    std::vector<tx_verification_batch_info> tx_info;
    tx_info.reserve(tx_blobs.size());
    for (size_t i = 0; i < tx_blobs.size(); i++) {
      if (!results[i].res || already_have[i])
        continue;
      tx_info.push_back({&results[i].tx, results[i].hash, tvc[i], results[i].res});
    }
    if (!tx_info.empty())
      handle_incoming_tx_accumulated_batch(tx_info, keeped_by_block);

    bool ok = true;
    it = tx_blobs.begin();
    for (size_t i = 0; i < tx_blobs.size(); i++, ++it) {
      if (!results[i].res)
      {
        ok = false;
        continue;
      }
      if (keeped_by_block)
        get_blockchain_storage().on_new_tx_from_block(results[i].tx);
      if (already_have[i])
        continue;

      const size_t weight = get_transaction_weight(results[i].tx, it->size());
      ok &= add_new_tx(results[i].tx, results[i].hash, tx_blobs[i], results[i].prefix_hash, weight, tvc[i], keeped_by_block, relayed, do_not_relay);
      if(tvc[i].m_verifivation_failed)
      {
        MERROR_VER("Transaction verification failed: " << results[i].hash);
      }
      else if(tvc[i].m_verifivation_impossible)
      {
        MERROR_VER("Transaction verification impossible: " << results[i].hash);
      }

      if(tvc[i].m_added_to_pool)
        MDEBUG("tx added: " << results[i].hash);
    }
    return ok;

    CATCH_ENTRY_L0("core::handle_incoming_txs()", false);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx(const blobdata& tx_blob, tx_verification_context& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    std::vector<cryptonote::blobdata> tx_blobs;
    tx_blobs.push_back(tx_blob);
    std::vector<tx_verification_context> tvcv(1);
    bool r = handle_incoming_txs(tx_blobs, tvcv, keeped_by_block, relayed, do_not_relay);
    tvc = tvcv[0];
    return r;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_stat_info(core_stat_info& st_inf) const
  {
    st_inf.mining_speed = m_miner.get_speed();
    st_inf.alternative_blocks = m_blockchain_storage.get_alternative_blocks_count();
    st_inf.blockchain_height = m_blockchain_storage.get_current_blockchain_height();
    st_inf.tx_pool_size = m_mempool.get_transactions_count();
    st_inf.top_block_id_str = epee::string_tools::pod_to_hex(m_blockchain_storage.get_tail_id());
    return true;
  }

  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_semantic(const transaction& tx, bool keeped_by_block) const
  {
    if (tx.get_type() != transaction::type_standard)
    {
      if (tx.vin.size() != 0)
      {
        MERROR_VER("tx type: " << transaction::type_to_string(tx.type) << " must have 0 inputs, received: " << tx.vin.size() << ", rejected for tx id = " << get_transaction_hash(tx));
        return false;
      }
    }
    else if (!tx.vin.size())
    {
      MERROR_VER("tx with empty inputs, rejected for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if(!check_inputs_types_supported(tx))
    {
      MERROR_VER("unsupported input types for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if(!check_outs_valid(tx))
    {
      MERROR_VER("tx with invalid outputs, rejected for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if (tx.version >= transaction::version_2)
    {
      if (tx.rct_signatures.outPk.size() != tx.vout.size())
      {
        MERROR_VER("tx with mismatched vout/outPk count, rejected for tx id= " << get_transaction_hash(tx));
        return false;
      }
    }

    if(!check_money_overflow(tx))
    {
      MERROR_VER("tx has money overflow, rejected for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if (tx.version == transaction::version_1)
    {
      uint64_t amount_in = 0;
      get_inputs_money_amount(tx, amount_in);
      uint64_t amount_out = get_outs_money_amount(tx);

      if(amount_in <= amount_out)
      {
        MERROR_VER("tx with wrong amounts: ins " << amount_in << ", outs " << amount_out << ", rejected for tx id= " << get_transaction_hash(tx));
        return false;
      }
    }

    if(!keeped_by_block && get_transaction_weight(tx) >= m_blockchain_storage.get_current_cumulative_block_weight_limit() - CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE)
    {
      MERROR_VER("tx is too large " << get_transaction_weight(tx) << ", expected not bigger than " << m_blockchain_storage.get_current_cumulative_block_weight_limit() - CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE);
      return false;
    }

    if(!check_tx_inputs_keyimages_diff(tx))
    {
      MERROR_VER("tx uses a single key image more than once");
      return false;
    }

    if (!check_tx_inputs_ring_members_diff(tx))
    {
      MERROR_VER("tx uses duplicate ring members");
      return false;
    }

    if (!check_tx_inputs_keyimages_domain(tx))
    {
      MERROR_VER("tx uses key image not in the valid domain");
      return false;
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::is_key_image_spent(const crypto::key_image &key_image) const
  {
    return m_blockchain_storage.have_tx_keyimg_as_spent(key_image);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::are_key_images_spent(const std::vector<crypto::key_image>& key_im, std::vector<bool> &spent) const
  {
    spent.clear();
    for(auto& ki: key_im)
    {
      spent.push_back(m_blockchain_storage.have_tx_keyimg_as_spent(ki));
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_block_sync_size(uint64_t height) const
  {
    static const uint64_t quick_height = m_nettype == TESTNET ? 801219 : m_nettype == MAINNET ? 1220516 : 0;
    if (block_sync_size > 0)
      return block_sync_size;
    if (height >= quick_height)
      return BLOCKS_SYNCHRONIZING_DEFAULT_COUNT;
    return BLOCKS_SYNCHRONIZING_DEFAULT_COUNT_PRE_V4;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::are_key_images_spent_in_pool(const std::vector<crypto::key_image>& key_im, std::vector<bool> &spent) const
  {
    spent.clear();

    return m_mempool.check_for_key_images(key_im, spent);
  }
  //-----------------------------------------------------------------------------------------------
  std::pair<uint64_t, uint64_t> core::get_coinbase_tx_sum(const uint64_t start_offset, const size_t count)
  {
    uint64_t emission_amount = 0;
    uint64_t total_fee_amount = 0;
    if (count)
    {
      const uint64_t end = start_offset + count - 1;
      m_blockchain_storage.for_blocks_range(start_offset, end,
        [this, &emission_amount, &total_fee_amount](uint64_t, const crypto::hash& hash, const block& b){
      std::vector<transaction> txs;
      std::vector<crypto::hash> missed_txs;
      uint64_t coinbase_amount = get_outs_money_amount(b.miner_tx);
      this->get_transactions(b.tx_hashes, txs, missed_txs);      
      uint64_t tx_fee_amount = 0;
      for(const auto& tx: txs)
      {
        tx_fee_amount += get_tx_fee(tx);
      }
      
      emission_amount += coinbase_amount - tx_fee_amount;
      total_fee_amount += tx_fee_amount;
      return true;
      });
    }

    return std::pair<uint64_t, uint64_t>(emission_amount, total_fee_amount);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_inputs_keyimages_diff(const transaction& tx) const
  {
    std::unordered_set<crypto::key_image> ki;
    for(const auto& in: tx.vin)
    {
      CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, tokey_in, false);
      if(!ki.insert(tokey_in.k_image).second)
        return false;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_inputs_ring_members_diff(const transaction& tx) const
  {
    const uint8_t version = m_blockchain_storage.get_current_hard_fork_version();
    if (version >= 6)
    {
      for(const auto& in: tx.vin)
      {
        CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, tokey_in, false);
        for (size_t n = 1; n < tokey_in.key_offsets.size(); ++n)
          if (tokey_in.key_offsets[n] == 0)
            return false;
      }
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_inputs_keyimages_domain(const transaction& tx) const
  {
    std::unordered_set<crypto::key_image> ki;
    for(const auto& in: tx.vin)
    {
      CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, tokey_in, false);
      if (!(rct::scalarmultKey(rct::ki2rct(tokey_in.k_image), rct::curveOrder()) == rct::identity()))
        return false;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_new_tx(transaction& tx, tx_verification_context& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    crypto::hash tx_hash = get_transaction_hash(tx);
    crypto::hash tx_prefix_hash = get_transaction_prefix_hash(tx);
    blobdata bl;
    t_serializable_object_to_blob(tx, bl);
    size_t tx_weight = get_transaction_weight(tx, bl.size());
    return add_new_tx(tx, tx_hash, bl, tx_prefix_hash, tx_weight, tvc, keeped_by_block, relayed, do_not_relay);
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_blockchain_total_transactions() const
  {
    return m_blockchain_storage.get_total_transactions();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_new_tx(transaction& tx, const crypto::hash& tx_hash, const cryptonote::blobdata &blob, const crypto::hash& tx_prefix_hash, size_t tx_weight, tx_verification_context& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    if(m_mempool.have_tx(tx_hash))
    {
      LOG_PRINT_L2("tx " << tx_hash << "already have transaction in tx_pool");
      return true;
    }

    if(m_blockchain_storage.have_tx(tx_hash))
    {
      LOG_PRINT_L2("tx " << tx_hash << " already have transaction in blockchain");
      return true;
    }

    uint8_t version = m_blockchain_storage.get_current_hard_fork_version();
    return m_mempool.add_tx(tx, tx_hash, blob, tx_weight, tvc, keeped_by_block, relayed, do_not_relay, version);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::relay_txpool_transactions()
  {
    // we attempt to relay txes that should be relayed, but were not
    std::vector<std::pair<crypto::hash, cryptonote::blobdata>> txs;
    if (m_mempool.get_relayable_transactions(txs) && !txs.empty())
    {
      cryptonote_connection_context fake_context {};
      tx_verification_context tvc {};
      NOTIFY_NEW_TRANSACTIONS::request r;
      for (auto it = txs.begin(); it != txs.end(); ++it)
      {
        r.txs.push_back(it->second);
      }
      get_protocol()->relay_transactions(r, fake_context);
      m_mempool.set_relayed(txs);
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::submit_uptime_proof()
  {
    if (m_full_node)
    {
      cryptonote_connection_context fake_context {};
      NOTIFY_UPTIME_PROOF::request r;
      m_quorum_cop.generate_uptime_proof_request(r);
      bool relayed = get_protocol()->relay_uptime_proof(r, fake_context);

      if (relayed)
        MGINFO("Submitted uptime-proof for fullnode (yours): " << m_full_node_pubkey);
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_uptime_proof(const crypto::public_key &key) const
  {
    uint64_t result = m_quorum_cop.get_uptime_proof(key);
    return result;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_uptime_proof(const NOTIFY_UPTIME_PROOF::request &proof)
  {
    return m_quorum_cop.handle_uptime_proof(proof);
  }
  //-----------------------------------------------------------------------------------------------
  void core::on_transaction_relayed(const cryptonote::blobdata& tx_blob)
  {
    std::vector<std::pair<crypto::hash, cryptonote::blobdata>> txs;
    cryptonote::transaction tx;
    crypto::hash tx_hash, tx_prefix_hash;
    if (!parse_and_validate_tx_from_blob(tx_blob, tx, tx_hash, tx_prefix_hash))
    {
      LOG_ERROR("Failed to parse relayed transaction");
      return;
    }
    txs.push_back(std::make_pair(tx_hash, std::move(tx_blob)));
    m_mempool.set_relayed(txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::relay_deregister_votes()
  {
    NOTIFY_NEW_DEREGISTER_VOTE::request req;
    req.votes = m_deregister_vote_pool.get_relayable_votes();
    if (!req.votes.empty())
    {
      cryptonote_connection_context fake_context {};
      if (get_protocol()->relay_deregister_votes(req, fake_context))
        m_deregister_vote_pool.set_relayed(req.votes);
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_block_template(block& b, const account_public_address& adr, difficulty_type& diffic, uint64_t& height, uint64_t& expected_reward, const blobdata& ex_nonce)
  {
    return m_blockchain_storage.create_block_template(b, adr, diffic, height, expected_reward, ex_nonce);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::find_blockchain_supplement(const std::list<crypto::hash>& qblock_ids, NOTIFY_RESPONSE_CHAIN_ENTRY::request& resp) const
  {
    return m_blockchain_storage.find_blockchain_supplement(qblock_ids, resp);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::find_blockchain_supplement(const uint64_t req_start_block, const std::list<crypto::hash>& qblock_ids, std::vector<std::pair<std::pair<cryptonote::blobdata, crypto::hash>, std::vector<std::pair<crypto::hash, cryptonote::blobdata> > > >& blocks, uint64_t& total_height, uint64_t& start_height, bool pruned, bool get_miner_tx_hash, size_t max_count) const
  {
    return m_blockchain_storage.find_blockchain_supplement(req_start_block, qblock_ids, blocks, total_height, start_height, pruned, get_miner_tx_hash, max_count);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_outs(const COMMAND_RPC_GET_OUTPUTS_BIN::request& req, COMMAND_RPC_GET_OUTPUTS_BIN::response& res) const
  {
    return m_blockchain_storage.get_outs(req, res);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_output_distribution(uint64_t amount, uint64_t from_height, uint64_t to_height, uint64_t &start_height, std::vector<uint64_t> &distribution, uint64_t &base) const
  {
    return m_blockchain_storage.get_output_distribution(amount, from_height, to_height, start_height, distribution, base);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_output_blacklist(std::vector<uint64_t> &blacklist) const
  {
    return m_blockchain_storage.get_output_blacklist(blacklist);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_tx_outputs_gindexs(const crypto::hash& tx_id, std::vector<uint64_t>& indexs) const
  {
    return m_blockchain_storage.get_tx_outputs_gindexs(tx_id, indexs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_tx_outputs_gindexs(const crypto::hash& tx_id, size_t n_txes, std::vector<std::vector<uint64_t>>& indexs) const
  {
    return m_blockchain_storage.get_tx_outputs_gindexs(tx_id, n_txes, indexs);
  }
  //-----------------------------------------------------------------------------------------------
  void core::pause_mine()
  {
    m_miner.pause();
  }
  //-----------------------------------------------------------------------------------------------
  void core::resume_mine()
  {
    m_miner.resume();
  }
  //-----------------------------------------------------------------------------------------------
  block_complete_entry get_block_complete_entry(block& b, tx_memory_pool &pool)
  {
    block_complete_entry bce;
    bce.block = cryptonote::block_to_blob(b);
    for (const auto &tx_hash: b.tx_hashes)
    {
      cryptonote::blobdata txblob;
      CHECK_AND_ASSERT_THROW_MES(pool.get_transaction(tx_hash, txblob), "Transaction not found in pool");
      bce.txs.push_back(txblob);
    }
    return bce;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_block_found(block& b)
  {
    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    m_miner.pause();
    std::vector<block_complete_entry> blocks;
    try
    {
      blocks.push_back(get_block_complete_entry(b, m_mempool));
    }
    catch (const std::exception &e)
    {
      m_miner.resume();
      return false;
    }
    prepare_handle_incoming_blocks(blocks);
    m_blockchain_storage.add_new_block(b, bvc);
    cleanup_handle_incoming_blocks(true);
    //anyway - update miner template
    update_miner_block_template();
    m_miner.resume();


    CHECK_AND_ASSERT_MES(!bvc.m_verifivation_failed, false, "mined block failed verification");
    if(bvc.m_added_to_main_chain)
    {
      cryptonote_connection_context exclude_context = boost::value_initialized<cryptonote_connection_context>();
      NOTIFY_NEW_BLOCK::request arg = AUTO_VAL_INIT(arg);
      arg.current_blockchain_height = m_blockchain_storage.get_current_blockchain_height();
      std::vector<crypto::hash> missed_txs;
      std::vector<cryptonote::blobdata> txs;
      m_blockchain_storage.get_transactions_blobs(b.tx_hashes, txs, missed_txs);
      if(missed_txs.size() &&  m_blockchain_storage.get_block_id_by_height(get_block_height(b)) != get_block_hash(b))
      {
        LOG_PRINT_L1("Block found but, seems that reorganize just happened after that, do not relay this block");
        return true;
      }
      CHECK_AND_ASSERT_MES(txs.size() == b.tx_hashes.size() && !missed_txs.size(), false, "can't find some transactions in found block:" << get_block_hash(b) << " txs.size()=" << txs.size()
        << ", b.tx_hashes.size()=" << b.tx_hashes.size() << ", missed_txs.size()" << missed_txs.size());

      block_to_blob(b, arg.b.block);
      //pack transactions
      for(auto& tx:  txs)
        arg.b.txs.push_back(tx);

      m_pprotocol->relay_block(arg, exclude_context);
    }
    return bvc.m_added_to_main_chain;
  }
  //-----------------------------------------------------------------------------------------------
  void core::on_synchronized()
  {
    m_miner.on_synchronized();
  }
  //-----------------------------------------------------------------------------------------------
  void core::safesyncmode(const bool onoff)
  {
    m_blockchain_storage.safesyncmode(onoff);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_new_block(const block& b, block_verification_context& bvc)
  {
    return m_blockchain_storage.add_new_block(b, bvc);
  }

  //-----------------------------------------------------------------------------------------------
  bool core::prepare_handle_incoming_blocks(const std::vector<block_complete_entry> &blocks)
  {
    m_incoming_tx_lock.lock();
    m_blockchain_storage.prepare_handle_incoming_blocks(blocks);
    return true;
  }

  //-----------------------------------------------------------------------------------------------
  bool core::cleanup_handle_incoming_blocks(bool force_sync)
  {
    bool success = false;
    try {
      success = m_blockchain_storage.cleanup_handle_incoming_blocks(force_sync);
    }
    catch (...) {}
    m_incoming_tx_lock.unlock();
    return success;
  }

  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_block(const blobdata& block_blob, block_verification_context& bvc, bool update_miner_blocktemplate)
  {
    TRY_ENTRY();

    // load json & DNS checkpoints every 10min/hour respectively,
    // and verify them with respect to what blocks we already have
    CHECK_AND_ASSERT_MES(update_checkpoints(), false, "One or more checkpoints loaded from json or dns conflicted with existing checkpoints.");

    bvc = boost::value_initialized<block_verification_context>();
    if(block_blob.size() > get_max_block_size())
    {
      LOG_PRINT_L1("WRONG BLOCK BLOB, too big size " << block_blob.size() << ", rejected");
      bvc.m_verifivation_failed = true;
      return false;
    }

    block b = AUTO_VAL_INIT(b);
    if(!parse_and_validate_block_from_blob(block_blob, b))
    {
      LOG_PRINT_L1("Failed to parse and validate new block");
      bvc.m_verifivation_failed = true;
      return false;
    }
    add_new_block(b, bvc);
    if(update_miner_blocktemplate && bvc.m_added_to_main_chain)
       update_miner_block_template();
    return true;

    CATCH_ENTRY_L0("core::handle_incoming_block()", false);
  }
  //-----------------------------------------------------------------------------------------------
  // Used by the RPC server to check the size of an incoming
  // block_blob
  bool core::check_incoming_block_size(const blobdata& block_blob) const
  {
    if(block_blob.size() > get_max_block_size())
    {
      LOG_PRINT_L1("WRONG BLOCK BLOB, too big size " << block_blob.size() << ", rejected");
      return false;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  crypto::hash core::get_tail_id() const
  {
    return m_blockchain_storage.get_tail_id();
  }
  //-----------------------------------------------------------------------------------------------
  difficulty_type core::get_block_cumulative_difficulty(uint64_t height) const
  {
    return m_blockchain_storage.get_db().get_block_cumulative_difficulty(height);
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_pool_transactions_count() const
  {
    return m_mempool.get_transactions_count();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::have_block(const crypto::hash& id) const
  {
    return m_blockchain_storage.have_block(id);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::parse_tx_from_blob(transaction& tx, crypto::hash& tx_hash, crypto::hash& tx_prefix_hash, const blobdata& blob) const
  {
    return parse_and_validate_tx_from_blob(blob, tx, tx_hash, tx_prefix_hash);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_syntax(const transaction& tx) const
  {
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transactions(std::vector<transaction>& txs, bool include_sensitive_data) const
  {
    m_mempool.get_transactions(txs, include_sensitive_data);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transaction_hashes(std::vector<crypto::hash>& txs, bool include_sensitive_data) const
  {
    m_mempool.get_transaction_hashes(txs, include_sensitive_data);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transaction_stats(struct txpool_stats& stats, bool include_sensitive_data) const
  {
    m_mempool.get_transaction_stats(stats, include_sensitive_data);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transaction(const crypto::hash &id, cryptonote::blobdata& tx) const
  {
    return m_mempool.get_transaction(id, tx);
  }  
  //-----------------------------------------------------------------------------------------------
  bool core::pool_has_tx(const crypto::hash &id) const
  {
    return m_mempool.have_tx(id);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transactions_and_spent_keys_info(std::vector<tx_info>& tx_infos, std::vector<spent_key_image_info>& key_image_infos, bool include_sensitive_data) const
  {
    return m_mempool.get_transactions_and_spent_keys_info(tx_infos, key_image_infos, include_sensitive_data);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_for_rpc(std::vector<cryptonote::rpc::tx_in_pool>& tx_infos, cryptonote::rpc::key_images_with_tx_hashes& key_image_infos) const
  {
    return m_mempool.get_pool_for_rpc(tx_infos, key_image_infos);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_short_chain_history(std::list<crypto::hash>& ids) const
  {
    return m_blockchain_storage.get_short_chain_history(ids);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_get_objects(NOTIFY_REQUEST_GET_OBJECTS::request& arg, NOTIFY_RESPONSE_GET_OBJECTS::request& rsp, cryptonote_connection_context& context)
  {
    return m_blockchain_storage.handle_get_objects(arg, rsp);
  }
  //-----------------------------------------------------------------------------------------------
  crypto::hash core::get_block_id_by_height(uint64_t height) const
  {
    return m_blockchain_storage.get_block_id_by_height(height);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_block_by_hash(const crypto::hash &h, block &blk, bool *orphan) const
  {
    return m_blockchain_storage.get_block_by_hash(h, blk, orphan);
  }
  //-----------------------------------------------------------------------------------------------
  std::string core::print_pool(bool short_format) const
  {
    return m_mempool.print_pool(short_format);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::update_miner_block_template()
  {
    m_miner.on_block_chain_update();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  void core::do_uptime_proof_call()
  {
    // wait one block before starting uptime proofs.
    std::vector<full_nodes::full_node_pubkey_info> const states = get_full_node_list_state({ m_full_node_pubkey });
    if (!states.empty() && states[0].info.registration_height + 1 < get_current_blockchain_height())
    {
      // Code snippet from Github @Jagerman
      m_check_uptime_proof_interval.do_call([&states, this](){
        uint64_t last_uptime = m_quorum_cop.get_uptime_proof(states[0].pubkey);
        if (last_uptime <= static_cast<uint64_t>(time(nullptr) - UPTIME_PROOF_FREQUENCY_IN_SECONDS))
          this->submit_uptime_proof();

        return true;
      });
    }
    else
    {
      // reset the interval so that we're ready when we register, OR if we get deregistered this primes us up for re-registration in the same session
      m_check_uptime_proof_interval = epee::math_helper::once_a_time_seconds<UPTIME_PROOF_BUFFER_IN_SECONDS, true /*start_immediately*/>();
    }
  }
  //-----------------------------------------------------------------------------------------------
  bool core::on_idle()
  {
    if(!m_starter_message_showed)
    {
      std::string main_message;
      if (m_offline)
        main_message = "The daemon is running offline and will not attempt to sync to the Antd network.";
      else
        main_message = "The daemon will start synchronizing with the network. This may take a long time to complete.";
      MGINFO_YELLOW(ENDL << "**********************************************************************" << ENDL
        << main_message << ENDL
        << ENDL
        << "You can set the level of process detailization through \"set_log <level|categories>\" command," << ENDL
        << "where <level> is between 0 (no details) and 4 (very verbose), or custom category based levels (eg, *:WARNING)." << ENDL
        << ENDL
        << "Use the \"help\" command to see the list of available commands." << ENDL
        << "Use \"help <command>\" to see a command's documentation." << ENDL
        << "**********************************************************************" << ENDL);
      m_starter_message_showed = true;
    }

    m_fork_moaner.do_call(boost::bind(&core::check_fork_time, this));
    m_txpool_auto_relayer.do_call(boost::bind(&core::relay_txpool_transactions, this));
    m_deregisters_auto_relayer.do_call(boost::bind(&core::relay_deregister_votes, this));
    // m_check_updates_interval.do_call(boost::bind(&core::check_updates, this));
    m_check_disk_space_interval.do_call(boost::bind(&core::check_disk_space, this));
    m_block_rate_interval.do_call(boost::bind(&core::check_block_rate, this));
    m_diff_recalc_interval.do_call(boost::bind(&core::recalculate_difficulties, this));

    time_t const lifetime = time(nullptr) - get_start_time();
    if (m_full_node && lifetime > DIFFICULTY_TARGET_V2) // Give us some time to connect to peers before sending uptimes
    {
      do_uptime_proof_call();
    }

    m_uptime_proof_pruner.do_call(boost::bind(&full_nodes::quorum_cop::prune_uptime_proof, &m_quorum_cop));

    m_blockchain_pruning_interval.do_call(boost::bind(&core::update_blockchain_pruning, this));
    m_miner.on_idle();
    m_mempool.on_idle();

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    antd::core_is_idle = true;
#endif

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_fork_time()
  {
    HardFork::State state = m_blockchain_storage.get_hard_fork_state();
    const el::Level level = el::Level::Warning;
    switch (state) {
      case HardFork::LikelyForked:
        MCLOG_RED(level, "global", "**********************************************************************");
        MCLOG_RED(level, "global", "Last scheduled hard fork is too far in the past.");
        MCLOG_RED(level, "global", "We are most likely forked from the network. Daemon update needed now.");
        MCLOG_RED(level, "global", "**********************************************************************");
        break;
      case HardFork::UpdateNeeded:
        MCLOG_RED(level, "global", "**********************************************************************");
        MCLOG_RED(level, "global", "Last scheduled hard fork time shows a daemon update is needed soon.");
        MCLOG_RED(level, "global", "**********************************************************************");
        break;
      default:
        break;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  uint8_t core::get_ideal_hard_fork_version() const
  {
    return get_blockchain_storage().get_ideal_hard_fork_version();
  }
  //-----------------------------------------------------------------------------------------------
  uint8_t core::get_ideal_hard_fork_version(uint64_t height) const
  {
    return get_blockchain_storage().get_ideal_hard_fork_version(height);
  }
  //-----------------------------------------------------------------------------------------------
  uint8_t core::get_hard_fork_version(uint64_t height) const
  {
    return get_blockchain_storage().get_hard_fork_version(height);
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_earliest_ideal_height_for_version(uint8_t version) const
  {
    return get_blockchain_storage().get_earliest_ideal_height_for_version(version);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_updates()
  {
    static const char software[] = "antd";
#ifdef BUILD_TAG
    static const char buildtag[] = BOOST_PP_STRINGIZE(BUILD_TAG);
    static const char subdir[] = "cli"; // because it can never be simple
#else
    static const char buildtag[] = "source";
    static const char subdir[] = "source"; // because it can never be simple
#endif

    if (m_offline)
      return true;

    if (check_updates_level == UPDATES_DISABLED)
      return true;

    std::string version, hash;
    MCDEBUG("updates", "Checking for a new " << software << " version for " << buildtag);
    if (!tools::check_updates(software, buildtag, version, hash))
      return false;

    if (tools::vercmp(version.c_str(), ANTD_VERSION) <= 0)
    {
      m_update_available = false;
      return true;
    }

    std::string url = tools::get_update_url(software, subdir, buildtag, version, true);
    MCLOG_CYAN(el::Level::Info, "global", "Version " << version << " of " << software << " for " << buildtag << " is available: " << url << ", SHA256 hash " << hash);
    m_update_available = true;

    if (check_updates_level == UPDATES_NOTIFY)
      return true;

    url = tools::get_update_url(software, subdir, buildtag, version, false);
    std::string filename;
    const char *slash = strrchr(url.c_str(), '/');
    if (slash)
      filename = slash + 1;
    else
      filename = std::string(software) + "-update-" + version;
    boost::filesystem::path path(epee::string_tools::get_current_module_folder());
    path /= filename;

    boost::unique_lock<boost::mutex> lock(m_update_mutex);

    if (m_update_download != 0)
    {
      MCDEBUG("updates", "Already downloading update");
      return true;
    }

    crypto::hash file_hash;
    if (!tools::sha256sum(path.string(), file_hash) || (hash != epee::string_tools::pod_to_hex(file_hash)))
    {
      MCDEBUG("updates", "We don't have that file already, downloading");
      const std::string tmppath = path.string() + ".tmp";
      if (epee::file_io_utils::is_file_exist(tmppath))
      {
        MCDEBUG("updates", "We have part of the file already, resuming download");
      }
      m_last_update_length = 0;
      m_update_download = tools::download_async(tmppath, url, [this, hash, path](const std::string &tmppath, const std::string &uri, bool success) {
        bool remove = false, good = true;
        if (success)
        {
          crypto::hash file_hash;
          if (!tools::sha256sum(tmppath, file_hash))
          {
            MCERROR("updates", "Failed to hash " << tmppath);
            remove = true;
            good = false;
          }
          else if (hash != epee::string_tools::pod_to_hex(file_hash))
          {
            MCERROR("updates", "Download from " << uri << " does not match the expected hash");
            remove = true;
            good = false;
          }
        }
        else
        {
          MCERROR("updates", "Failed to download " << uri);
          good = false;
        }
        boost::unique_lock<boost::mutex> lock(m_update_mutex);
        m_update_download = 0;
        if (success && !remove)
        {
          std::error_code e = tools::replace_file(tmppath, path.string());
          if (e)
          {
            MCERROR("updates", "Failed to rename downloaded file");
            good = false;
          }
        }
        else if (remove)
        {
          if (!boost::filesystem::remove(tmppath))
          {
            MCERROR("updates", "Failed to remove invalid downloaded file");
            good = false;
          }
        }
        if (good)
          MCLOG_CYAN(el::Level::Info, "updates", "New version downloaded to " << path.string());
      }, [this](const std::string &path, const std::string &uri, size_t length, ssize_t content_length) {
        if (length >= m_last_update_length + 1024 * 1024 * 10)
        {
          m_last_update_length = length;
          MCDEBUG("updates", "Downloaded " << length << "/" << (content_length ? std::to_string(content_length) : "unknown"));
        }
        return true;
      });
    }
    else
    {
      MCDEBUG("updates", "We already have " << path << " with expected hash");
    }

    lock.unlock();

    if (check_updates_level == UPDATES_DOWNLOAD)
      return true;

    MCERROR("updates", "Download/update not implemented yet");
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_disk_space()
  {
    uint64_t free_space = get_free_space();
    if (free_space < 1ull * 1024 * 1024 * 1024) // 1 GB
    {
      const el::Level level = el::Level::Warning;
      MCLOG_RED(level, "global", "Free space is below 1 GB on " << m_config_folder);
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  double factorial(unsigned int n)
  {
    if (n <= 1)
      return 1.0;
    double f = n;
    while (n-- > 1)
      f *= n;
    return f;
  }
  //-----------------------------------------------------------------------------------------------
  static double probability(unsigned int blocks, unsigned int expected)
  {
    // https://www.umass.edu/wsp/resources/poisson/#computing
    return pow(expected, blocks) / (factorial(blocks) * exp(expected));
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_block_rate()
  {
    if (m_offline || m_target_blockchain_height > get_current_blockchain_height())
    {
      MDEBUG("Not checking block rate, offline or syncing");
      return true;
    }

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    MDEBUG("Not checking block rate, integration test mode");
    return true;
#endif

    static constexpr double threshold = 1. / (864000 / DIFFICULTY_TARGET_V2); // one false positive every 10 days

    const time_t now = time(NULL);
    const std::vector<time_t> timestamps = m_blockchain_storage.get_last_block_timestamps(60);

    static const unsigned int seconds[] = { 5400, 3600, 1800, 1200, 600 };
    for (size_t n = 0; n < sizeof(seconds)/sizeof(seconds[0]); ++n)
    {
      unsigned int b = 0;
      const time_t time_boundary = now - static_cast<time_t>(seconds[n]);
      for (time_t ts: timestamps) b += ts >= time_boundary;
      const double p = probability(b, seconds[n] / DIFFICULTY_TARGET_V2);
      MDEBUG("blocks in the last " << seconds[n] / 60 << " minutes: " << b << " (probability " << p << ")");
      if (p < threshold)
      {
        //MWARNING("There were " << b << " blocks in the last " << seconds[n] / 60 << " minutes, there might be large hash rate changes, or we might be partitioned, cut off from the Antd network or under attack. Or it could be just sheer bad luck.");

        std::shared_ptr<tools::Notify> block_rate_notify = m_block_rate_notify;
        if (block_rate_notify)
        {
          auto expected = seconds[n] / DIFFICULTY_TARGET_V2;
          block_rate_notify->notify("%t", std::to_string(seconds[n] / 60).c_str(), "%b", std::to_string(b).c_str(), "%e", std::to_string(expected).c_str(), NULL);
        }

        break; // no need to look further
      }
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::recalculate_difficulties()
  {
    m_blockchain_storage.recalculate_difficulties();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::update_blockchain_pruning()
  {
    return m_blockchain_storage.update_blockchain_pruning();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_blockchain_pruning()
  {
    return m_blockchain_storage.check_blockchain_pruning();
  }
  //-----------------------------------------------------------------------------------------------
  void core::set_target_blockchain_height(uint64_t target_blockchain_height)
  {
    m_target_blockchain_height = target_blockchain_height;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_target_blockchain_height() const
  {
    return m_target_blockchain_height;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::prevalidate_block_hashes(uint64_t height, const std::vector<crypto::hash> &hashes)
  {
    return get_blockchain_storage().prevalidate_block_hashes(height, hashes);
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_free_space() const
  {
    boost::filesystem::path path(m_config_folder);
    boost::filesystem::space_info si = boost::filesystem::space(path);
    return si.available;
  }
  //-----------------------------------------------------------------------------------------------
  const std::shared_ptr<const full_nodes::quorum_state> core::get_quorum_state(uint64_t height) const
  {
    return m_full_node_list.get_quorum_state(height);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::is_full_node(const crypto::public_key& pubkey) const
  {
    return m_full_node_list.is_full_node(pubkey);
  }
  //-----------------------------------------------------------------------------------------------
  const std::vector<full_nodes::key_image_blacklist_entry> &core::get_full_node_blacklisted_key_images() const
  {
    const auto &result = m_full_node_list.get_blacklisted_key_images();
    return result;
  }
  //-----------------------------------------------------------------------------------------------
  std::vector<full_nodes::full_node_pubkey_info> core::get_full_node_list_state(const std::vector<crypto::public_key> &full_node_pubkeys) const
  {
    std::vector<full_nodes::full_node_pubkey_info> result = m_full_node_list.get_full_node_list_state(full_node_pubkeys);
    return result;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_deregister_vote(const full_nodes::deregister_vote& vote, vote_verification_context &vvc)
  {
    uint64_t latest_block_height = std::max(get_current_blockchain_height(), get_target_blockchain_height());
    uint64_t delta_height = latest_block_height - vote.block_height;

    if (vote.block_height < latest_block_height && delta_height >= full_nodes::deregister_vote::VOTE_LIFETIME_BY_HEIGHT)
    {
      LOG_PRINT_L1("Received vote for height: " << vote.block_height
                << " and fullnode: "     << vote.full_node_index
                << ", is older than: "       << full_nodes::deregister_vote::VOTE_LIFETIME_BY_HEIGHT
                << " blocks and has been rejected.");
      vvc.m_invalid_block_height = true;
    }
    else if (vote.block_height > latest_block_height)
    {
      LOG_PRINT_L1("Received vote for height: " << vote.block_height
                << " and fullnode: "     << vote.full_node_index
                << ", is newer than: "       << latest_block_height
                << " (latest block height) and has been rejected.");
      vvc.m_invalid_block_height = true;
    }

    if (vvc.m_invalid_block_height)
    {
      vvc.m_verification_failed = true;
      return false;
    }

    const auto quorum_state = m_full_node_list.get_quorum_state(vote.block_height);
    if (!quorum_state)
    {
      vvc.m_verification_failed  = true;
      vvc.m_invalid_block_height = true;
      LOG_ERROR("Could not get quorum state for height: " << vote.block_height);
      return false;
    }

    cryptonote::transaction deregister_tx;
    int hf_version = m_blockchain_storage.get_current_hard_fork_version();
    bool result    = m_deregister_vote_pool.add_vote(hf_version, vote, vvc, *quorum_state, deregister_tx);
    if (result && vvc.m_full_tx_deregister_made)
    {
      tx_verification_context tvc = AUTO_VAL_INIT(tvc);
      blobdata const tx_blob = tx_to_blob(deregister_tx);

      result = handle_incoming_tx(tx_blob, tvc, false /*keeped_by_block*/, false /*relayed*/, false /*do_not_relay*/);
      if (!result || tvc.m_verifivation_failed)
      {
        LOG_PRINT_L1("A full deregister tx for height: " << vote.block_height <<
                     " and fullnode: " << vote.full_node_index <<
                     " could not be verified and was not added to the memory pool, reason: " <<
                     print_tx_verification_context(tvc, &deregister_tx));
      }
    }

    return result;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_full_node_keys(crypto::public_key &pub_key, crypto::secret_key &sec_key) const
  {
    if (m_full_node)
    {
      pub_key = m_full_node_pubkey;
      sec_key = m_full_node_key;
    }
    return m_full_node;
  }
  uint32_t core::get_blockchain_pruning_seed() const
  {
    return get_blockchain_storage().get_blockchain_pruning_seed();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::prune_blockchain(uint32_t pruning_seed)
  {
    return get_blockchain_storage().prune_blockchain(pruning_seed);
  }
  //-----------------------------------------------------------------------------------------------
  void core::get_all_full_nodes_public_keys(std::vector<crypto::public_key>& keys, bool fully_funded_nodes_only) const
  {
    m_full_node_list.get_all_full_nodes_public_keys(keys, fully_funded_nodes_only);
  }
  //-----------------------------------------------------------------------------------------------
  std::time_t core::get_start_time() const
  {
    return start_time;
  }
  //-----------------------------------------------------------------------------------------------
  void core::graceful_exit()
  {
    raise(SIGTERM);
  }
}
