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

#include <boost/algorithm/string.hpp>
#include "cryptonote_config.h"
#include "version.h"
#include "string_tools.h"
#include "daemon/command_server.h"

#include "common/antd_integration_test_hooks.h"


#undef ANTD_DEFAULT_LOG_CATEGORY
#define ANTD_DEFAULT_LOG_CATEGORY "daemon"

namespace daemonize {

namespace p = std::placeholders;

t_command_server::t_command_server(
    uint32_t ip
  , uint16_t port
  , const boost::optional<tools::login>& login
  , bool is_rpc
  , cryptonote::core_rpc_server* rpc_server
  )
  : m_parser(ip, port, login, is_rpc, rpc_server)
  , m_command_lookup()
  , m_is_rpc(is_rpc)
{
  m_command_lookup.set_handler(
      "help"
    , std::bind(&t_command_server::help, this, p::_1)
    , "help [<command>]"
    , "Show the help section or the documentation about a <command>."
    );
  m_command_lookup.set_handler(
      "print_height"
    , std::bind(&t_command_parser_executor::print_height, &m_parser, p::_1)
    , "Print the local blockchain height."
    );
  m_command_lookup.set_handler(
      "print_pl"
    , std::bind(&t_command_parser_executor::print_peer_list, &m_parser, p::_1)
    , "print_pl [white] [gray] [<limit>]"
    , "Print the current peer list."
    );
  m_command_lookup.set_handler(
      "print_pl_stats"
    , std::bind(&t_command_parser_executor::print_peer_list_stats, &m_parser, p::_1)
    , "Print the peer list statistics."
    );
  m_command_lookup.set_handler(
      "print_cn"
    , std::bind(&t_command_parser_executor::print_connections, &m_parser, p::_1)
    , "Print the current connections."
    );
  m_command_lookup.set_handler(
      "print_bc"
    , std::bind(&t_command_parser_executor::print_blockchain_info, &m_parser, p::_1)
    , "print_bc <begin_height> [<end_height>]"
    , "Print the blockchain info in a given blocks range."
    );
  m_command_lookup.set_handler(
      "print_block"
    , std::bind(&t_command_parser_executor::print_block, &m_parser, p::_1)
    , "print_block <block_hash> | <block_height>"
    , "Print a given block."
    );
  m_command_lookup.set_handler(
      "show_article"
    , std::bind(&t_command_parser_executor::show_article, &m_parser, p::_1)
    , "show_article <txid>"
    , "Show article data from a transaction."
    );
  m_command_lookup.set_handler(
      "print_tx"
    , std::bind(&t_command_parser_executor::print_transaction, &m_parser, p::_1)
    , "print_tx <transaction_hash> [+hex] [+json]"
    , "Print a given transaction."
    );
  m_command_lookup.set_handler(
      "print_quorum_state"
    , std::bind(&t_command_parser_executor::print_quorum_state, &m_parser, p::_1)
    , "print_quorum_state <height>"
    , "Print the quorum state for the block height."
    );
  m_command_lookup.set_handler(
      "print_sn_key"
    , std::bind(&t_command_parser_executor::print_sn_key, &m_parser, p::_1)
    , "print_sn_key"
    , "Print this daemon's fullnode key, if it is one and launched in fullnode mode."
    );
  m_command_lookup.set_handler(
      "print_sr"
    , std::bind(&t_command_parser_executor::print_sr, &m_parser, p::_1)
    , "print_sr <height>"
    , "Print the staking requirement for the height."
    );
  m_command_lookup.set_handler(
      "prepare_registration"
    , std::bind(&t_command_parser_executor::prepare_registration, &m_parser)
    , "prepare_registration"
    , "Interactive prompt to prepare a fullnode registration command. The resulting registration command can be run in the command-line wallet to send the registration to the blockchain."
    );
  m_command_lookup.set_handler(
      "print_sn"
    , std::bind(&t_command_parser_executor::print_sn, &m_parser, p::_1)
    , "print_sn [<pubkey> [...]] [+json]"
    , "Print fullnode registration info for the current height"
    );
  m_command_lookup.set_handler(
      "print_sn_status"
    , std::bind(&t_command_parser_executor::print_sn_status, &m_parser, p::_1)
    , "print_sn_status [+json]"
    , "Print fullnode registration info for this fullnode"
    );
  m_command_lookup.set_handler(
      "is_key_image_spent"
    , std::bind(&t_command_parser_executor::is_key_image_spent, &m_parser, p::_1)
    , "is_key_image_spent <key_image>"
    , "Print whether a given key image is in the spent key images set."
    );
  m_command_lookup.set_handler(
      "start_mining"
    , std::bind(&t_command_parser_executor::start_mining, &m_parser, p::_1)
    , "start_mining <addr> [<threads>] [do_background_mining] [ignore_battery]"
    , "Start mining for specified address. Defaults to 1 thread and no background mining."
    );
  m_command_lookup.set_handler(
      "stop_mining"
    , std::bind(&t_command_parser_executor::stop_mining, &m_parser, p::_1)
    , "Stop mining."
    );
  m_command_lookup.set_handler(
      "print_pool"
    , std::bind(&t_command_parser_executor::print_transaction_pool_long, &m_parser, p::_1)
    , "Print the transaction pool using a long format."
    );
  m_command_lookup.set_handler(
      "print_pool_sh"
    , std::bind(&t_command_parser_executor::print_transaction_pool_short, &m_parser, p::_1)
    , "Print transaction pool using a short format."
    );
  m_command_lookup.set_handler(
      "print_pool_stats"
    , std::bind(&t_command_parser_executor::print_transaction_pool_stats, &m_parser, p::_1)
    , "Print the transaction pool's statistics."
    );
  m_command_lookup.set_handler(
      "show_hr"
    , std::bind(&t_command_parser_executor::show_hash_rate, &m_parser, p::_1)
    , "Start showing the current hash rate."
    );
  m_command_lookup.set_handler(
      "hide_hr"
    , std::bind(&t_command_parser_executor::hide_hash_rate, &m_parser, p::_1)
    , "Stop showing the hash rate."
    );
  m_command_lookup.set_handler(
      "save"
    , std::bind(&t_command_parser_executor::save_blockchain, &m_parser, p::_1)
    , "Save the blockchain."
    );
  m_command_lookup.set_handler(
      "set_log"
    , std::bind(&t_command_parser_executor::set_log_level, &m_parser, p::_1)
    , "set_log <level>|<{+,-,}categories>"
    , "Change the current log level/categories where <level> is a number 0-4."
    );
  m_command_lookup.set_handler(
      "diff"
    , std::bind(&t_command_parser_executor::show_difficulty, &m_parser, p::_1)
    , "Show the current difficulty."
    );
  m_command_lookup.set_handler(
      "status"
    , std::bind(&t_command_parser_executor::show_status, &m_parser, p::_1)
    , "Show the current status."
    );
  m_command_lookup.set_handler(
      "stop_daemon"
    , std::bind(&t_command_parser_executor::stop_daemon, &m_parser, p::_1)
    , "Stop the daemon."
    );
  m_command_lookup.set_handler(
      "exit"
    , std::bind(&t_command_parser_executor::stop_daemon, &m_parser, p::_1)
    , "Stop the daemon."
    );
  m_command_lookup.set_handler(
      "print_status"
    , std::bind(&t_command_parser_executor::print_status, &m_parser, p::_1)
    , "Print the current daemon status."
    );
  m_command_lookup.set_handler(
      "limit"
    , std::bind(&t_command_parser_executor::set_limit, &m_parser, p::_1)
    , "limit [<kB/s>]"
    , "Get or set the download and upload limit."
    );
  m_command_lookup.set_handler(
      "limit_up"
    , std::bind(&t_command_parser_executor::set_limit_up, &m_parser, p::_1)
    , "limit_up [<kB/s>]"
    , "Get or set the upload limit."
    );
  m_command_lookup.set_handler(
      "limit_down"
    , std::bind(&t_command_parser_executor::set_limit_down, &m_parser, p::_1)
    , "limit_down [<kB/s>]"
    , "Get or set the download limit."
    );
    m_command_lookup.set_handler(
      "out_peers"
    , std::bind(&t_command_parser_executor::out_peers, &m_parser, p::_1)
    , "out_peers <max_number>"
    , "Set the <max_number> of out peers."
    );
    m_command_lookup.set_handler(
      "in_peers"
    , std::bind(&t_command_parser_executor::in_peers, &m_parser, p::_1)
    , "in_peers <max_number>"
    , "Set the <max_number> of in peers."
    );
    m_command_lookup.set_handler(
      "start_save_graph"
    , std::bind(&t_command_parser_executor::start_save_graph, &m_parser, p::_1)
    , "Start saving data for dr antd."
    );
    m_command_lookup.set_handler(
      "stop_save_graph"
    , std::bind(&t_command_parser_executor::stop_save_graph, &m_parser, p::_1)
    , "Stop saving data for dr antd."
    );
    m_command_lookup.set_handler(
      "hard_fork_info"
    , std::bind(&t_command_parser_executor::hard_fork_info, &m_parser, p::_1)
    , "Print the hard fork voting information."
    );
    m_command_lookup.set_handler(
      "bans"
    , std::bind(&t_command_parser_executor::show_bans, &m_parser, p::_1)
    , "Show the currently banned IPs."
    );
    m_command_lookup.set_handler(
      "ban"
    , std::bind(&t_command_parser_executor::ban, &m_parser, p::_1)
    , "ban <IP> [<seconds>]"
    , "Ban a given <IP> for a given amount of <seconds>."
    );
    m_command_lookup.set_handler(
      "unban"
    , std::bind(&t_command_parser_executor::unban, &m_parser, p::_1)
    , "unban <IP>"
    , "Unban a given <IP>."
    );
    m_command_lookup.set_handler(
      "flush_txpool"
    , std::bind(&t_command_parser_executor::flush_txpool, &m_parser, p::_1)
    , "flush_txpool [<txid>]"
    , "Flush a transaction from the tx pool by its <txid>, or the whole tx pool."
    );
    m_command_lookup.set_handler(
      "output_histogram"
    , std::bind(&t_command_parser_executor::output_histogram, &m_parser, p::_1)
    , "output_histogram [@<amount>] <min_count> [<max_count>]"
    , "Print the output histogram of outputs."
    );
    m_command_lookup.set_handler(
      "print_coinbase_tx_sum"
    , std::bind(&t_command_parser_executor::print_coinbase_tx_sum, &m_parser, p::_1)
    , "print_coinbase_tx_sum <start_height> [<block_count>]"
    , "Print the sum of coinbase transactions."
    );
    m_command_lookup.set_handler(
      "alt_chain_info"
    , std::bind(&t_command_parser_executor::alt_chain_info, &m_parser, p::_1)
    , "alt_chain_info [blockhash]"
    , "Print the information about alternative chains."
    );
    m_command_lookup.set_handler(
      "bc_dyn_stats"
    , std::bind(&t_command_parser_executor::print_blockchain_dynamic_stats, &m_parser, p::_1)
    , "bc_dyn_stats <last_block_count>"
    , "Print the information about current blockchain dynamic state."
    );
    m_command_lookup.set_handler(
      "update"
    , std::bind(&t_command_parser_executor::update, &m_parser, p::_1)
    , "update (check|download)"
    , "Check if an update is available, optionally downloads it if there is. Updating is not yet implemented."
    );
    m_command_lookup.set_handler(
      "relay_tx"
    , std::bind(&t_command_parser_executor::relay_tx, &m_parser, p::_1)
    , "relay_tx <txid>"
    , "Relay a given transaction by its <txid>."
    );
    m_command_lookup.set_handler(
      "sync_info"
    , std::bind(&t_command_parser_executor::sync_info, &m_parser, p::_1)
    , "Print information about the blockchain sync state."
    );
    m_command_lookup.set_handler(
      "pop_blocks"
    , std::bind(&t_command_parser_executor::pop_blocks, &m_parser, p::_1)
    , "pop_blocks <nblocks>"
    , "Remove blocks from end of blockchain"
    );
    m_command_lookup.set_handler(
      "version"
    , std::bind(&t_command_parser_executor::version, &m_parser, p::_1)
    , "Print version information."
    );
    m_command_lookup.set_handler(
      "prune_blockchain"
    , std::bind(&t_command_parser_executor::prune_blockchain, &m_parser, p::_1)
    , "Prune the blockchain."
    );
    m_command_lookup.set_handler(
      "check_blockchain_pruning"
    , std::bind(&t_command_parser_executor::check_blockchain_pruning, &m_parser, p::_1)
    , "Check the blockchain pruning."
    );
}

bool t_command_server::process_command_str(const std::string& cmd)
{
  return m_command_lookup.process_command_str(cmd);
}

bool t_command_server::process_command_vec(const std::vector<std::string>& cmd)
{
  bool result = m_command_lookup.process_command_vec(cmd);
  if (!result)
  {
    help(std::vector<std::string>());
  }
  return result;
}

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
#include <thread>
#endif

bool t_command_server::start_handling(std::function<void(void)> exit_handler)
{
  if (m_is_rpc) return false;

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
  auto handle_shared_mem_ins_and_outs = [&]()
  {
    // TODO(doyle): Hack, don't hook into input until the daemon has completely initialised, i.e. you can print the status
    while(!antd::core_is_idle) {}
    mlog_set_categories("");

    for (;;)
    {
      antd::fixed_buffer const input = antd::read_from_stdin_shared_mem();
      std::vector<std::string> args  = antd::separate_stdin_to_space_delim_args(&input);
      {
        boost::unique_lock<boost::mutex> scoped_lock(antd::integration_test_mutex);
        antd::use_standard_cout();
        std::cout << input.data << std::endl;
        antd::use_redirected_cout();
      }

      process_command_vec(args);
      if (args.size() == 1 && args[0] == "exit")
      {
        antd::deinit_integration_test_context();
        break;
      }

      antd::write_redirected_stdout_to_shared_mem();
    }
  };
  static std::thread handle_remote_stdin_out_thread(handle_shared_mem_ins_and_outs);
#endif

  m_command_lookup.start_handling("", get_commands_str(), exit_handler);
  return true;
}

void t_command_server::stop_handling()
{
  if (m_is_rpc) return;

  m_command_lookup.stop_handling();
}

bool t_command_server::help(const std::vector<std::string>& args)
{
  if(args.empty())
  {
    std::cout << get_commands_str() << std::endl;
  }
  else
  {
    std::cout << get_command_usage(args) << std::endl;
  }
  return true;
}

std::string t_command_server::get_commands_str()
{
  std::stringstream ss;
  ss << "Antd '" << ANTD_RELEASE_NAME << "' (v" << ANTD_VERSION_FULL << ")" << std::endl;
  ss << "Commands: " << std::endl;
  std::string usage = m_command_lookup.get_usage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << std::endl;
  return ss.str();
}

 std::string t_command_server::get_command_usage(const std::vector<std::string> &args)
 {
   std::pair<std::string, std::string> documentation = m_command_lookup.get_documentation(args);
   std::stringstream ss;
   if(documentation.first.empty())
   {
     ss << "Unknown command: " << args.front() << std::endl;
   }
   else
   {
     std::string usage = documentation.second.empty() ? args.front() : documentation.first;
     std::string description = documentation.second.empty() ? documentation.first : documentation.second;
     usage.insert(0, "  ");
     ss << "Command usage: " << std::endl << usage << std::endl << std::endl;
     boost::replace_all(description, "\n", "\n  ");
     description.insert(0, "  ");
     ss << "Command description: " << std::endl << description << std::endl;
   }
   return ss.str();
 }

} // namespace daemonize
