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

/*!
 * \file simplewallet.cpp
 * 
 * \brief Source file that defines simple_wallet class.
 */

#ifdef _WIN32
 #define __STDC_FORMAT_MACROS // NOTE(antd): Explicitly define the PRIu64 macro on Mingw
#endif

#include <thread>
#include <iostream>
#include <sstream>
#include <fstream>
#include <ctype.h>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include "include_base_utils.h"
#include "common/i18n.h"
#include "common/command_line.h"
#include "common/util.h"
#include "common/dns_utils.h"
#include "common/base58.h"
#include "common/scoped_message_writer.h"
#include "common/antd_integration_test_hooks.h"
#include "cryptonote_protocol/cryptonote_protocol_handler.h"
#include "cryptonote_core/full_node_deregister.h"
#include "cryptonote_core/full_node_list.h"
#include "simplewallet.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "storages/http_abstract_invoke.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "crypto/crypto.h"  // for crypto::secret_key definition
#include "mnemonics/electrum-words.h"
#include "rapidjson/document.h"
#include "common/json_util.h"
#include "ringct/rctSigs.h"
#include "multisig/multisig.h"
#include "wallet/wallet_args.h"
#include "version.h"
#include <stdexcept>
#include "int-util.h"
#include "wallet/message_store.h"

#ifdef WIN32
#include <boost/locale.hpp>
#include <boost/filesystem.hpp>
#endif

#ifdef HAVE_READLINE
  #include "readline_buffer.h"
  #define PAUSE_READLINE() \
    rdln::suspend_readline pause_readline; 
#else
  #define PAUSE_READLINE()
#endif

using namespace std;
using namespace epee;
using namespace cryptonote;
using boost::lexical_cast;
namespace po = boost::program_options;
typedef cryptonote::simple_wallet sw;

#undef ANTD_DEFAULT_LOG_CATEGORY
#define ANTD_DEFAULT_LOG_CATEGORY "wallet.simplewallet"

#define EXTENDED_LOGS_FILE "wallet_details.log"

#define OUTPUT_EXPORT_FILE_MAGIC "Antd output export\003"

#define LOCK_IDLE_SCOPE() \
  bool auto_refresh_enabled = m_auto_refresh_enabled.load(std::memory_order_relaxed); \
  m_auto_refresh_enabled.store(false, std::memory_order_relaxed); \
  /* stop any background refresh, and take over */ \
  m_wallet->stop(); \
  boost::unique_lock<boost::mutex> lock(m_idle_mutex); \
  m_idle_cond.notify_all(); \
  epee::misc_utils::auto_scope_leave_caller scope_exit_handler = epee::misc_utils::create_scope_leave_handler([&](){ \
    m_auto_refresh_enabled.store(auto_refresh_enabled, std::memory_order_relaxed); \
  })

#define SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(code) \
  LOCK_IDLE_SCOPE(); \
  boost::optional<tools::password_container> pwd_container = boost::none; \
  if (m_wallet->ask_password() && !(pwd_container = get_and_verify_password())) { code; } \
  tools::wallet_keys_unlocker unlocker(*m_wallet, pwd_container);

#define SCOPED_WALLET_UNLOCK() SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return true;)

#define PRINT_USAGE(usage_help) fail_msg_writer() << boost::format(tr("usage: %s")) % usage_help;

#define LONG_PAYMENT_ID_SUPPORT_CHECK() \
  do { \
    if (!m_long_payment_id_support) { \
      fail_msg_writer() << tr("Long payment IDs are obsolete. Use --long-payment-id-support if you really must use one, and warn the recipient they are using an obsolete feature that will disappear in the future."); \
      return true; \
    } \
  } while(0)

enum TransferType {
  Transfer,
  TransferLocked,
};

namespace
{
  const auto arg_wallet_file = wallet_args::arg_wallet_file();
  const command_line::arg_descriptor<std::string> arg_generate_new_wallet = {"generate-new-wallet", sw::tr("Generate new wallet and save it to <arg>"), ""};
  const command_line::arg_descriptor<std::string> arg_generate_from_device = {"generate-from-device", sw::tr("Generate new wallet from device and save it to <arg>"), ""};
  const command_line::arg_descriptor<std::string> arg_generate_from_view_key = {"generate-from-view-key", sw::tr("Generate incoming-only wallet from view key"), ""};
  const command_line::arg_descriptor<std::string> arg_generate_from_spend_key = {"generate-from-spend-key", sw::tr("Generate deterministic wallet from spend key"), ""};
  const command_line::arg_descriptor<std::string> arg_generate_from_keys = {"generate-from-keys", sw::tr("Generate wallet from private keys"), ""};
  const command_line::arg_descriptor<std::string> arg_generate_from_multisig_keys = {"generate-from-multisig-keys", sw::tr("Generate a master wallet from multisig wallet keys"), ""};
  const auto arg_generate_from_json = wallet_args::arg_generate_from_json();
  const command_line::arg_descriptor<std::string> arg_mnemonic_language = {"mnemonic-language", sw::tr("Language for mnemonic"), ""};
  const command_line::arg_descriptor<std::string> arg_electrum_seed = {"electrum-seed", sw::tr("Specify Electrum seed for wallet recovery/creation"), ""};
  const command_line::arg_descriptor<bool> arg_restore_deterministic_wallet = {"restore-deterministic-wallet", sw::tr("Recover wallet using Electrum-style mnemonic seed"), false};
  const command_line::arg_descriptor<bool> arg_restore_multisig_wallet = {"restore-multisig-wallet", sw::tr("Recover multisig wallet using Electrum-style mnemonic seed"), false};
  const command_line::arg_descriptor<bool> arg_non_deterministic = {"non-deterministic", sw::tr("Generate non-deterministic view and spend keys"), false};
  const command_line::arg_descriptor<bool> arg_allow_mismatched_daemon_version = {"allow-mismatched-daemon-version", sw::tr("Allow communicating with a daemon that uses a different RPC version"), false};
  const command_line::arg_descriptor<uint64_t> arg_restore_height = {"restore-height", sw::tr("Restore from specific blockchain height"), 0};
  const command_line::arg_descriptor<std::string> arg_restore_date = {"restore-date", sw::tr("Restore from estimated blockchain height on specified date"), ""};
  const command_line::arg_descriptor<bool> arg_do_not_relay = {"do-not-relay", sw::tr("The newly created transaction will not be relayed to the antd network"), false};
  const command_line::arg_descriptor<bool> arg_create_address_file = {"create-address-file", sw::tr("Create an address file for new wallets"), false};
  const command_line::arg_descriptor<std::string> arg_subaddress_lookahead = {"subaddress-lookahead", tools::wallet2::tr("Set subaddress lookahead sizes to <major>:<minor>"), ""};
  const command_line::arg_descriptor<bool> arg_use_english_language_names = {"use-english-language-names", sw::tr("Display English language names"), false};
  const command_line::arg_descriptor<bool> arg_long_payment_id_support = {"long-payment-id-support", sw::tr("Support obsolete long (unencrypted) payment ids"), false};
  const command_line::arg_descriptor< std::vector<std::string> > arg_command = {"command", ""};

  const char* USAGE_START_MINING("start_mining [<number_of_threads>] [bg_mining] [ignore_battery]");
  const char* USAGE_SET_DAEMON("set_daemon <host>[:<port>] [trusted|untrusted]");
  const char* USAGE_SHOW_BALANCE("balance [detail]");
  const char* USAGE_INCOMING_TRANSFERS("incoming_transfers [available|unavailable] [verbose] [uses] [index=<N1>[,<N2>[,...]]]");
  const char* USAGE_PAYMENTS("payments <PID_1> [<PID_2> ... <PID_N>]");
  const char* USAGE_PAYMENT_ID("payment_id");
  const char* USAGE_TRANSFER("transfer [index=<N1>[,<N2>,...]] [<priority>] (<URI> | <address> <amount>) [<payment_id>]");
  const char* USAGE_LOCKED_TRANSFER("locked_transfer [index=<N1>[,<N2>,...]] [<priority>] (<URI> | <addr> <amount>) <lockblocks> [<payment_id (obsolete)>]");
  const char* USAGE_LOCKED_SWEEP_ALL("locked_sweep_all [index=<N1>[,<N2>,...]] [<priority>] <address> <lockblocks> [<payment_id (obsolete)>]");
  const char* USAGE_SWEEP_ALL("sweep_all [index=<N1>[,<N2>,...]] [<priority>] [outputs=<N>] <address> [<payment_id (obsolete)>] [use_v1_tx]");
  const char* USAGE_SWEEP_BELOW("sweep_below <amount_threshold> [index=<N1>[,<N2>,...]] [<priority>] <address> [<payment_id (obsolete)>]");
  const char* USAGE_SWEEP_SINGLE("sweep_single [<priority>] [outputs=<N>] <key_image> <address> [<payment_id (obsolete)>]");
  const char* USAGE_SIGN_TRANSFER("sign_transfer [export_raw]");
  const char* USAGE_SET_LOG("set_log <level>|{+,-,}<categories>");
  const char* USAGE_ACCOUNT("account\n"
                            "  account new <label text with white spaces allowed>\n"
                            "  account switch <index> \n"
                            "  account label <index> <label text with white spaces allowed>\n"
                            "  account tag <tag_name> <account_index_1> [<account_index_2> ...]\n"
                            "  account untag <account_index_1> [<account_index_2> ...]\n"
                            "  account tag_description <tag_name> <description>");
  const char* USAGE_ADDRESS("address [ new <label text with white spaces allowed> | all | <index_min> [<index_max>] | label <index> <label text with white spaces allowed>]");
  const char* USAGE_INTEGRATED_ADDRESS("integrated_address [<payment_id> | <address>]");
  const char* USAGE_ADDRESS_BOOK("address_book [(add ((<address> [pid <id>])|<integrated address>) [<description possibly with whitespaces>])|(delete <index>)]");
  const char* USAGE_SET_VARIABLE("set <option> [<value>]");
  const char* USAGE_GET_TX_KEY("get_tx_key <txid>");
  const char* USAGE_SET_TX_KEY("set_tx_key <txid> <tx_key>");
  const char* USAGE_CHECK_TX_KEY("check_tx_key <txid> <txkey> <address>");
  const char* USAGE_GET_TX_PROOF("get_tx_proof <txid> <address> [<message>]");
  const char* USAGE_CHECK_TX_PROOF("check_tx_proof <txid> <address> <signature_file> [<message>]");
  const char* USAGE_GET_SPEND_PROOF("get_spend_proof <txid> [<message>]");
  const char* USAGE_CHECK_SPEND_PROOF("check_spend_proof <txid> <signature_file> [<message>]");
  const char* USAGE_GET_RESERVE_PROOF("get_reserve_proof (all|<amount>) [<message>]");
  const char* USAGE_CHECK_RESERVE_PROOF("check_reserve_proof <address> <signature_file> [<message>]");
  const char* USAGE_SHOW_TRANSFERS("show_transfers [in|out|all|pending|failed|coinbase] [index=<N1>[,<N2>,...]] [<min_height> [<max_height>]]");
  const char* USAGE_EXPORT_TRANSFERS("export_transfers [in|out|all|pending|failed] [index=<N1>[,<N2>,...]] [<min_height> [<max_height>]] [output=<path>]");
  const char* USAGE_UNSPENT_OUTPUTS("unspent_outputs [index=<N1>[,<N2>,...]] [<min_amount> [<max_amount>]]");
  const char* USAGE_RESCAN_BC("rescan_bc [hard]");
  const char* USAGE_SET_TX_NOTE("set_tx_note <txid> [free text note]");
  const char* USAGE_GET_TX_NOTE("get_tx_note <txid>");
  const char* USAGE_GET_DESCRIPTION("get_description");
  const char* USAGE_SET_DESCRIPTION("set_description [free text note]");
  const char* USAGE_SIGN("sign <filename>");
  const char* USAGE_VERIFY("verify <filename> <address> <signature>");
  const char* USAGE_EXPORT_KEY_IMAGES("export_key_images <filename> [requested-only]");
  const char* USAGE_IMPORT_KEY_IMAGES("import_key_images <filename>");
  const char* USAGE_HW_KEY_IMAGES_SYNC("hw_key_images_sync");
  const char* USAGE_HW_RECONNECT("hw_reconnect");
  const char* USAGE_EXPORT_OUTPUTS("export_outputs <filename>");
  const char* USAGE_IMPORT_OUTPUTS("import_outputs <filename>");
  const char* USAGE_SHOW_TRANSFER("show_transfer <txid>");
  const char* USAGE_MAKE_MULTISIG("make_multisig <threshold> <string1> [<string>...]");
  const char* USAGE_FINALIZE_MULTISIG("finalize_multisig <string> [<string>...]");
  const char* USAGE_EXCHANGE_MULTISIG_KEYS("exchange_multisig_keys <string> [<string>...]");
  const char* USAGE_EXPORT_MULTISIG_INFO("export_multisig_info <filename>");
  const char* USAGE_IMPORT_MULTISIG_INFO("import_multisig_info <filename> [<filename>...]");
  const char* USAGE_SIGN_MULTISIG("sign_multisig <filename>");
  const char* USAGE_SUBMIT_MULTISIG("submit_multisig <filename>");
  const char* USAGE_EXPORT_RAW_MULTISIG_TX("export_raw_multisig_tx <filename>");
  const char* USAGE_MMS("mms [<subcommand> [<subcommand_parameters>]]");
  const char* USAGE_MMS_INIT("mms init <required_signers>/<authorized_signers> <own_label> <own_transport_address>");
  const char* USAGE_MMS_INFO("mms info");
  const char* USAGE_MMS_SIGNER("mms signer [<number> <label> [<transport_address> [<antd_address>]]]");
  const char* USAGE_MMS_LIST("mms list");
  const char* USAGE_MMS_NEXT("mms next [sync]");
  const char* USAGE_MMS_SYNC("mms sync");
  const char* USAGE_MMS_TRANSFER("mms transfer <transfer_command_arguments>");
  const char* USAGE_MMS_DELETE("mms delete (<message_id> | all)");
  const char* USAGE_MMS_SEND("mms send [<message_id>]");
  const char* USAGE_MMS_RECEIVE("mms receive");
  const char* USAGE_MMS_EXPORT("mms export <message_id>");
  const char* USAGE_MMS_NOTE("mms note [<label> <text>]");
  const char* USAGE_MMS_SHOW("mms show <message_id>");
  const char* USAGE_MMS_SET("mms set <option_name> [<option_value>]");
  const char* USAGE_MMS_SEND_SIGNER_CONFIG("mms send_signer_config");
  const char* USAGE_MMS_START_AUTO_CONFIG("mms start_auto_config [<label> <label> ...]");
  const char* USAGE_MMS_STOP_AUTO_CONFIG("mms stop_auto_config");
  const char* USAGE_MMS_AUTO_CONFIG("mms auto_config <auto_config_token>");
  const char* USAGE_PRINT_RING("print_ring <key_image> | <txid>");
  const char* USAGE_SET_RING("set_ring <filename> | ( <key_image> absolute|relative <index> [<index>...] )");
  const char* USAGE_SAVE_KNOWN_RINGS("save_known_rings");
  const char* USAGE_MARK_OUTPUT_SPENT("mark_output_spent <amount>/<offset> | <filename> [add]");
  const char* USAGE_MARK_OUTPUT_UNSPENT("mark_output_unspent <amount>/<offset>");
  const char* USAGE_IS_OUTPUT_SPENT("is_output_spent <amount>/<offset>");
  const char* USAGE_VERSION("version");
  const char* USAGE_HELP("help [<command>]");

  //
  // Antd
  //
  const char* USAGE_REGISTER_FULL_NODE("register_full_node [index=<N1>[,<N2>,...]] [priority] <operator cut> <address1> <fraction1> [<address2> <fraction2> [...]] <expiration timestamp> <pubkey> <signature>");
  const char* USAGE_STAKE("stake [index=<N1>[,<N2>,...]] [priority] <fullnode pubkey> <amount|percent%>");
  const char* USAGE_REQUEST_STAKE_UNLOCK("request_stake_unlock <full_node_pubkey>");
  const char* USAGE_PRINT_LOCKED_STAKES("print_locked_stakes");
  const char* USAGE_ADD_ARTICLE("add_article <title>  <content> <pulish> 44Affq... 0.001");

#if defined (ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
  std::string input_line(const std::string& prompt, bool yesno = false)
  {
    std::string buf;
    if (yesno) std::cout << prompt << " (Y/Yes/N/No): ";
    else       std::cout << prompt << ": ";
    antd::write_redirected_stdout_to_shared_mem();
    antd::fixed_buffer buffer = antd::read_from_stdin_shared_mem();
    buf.reserve(buffer.len);
    buf = buffer.data;
    return epee::string_tools::trim(buf);
  }
#else // ANTD_ENABLE_INTEGRATION_TEST_HOOKS
  std::string input_line(const std::string& prompt, bool yesno = false)
  {
    std::string buf;

#ifdef HAVE_READLINE
    rdln::suspend_readline pause_readline;
#endif
    std::cout << prompt;
    if (yesno)
      std::cout << " (Y/Yes/N/No)";
    std::cout << ": " << std::flush;

#ifdef _WIN32
    buf = tools::input_line_win();
#else
    std::getline(std::cin, buf);
#endif

    return epee::string_tools::trim(buf);
  }
#endif // ANTD_ENABLE_INTEGRATION_TEST_HOOKS

  epee::wipeable_string input_secure_line(const char *prompt)
  {
#if defined (ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    std::cout << prompt;
    antd::write_redirected_stdout_to_shared_mem();
    antd::fixed_buffer buffer = antd::read_from_stdin_shared_mem();
    epee::wipeable_string buf = buffer.data;
#else

#ifdef HAVE_READLINE
    rdln::suspend_readline pause_readline;
#endif
    auto pwd_container = tools::password_container::prompt(false, prompt, false);
    if (!pwd_container)
    {
      MERROR("Failed to read secure line");
      return "";
    }

    epee::wipeable_string buf = pwd_container->password();

    buf.trim();
#endif
    return buf;
  }

  boost::optional<tools::password_container> password_prompter(const char *prompt, bool verify)
  {
#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    std::cout << prompt << ": NOTE(antd): Passwords not supported, defaulting to empty password";
    antd::write_redirected_stdout_to_shared_mem();
    tools::password_container pwd_container(std::string(""));
#else
  #ifdef HAVE_READLINE
    rdln::suspend_readline pause_readline;
  #endif
    auto pwd_container = tools::password_container::prompt(verify, prompt);
    if (!pwd_container)
    {
      tools::fail_msg_writer() << sw::tr("failed to read wallet password");
    }
#endif
    return pwd_container;
  }

  boost::optional<tools::password_container> default_password_prompter(bool verify)
  {
    return password_prompter(verify ? sw::tr("Enter a new password for the wallet") : sw::tr("Wallet password"), verify);
  }

  inline std::string interpret_rpc_response(bool ok, const std::string& status)
  {
    std::string err;
    if (ok)
    {
      if (status == CORE_RPC_STATUS_BUSY)
      {
        err = sw::tr("daemon is busy. Please try again later.");
      }
      else if (status != CORE_RPC_STATUS_OK)
      {
        err = status;
      }
    }
    else
    {
      err = sw::tr("possibly lost connection to daemon");
    }
    return err;
  }

  tools::scoped_message_writer success_msg_writer(bool color = false)
  {
    return tools::scoped_message_writer(color ? console_color_green : console_color_default, false, std::string(), el::Level::Info);
  }

  tools::scoped_message_writer message_writer(epee::console_colors color = epee::console_color_default, bool bright = false)
  {
    return tools::scoped_message_writer(color, bright);
  }

  tools::scoped_message_writer fail_msg_writer()
  {
    return tools::scoped_message_writer(console_color_red, true, sw::tr("Error: "), el::Level::Error);
  }

  bool parse_bool(const std::string& s, bool& result)
  {
    if (s == "1" || command_line::is_yes(s))
    {
      result = true;
      return true;
    }
    if (s == "0" || command_line::is_no(s))
    {
      result = false;
      return true;
    }

    boost::algorithm::is_iequal ignore_case{};
    if (boost::algorithm::equals("true", s, ignore_case) || boost::algorithm::equals(simple_wallet::tr("true"), s, ignore_case))
    {
      result = true;
      return true;
    }
    if (boost::algorithm::equals("false", s, ignore_case) || boost::algorithm::equals(simple_wallet::tr("false"), s, ignore_case))
    {
      result = false;
      return true;
    }

    return false;
  }

  template <typename F>
  bool parse_bool_and_use(const std::string& s, F func)
  {
    bool r;
    if (parse_bool(s, r))
    {
      func(r);
      return true;
    }
    else
    {
      fail_msg_writer() << sw::tr("invalid argument: must be either 0/1, true/false, y/n, yes/no");
      return false;
    }
  }

  const struct
  {
    const char *name;
    tools::wallet2::RefreshType refresh_type;
  } refresh_type_names[] =
  {
    { "full", tools::wallet2::RefreshFull },
    { "optimize-coinbase", tools::wallet2::RefreshOptimizeCoinbase },
    { "optimized-coinbase", tools::wallet2::RefreshOptimizeCoinbase },
    { "no-coinbase", tools::wallet2::RefreshNoCoinbase },
    { "default", tools::wallet2::RefreshDefault },
  };

  bool parse_refresh_type(const std::string &s, tools::wallet2::RefreshType &refresh_type)
  {
    for (size_t n = 0; n < sizeof(refresh_type_names) / sizeof(refresh_type_names[0]); ++n)
    {
      if (s == refresh_type_names[n].name)
      {
        refresh_type = refresh_type_names[n].refresh_type;
        return true;
      }
    }
    fail_msg_writer() << cryptonote::simple_wallet::tr("failed to parse refresh type");
    return false;
  }

  std::string get_refresh_type_name(tools::wallet2::RefreshType type)
  {
    for (size_t n = 0; n < sizeof(refresh_type_names) / sizeof(refresh_type_names[0]); ++n)
    {
      if (type == refresh_type_names[n].refresh_type)
        return refresh_type_names[n].name;
    }
    return "invalid";
  }

  std::string get_version_string(uint32_t version)
  {
    return boost::lexical_cast<std::string>(version >> 16) + "." + boost::lexical_cast<std::string>(version & 0xffff);
  }

  std::string oa_prompter(const std::string &url, const std::vector<std::string> &addresses, bool dnssec_valid)
  {
    if (addresses.empty())
      return {};
    // prompt user for confirmation.
    // inform user of DNSSEC validation status as well.
    std::string dnssec_str;
    if (dnssec_valid)
    {
      dnssec_str = sw::tr("DNSSEC validation passed");
    }
    else
    {
      dnssec_str = sw::tr("WARNING: DNSSEC validation was unsuccessful, this address may not be correct!");
    }
    std::stringstream prompt;
    prompt << sw::tr("For URL: ") << url
           << ", " << dnssec_str << std::endl
           << sw::tr(" Antd Address = ") << addresses[0]
           << std::endl
           << sw::tr("Is this OK?")
    ;
    // prompt the user for confirmation given the dns query and dnssec status
    std::string confirm_dns_ok = input_line(prompt.str(), true);
    if (std::cin.eof())
    {
      return {};
    }
    if (!command_line::is_yes(confirm_dns_ok))
    {
      std::cout << sw::tr("you have cancelled the transfer request") << std::endl;
      return {};
    }
    return addresses[0];
  }

  boost::optional<std::pair<uint32_t, uint32_t>> parse_subaddress_lookahead(const std::string& str)
  {
    auto r = tools::parse_subaddress_lookahead(str);
    if (!r)
      fail_msg_writer() << sw::tr("invalid format for subaddress lookahead; must be <major>:<minor>");
    return r;
  }

  void handle_transfer_exception(const std::exception_ptr &e, bool trusted_daemon)
  {
    bool warn_of_possible_attack = !trusted_daemon;
    try
    {
      std::rethrow_exception(e);
    }
    catch (const tools::error::daemon_busy&)
    {
      fail_msg_writer() << sw::tr("daemon is busy. Please try again later.");
    }
    catch (const tools::error::no_connection_to_daemon&)
    {
      fail_msg_writer() << sw::tr("no connection to daemon. Please make sure daemon is running.");
    }
    catch (const tools::error::wallet_rpc_error& e)
    {
      LOG_ERROR("RPC error: " << e.to_string());
      fail_msg_writer() << sw::tr("RPC error: ") << e.what();
    }
    catch (const tools::error::get_outs_error &e)
    {
      fail_msg_writer() << sw::tr("failed to get random outputs to mix: ") << e.what();
    }
    catch (const tools::error::not_enough_unlocked_money& e)
    {
      LOG_PRINT_L0(boost::format("not enough money to transfer, available only %s, sent amount %s") %
        print_money(e.available()) %
        print_money(e.tx_amount()));
      fail_msg_writer() << sw::tr("Not enough money in unlocked balance");
      warn_of_possible_attack = false;
    }
    catch (const tools::error::not_enough_money& e)
    {
      LOG_PRINT_L0(boost::format("not enough money to transfer, available only %s, sent amount %s") %
        print_money(e.available()) %
        print_money(e.tx_amount()));
      fail_msg_writer() << sw::tr("Not enough money in unlocked balance");
      warn_of_possible_attack = false;
    }
    catch (const tools::error::tx_not_possible& e)
    {
      LOG_PRINT_L0(boost::format("not enough money to transfer, available only %s, transaction amount %s = %s + %s (fee)") %
        print_money(e.available()) %
        print_money(e.tx_amount() + e.fee())  %
        print_money(e.tx_amount()) %
        print_money(e.fee()));
      fail_msg_writer() << sw::tr("Failed to find a way to create transactions. This is usually due to dust which is so small it cannot pay for itself in fees, or trying to send more money than the unlocked balance, or not leaving enough for fees");
      warn_of_possible_attack = false;
    }
    catch (const tools::error::not_enough_outs_to_mix& e)
    {
      auto writer = fail_msg_writer();
      writer << sw::tr("not enough outputs for specified ring size") << " = " << (e.mixin_count() + 1) << ":";
      for (std::pair<uint64_t, uint64_t> outs_for_amount : e.scanty_outs())
      {
        writer << "\n" << sw::tr("output amount") << " = " << print_money(outs_for_amount.first) << ", " << sw::tr("found outputs to use") << " = " << outs_for_amount.second;
      }
      writer << "\n" << sw::tr("Please use sweep_unmixable.");
    }
    catch (const tools::error::tx_not_constructed&)
    {
      fail_msg_writer() << sw::tr("transaction was not constructed");
      warn_of_possible_attack = false;
    }
    catch (const tools::error::tx_rejected& e)
    {
      fail_msg_writer() << (boost::format(sw::tr("transaction %s was rejected by daemon")) % get_transaction_hash(e.tx()));
      std::string reason = e.reason();
      if (!reason.empty())
        fail_msg_writer() << sw::tr("Reason: ") << reason;
    }
    catch (const tools::error::tx_sum_overflow& e)
    {
      fail_msg_writer() << e.what();
      warn_of_possible_attack = false;
    }
    catch (const tools::error::zero_destination&)
    {
      fail_msg_writer() << sw::tr("one of destinations is zero");
      warn_of_possible_attack = false;
    }
    catch (const tools::error::tx_too_big& e)
    {
      fail_msg_writer() << sw::tr("failed to find a suitable way to split transactions");
      warn_of_possible_attack = false;
    }
    catch (const tools::error::transfer_error& e)
    {
      LOG_ERROR("unknown transfer error: " << e.to_string());
      fail_msg_writer() << sw::tr("unknown transfer error: ") << e.what();
    }
    catch (const tools::error::multisig_export_needed& e)
    {
      LOG_ERROR("Multisig error: " << e.to_string());
      fail_msg_writer() << sw::tr("Multisig error: ") << e.what();
      warn_of_possible_attack = false;
    }
    catch (const tools::error::wallet_internal_error& e)
    {
      LOG_ERROR("internal error: " << e.to_string());
      fail_msg_writer() << sw::tr("internal error: ") << e.what();
    }
    catch (const std::exception& e)
    {
      LOG_ERROR("unexpected error: " << e.what());
      fail_msg_writer() << sw::tr("unexpected error: ") << e.what();
    }

    if (warn_of_possible_attack)
      fail_msg_writer() << sw::tr("There was an error, which could mean the node may be trying to get you to retry creating a transaction, and zero in on which outputs you own. Or it could be a bona fide error. It may be prudent to disconnect from this node, and not try to send a transaction immediately. Alternatively, connect to another node so the original node cannot correlate information.");
  }

  bool check_file_overwrite(const std::string &filename)
  {
    boost::system::error_code errcode;
    if (boost::filesystem::exists(filename, errcode))
    {
      if (boost::ends_with(filename, ".keys"))
      {
        fail_msg_writer() << boost::format(sw::tr("File %s likely stores wallet private keys! Use a different file name.")) % filename;
        return false;
      }
      return command_line::is_yes(input_line((boost::format(sw::tr("File %s already exists. Are you sure to overwrite it?")) % filename).str(), true));
    }
    return true;
  }

  void print_secret_key(const crypto::secret_key &k)
  {
    static constexpr const char hex[] = u8"0123456789abcdef";
    const uint8_t *ptr = (const uint8_t*)k.data;
    for (size_t i = 0, sz = sizeof(k); i < sz; ++i)
    {
      putchar(hex[*ptr >> 4]);
      putchar(hex[*ptr & 15]);
      ++ptr;
    }
  }
}

std::string join_priority_strings(const char *delimiter)
{
  std::string s;
  for (size_t n = 0; n < tools::allowed_priority_strings.size(); ++n)
  {
    if (!s.empty())
      s += delimiter;
    s += tools::allowed_priority_strings[n];
  }
  return s;
}

std::string simple_wallet::get_commands_str()
{
  std::stringstream ss;
  ss << tr("Commands: ") << ENDL;
  std::string usage = m_cmd_binder.get_usage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}

std::string simple_wallet::get_command_usage(const std::vector<std::string> &args)
{
  std::pair<std::string, std::string> documentation = m_cmd_binder.get_documentation(args);
  std::stringstream ss;
  if(documentation.first.empty())
  {
    ss << tr("Unknown command: ") << args.front();
  }
  else
  {
    std::string usage = documentation.second.empty() ? args.front() : documentation.first;
    std::string description = documentation.second.empty() ? documentation.first : documentation.second;
    usage.insert(0, "  ");
    ss << tr("Command usage: ") << ENDL << usage << ENDL << ENDL;
    boost::replace_all(description, "\n", "\n  ");
    description.insert(0, "  ");
    ss << tr("Command description: ") << ENDL << description << ENDL;
  }
  return ss.str();
}

bool simple_wallet::viewkey(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  // don't log
  PAUSE_READLINE();
  if (m_wallet->key_on_device()) {
    std::cout << "secret: On device. Not available" << std::endl;
  } else {
    SCOPED_WALLET_UNLOCK();
    printf("secret: ");
    print_secret_key(m_wallet->get_account().get_keys().m_view_secret_key);
    putchar('\n');
  }
  std::cout << "public: " << string_tools::pod_to_hex(m_wallet->get_account().get_keys().m_account_address.m_view_public_key) << std::endl;

  return true;
}

bool simple_wallet::spendkey(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and has no spend key");
    return true;
  }
  // don't log
  PAUSE_READLINE();
  if (m_wallet->key_on_device()) {
    std::cout << "secret: On device. Not available" << std::endl;
  } else {
    SCOPED_WALLET_UNLOCK();
    printf("secret: ");
    print_secret_key(m_wallet->get_account().get_keys().m_spend_secret_key);
    putchar('\n');
  }
  std::cout << "public: " << string_tools::pod_to_hex(m_wallet->get_account().get_keys().m_account_address.m_spend_public_key) << std::endl;

  return true;
}

bool simple_wallet::print_seed(bool encrypted)
{
  bool success =  false;
  epee::wipeable_string seed;
  bool ready, multisig;

  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and has no seed");
    return true;
  }

  multisig = m_wallet->multisig(&ready);
  if (multisig)
  {
    if (!ready)
    {
      fail_msg_writer() << tr("wallet is multisig but not yet finalized");
      return true;
    }
  }

  SCOPED_WALLET_UNLOCK();

  if (!multisig && !m_wallet->is_deterministic())
  {
    fail_msg_writer() << tr("wallet is non-deterministic and has no seed");
    return true;
  }

  epee::wipeable_string seed_pass;
  if (encrypted)
  {
    auto pwd_container = password_prompter(tr("Enter optional seed offset passphrase, empty to see raw seed"), true);
    if (std::cin.eof() || !pwd_container)
      return true;
    seed_pass = pwd_container->password();
  }

  if (multisig)
    success = m_wallet->get_multisig_seed(seed, seed_pass);
  else if (m_wallet->is_deterministic())
    success = m_wallet->get_seed(seed, seed_pass);

  if (success) 
  {
    print_seed(seed);
  }
  else
  {
    fail_msg_writer() << tr("Failed to retrieve seed");
  }
  return true;
}

bool simple_wallet::seed(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  return print_seed(false);
}

bool simple_wallet::encrypted_seed(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  return print_seed(true);
}

bool simple_wallet::seed_set_language(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (m_wallet->multisig())
  {
    fail_msg_writer() << tr("wallet is multisig and has no seed");
    return true;
  }
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and has no seed");
    return true;
  }

  epee::wipeable_string password;
  {
    SCOPED_WALLET_UNLOCK();

    if (!m_wallet->is_deterministic())
    {
      fail_msg_writer() << tr("wallet is non-deterministic and has no seed");
      return true;
    }

    // we need the password, even if ask-password is unset
    if (!pwd_container)
    {
      pwd_container = get_and_verify_password();
      if (pwd_container == boost::none)
      {
        fail_msg_writer() << tr("Incorrect password");
        return true;
      }
    }
    password = pwd_container->password();
  }

  std::string mnemonic_language = get_mnemonic_language();
  if (mnemonic_language.empty())
    return true;

  m_wallet->set_seed_language(std::move(mnemonic_language));
  m_wallet->rewrite(m_wallet_file, password);
  return true;
}

bool simple_wallet::change_password(const std::vector<std::string> &args)
{ 
  const auto orig_pwd_container = get_and_verify_password();

  if(orig_pwd_container == boost::none)
  {
    fail_msg_writer() << tr("Your original password was incorrect.");
    return true;
  }

  // prompts for a new password, pass true to verify the password
  const auto pwd_container = default_password_prompter(true);
  if(!pwd_container)
    return true;

  try
  {
    m_wallet->change_password(m_wallet_file, orig_pwd_container->password(), pwd_container->password());
  }
  catch (const tools::error::wallet_logic_error& e)
  {
    fail_msg_writer() << tr("Error with wallet rewrite: ") << e.what();
    return true;
  }

  return true;
}

bool simple_wallet::payment_id(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  LONG_PAYMENT_ID_SUPPORT_CHECK();

  crypto::hash payment_id;
  if (args.size() > 0)
  {
    PRINT_USAGE(USAGE_PAYMENT_ID);
    return true;
  }
  payment_id = crypto::rand<crypto::hash>();
  success_msg_writer() << tr("Random payment ID: ") << payment_id;
  return true;
}

bool simple_wallet::print_fee_info(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if (!try_connect_to_daemon())
    return true;
  const bool per_byte = m_wallet->use_fork_rules(HF_VERSION_PER_BYTE_FEE);
  const uint64_t base_fee = m_wallet->get_base_fee();
  const char *base = per_byte ? "byte" : "kB";
  const uint64_t typical_size = per_byte ? 2500 : 13;
  const uint64_t size_granularity = per_byte ? 1 : 1024;
  message_writer() << (boost::format(tr("Current fee is %s %s per %s")) % print_money(base_fee) % cryptonote::get_unit(cryptonote::get_default_decimal_point()) % base).str();

  std::vector<uint64_t> fees;
  for (uint32_t priority = 1; priority <= 4; ++priority)
  {
    uint64_t mult = m_wallet->get_fee_multiplier(priority);
    fees.push_back(base_fee * typical_size * mult);
  }
  std::vector<std::pair<uint64_t, uint64_t>> blocks;
  try
  {
    uint64_t base_size = typical_size * size_granularity;
    blocks = m_wallet->estimate_backlog(base_size, base_size + size_granularity - 1, fees);
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Error: failed to estimate backlog array size: ") << e.what();
    return true;
  }
  if (blocks.size() != 4)
  {
    fail_msg_writer() << tr("Error: bad estimated backlog array size");
    return true;
  }

  for (uint32_t priority = 1; priority <= 4; ++priority)
  {
    uint64_t nblocks_low = blocks[priority - 1].first;
    uint64_t nblocks_high = blocks[priority - 1].second;
    if (nblocks_low > 0)
    {
      std::string msg;
      if (priority == m_wallet->get_default_priority() || (m_wallet->get_default_priority() == 0 && priority == 2))
        msg = tr(" (current)");
      uint64_t minutes_low = nblocks_low * DIFFICULTY_TARGET_V2 / 60, minutes_high = nblocks_high * DIFFICULTY_TARGET_V2 / 60;
      if (nblocks_high == nblocks_low)
        message_writer() << (boost::format(tr("%u block (%u minutes) backlog at priority %u%s")) % nblocks_low % minutes_low % priority % msg).str();
      else
        message_writer() << (boost::format(tr("%u to %u block (%u to %u minutes) backlog at priority %u")) % nblocks_low % nblocks_high % minutes_low % minutes_high % priority).str();
    }
    else
      message_writer() << tr("No backlog at priority ") << priority;
  }
  return true;
}

bool simple_wallet::prepare_multisig(const std::vector<std::string> &args)
{
  prepare_multisig_main(args, false);
  return true;
}

bool simple_wallet::prepare_multisig_main(const std::vector<std::string> &args, bool called_by_mms)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return false;
  }
  if (m_wallet->multisig())
  {
    fail_msg_writer() << tr("This wallet is already multisig");
    return false;
  }
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and cannot be made multisig");
    return false;
  }

  if(m_wallet->get_num_transfer_details())
  {
    fail_msg_writer() << tr("This wallet has been used before, please use a new wallet to create a multisig wallet");
    return false;
  }

  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return false;);

  std::string multisig_info = m_wallet->get_multisig_info();
  success_msg_writer() << multisig_info;
  success_msg_writer() << tr("Send this multisig info to all other participants, then use make_multisig <threshold> <info1> [<info2>...] with others' multisig info");
  success_msg_writer() << tr("This includes the PRIVATE view key, so needs to be disclosed only to that multisig wallet's participants ");

  if (called_by_mms)
  {
    get_message_store().process_wallet_created_data(get_multisig_wallet_state(), mms::message_type::key_set, multisig_info);
  }

  return true;
}

bool simple_wallet::make_multisig(const std::vector<std::string> &args)
{
  make_multisig_main(args, false);
  return true;
}

bool simple_wallet::make_multisig_main(const std::vector<std::string> &args, bool called_by_mms)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return false;
  }
  if (m_wallet->multisig())
  {
    fail_msg_writer() << tr("This wallet is already multisig");
    return false;
  }
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and cannot be made multisig");
    return false;
  }

  if(m_wallet->get_num_transfer_details())
  {
    fail_msg_writer() << tr("This wallet has been used before, please use a new wallet to create a multisig wallet");
    return false;
  }

  if (args.size() < 2)
  {
    PRINT_USAGE(USAGE_MAKE_MULTISIG);
    return false;
  }

  // parse threshold
  uint32_t threshold;
  if (!string_tools::get_xtype_from_string(threshold, args[0]))
  {
    fail_msg_writer() << tr("Invalid threshold");
    return false;
  }

  const auto orig_pwd_container = get_and_verify_password();
  if(orig_pwd_container == boost::none)
  {
    fail_msg_writer() << tr("Your original password was incorrect.");
    return false;
  }

  LOCK_IDLE_SCOPE();

  try
  {
    auto local_args = args;
    local_args.erase(local_args.begin());
    std::string multisig_extra_info = m_wallet->make_multisig(orig_pwd_container->password(), local_args, threshold);
    if (!multisig_extra_info.empty())
    {
      success_msg_writer() << tr("Another step is needed");
      success_msg_writer() << multisig_extra_info;
      success_msg_writer() << tr("Send this multisig info to all other participants, then use exchange_multisig_keys <info1> [<info2>...] with others' multisig info");
      if (called_by_mms)
      {
        get_message_store().process_wallet_created_data(get_multisig_wallet_state(), mms::message_type::additional_key_set, multisig_extra_info);
      }
      return true;
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Error creating multisig: ") << e.what();
    return false;
  }

  uint32_t total;
  if (!m_wallet->multisig(NULL, &threshold, &total))
  {
    fail_msg_writer() << tr("Error creating multisig: new wallet is not multisig");
    return false;
  }
  success_msg_writer() << std::to_string(threshold) << "/" << total << tr(" multisig address: ")
      << m_wallet->get_account().get_public_address_str(m_wallet->nettype());

  return true;
}

bool simple_wallet::finalize_multisig(const std::vector<std::string> &args)
{
  bool ready;
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }

  const auto pwd_container = get_and_verify_password();
  if(pwd_container == boost::none)
  {
    fail_msg_writer() << tr("Your original password was incorrect.");
    return true;
  }

  if (!m_wallet->multisig(&ready))
  {
    fail_msg_writer() << tr("This wallet is not multisig");
    return true;
  }
  if (ready)
  {
    fail_msg_writer() << tr("This wallet is already finalized");
    return true;
  }

  LOCK_IDLE_SCOPE();

  if (args.size() < 2)
  {
    PRINT_USAGE(USAGE_FINALIZE_MULTISIG);
    return true;
  }

  try
  {
    if (!m_wallet->finalize_multisig(pwd_container->password(), args))
    {
      fail_msg_writer() << tr("Failed to finalize multisig");
      return true;
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to finalize multisig: ") << e.what();
    return true;
  }

  return true;
}

bool simple_wallet::exchange_multisig_keys(const std::vector<std::string> &args)
{
  exchange_multisig_keys_main(args, false);
  return true;
}

bool simple_wallet::exchange_multisig_keys_main(const std::vector<std::string> &args, bool called_by_mms) {
    bool ready;
    if (m_wallet->key_on_device())
    {
      fail_msg_writer() << tr("command not supported by HW wallet");
      return false;
    }
    if (!m_wallet->multisig(&ready))
    {
      fail_msg_writer() << tr("This wallet is not multisig");
      return false;
    }
    if (ready)
    {
      fail_msg_writer() << tr("This wallet is already finalized");
      return false;
    }

    const auto orig_pwd_container = get_and_verify_password();
    if(orig_pwd_container == boost::none)
    {
      fail_msg_writer() << tr("Your original password was incorrect.");
      return false;
    }

    if (args.size() < 2)
    {
      PRINT_USAGE(USAGE_EXCHANGE_MULTISIG_KEYS);
      return false;
    }

    try
    {
      std::string multisig_extra_info = m_wallet->exchange_multisig_keys(orig_pwd_container->password(), args);
      if (!multisig_extra_info.empty())
      {
        message_writer() << tr("Another step is needed");
        message_writer() << multisig_extra_info;
        message_writer() << tr("Send this multisig info to all other participants, then use exchange_multisig_keys <info1> [<info2>...] with others' multisig info");
        if (called_by_mms)
        {
          get_message_store().process_wallet_created_data(get_multisig_wallet_state(), mms::message_type::additional_key_set, multisig_extra_info);
        }
        return true;
      } else {
        uint32_t threshold, total;
        m_wallet->multisig(NULL, &threshold, &total);
        success_msg_writer() << tr("Multisig wallet has been successfully created. Current wallet type: ") << threshold << "/" << total;
        success_msg_writer() << tr("Multisig address: ") << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
      }
    }
    catch (const std::exception &e)
    {
      fail_msg_writer() << tr("Failed to perform multisig keys exchange: ") << e.what();
      return false;
    }

    return true;
}

bool simple_wallet::export_multisig(const std::vector<std::string> &args)
{
  export_multisig_main(args, false);
  return true;
}

bool simple_wallet::export_multisig_main(const std::vector<std::string> &args, bool called_by_mms)
{
  bool ready;
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return false;
  }
  if (!m_wallet->multisig(&ready))
  {
    fail_msg_writer() << tr("This wallet is not multisig");
    return false;
  }
  if (!ready)
  {
    fail_msg_writer() << tr("This multisig wallet is not yet finalized");
    return false;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_EXPORT_MULTISIG_INFO);
    return false;
  }

  const std::string filename = args[0];
  if (!called_by_mms && m_wallet->confirm_export_overwrite() && !check_file_overwrite(filename))
    return true;

  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return false;);

  try
  {
    cryptonote::blobdata ciphertext = m_wallet->export_multisig();

    if (called_by_mms)
    {
      get_message_store().process_wallet_created_data(get_multisig_wallet_state(), mms::message_type::multisig_sync_data, ciphertext);
    }
    else
    {
      bool r = epee::file_io_utils::save_string_to_file(filename, ciphertext);
      if (!r)
      {
        fail_msg_writer() << tr("failed to save file ") << filename;
        return false;
      }
    }
  }
  catch (const std::exception &e)
  {
    LOG_ERROR("Error exporting multisig info: " << e.what());
    fail_msg_writer() << tr("Error exporting multisig info: ") << e.what();
    return false;
  }

  success_msg_writer() << tr("Multisig info exported to ") << filename;
  return true;
}

bool simple_wallet::import_multisig(const std::vector<std::string> &args)
{
  import_multisig_main(args, false);
  return true;
}

bool simple_wallet::import_multisig_main(const std::vector<std::string> &args, bool called_by_mms)
{
  bool ready;
  uint32_t threshold, total;
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return false;
  }
  if (!m_wallet->multisig(&ready, &threshold, &total))
  {
    fail_msg_writer() << tr("This wallet is not multisig");
    return false;
  }
  if (!ready)
  {
    fail_msg_writer() << tr("This multisig wallet is not yet finalized");
    return false;
  }
  if (args.size() < threshold - 1)
  {
    PRINT_USAGE(USAGE_IMPORT_MULTISIG_INFO);
    return false;
  }

  std::vector<cryptonote::blobdata> info;
  for (size_t n = 0; n < args.size(); ++n)
  {
    if (called_by_mms)
    {
      info.push_back(args[n]);
    }
    else
    {
      const std::string &filename = args[n];
      std::string data;
      bool r = epee::file_io_utils::load_file_to_string(filename, data);
      if (!r)
      {
        fail_msg_writer() << tr("failed to read file ") << filename;
        return false;
      }
      info.push_back(std::move(data));
    }
  }

  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return false;);

  // all read and parsed, actually import
  try
  {
    m_in_manual_refresh.store(true, std::memory_order_relaxed);
    epee::misc_utils::auto_scope_leave_caller scope_exit_handler = epee::misc_utils::create_scope_leave_handler([&](){m_in_manual_refresh.store(false, std::memory_order_relaxed);});
    size_t n_outputs = m_wallet->import_multisig(info);
    // Clear line "Height xxx of xxx"
    std::cout << "\r                                                                \r";
    success_msg_writer() << tr("Multisig info imported");
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to import multisig info: ") << e.what();
    return false;
  }
  if (m_wallet->is_trusted_daemon())
  {
    try
    {
      m_wallet->rescan_spent();
    }
    catch (const std::exception &e)
    {
      message_writer() << tr("Failed to update spent status after importing multisig info: ") << e.what();
      return false;
    }
  }
  else
  {
    message_writer() << tr("Untrusted daemon, spent status may be incorrect. Use a trusted daemon and run \"rescan_spent\"");
    return false;
  }
  return true;
}

bool simple_wallet::accept_loaded_tx(const tools::wallet2::multisig_tx_set &txs)
{
  std::string extra_message;
  return accept_loaded_tx([&txs](){return txs.m_ptx.size();}, [&txs](size_t n)->const tools::wallet2::tx_construction_data&{return txs.m_ptx[n].construction_data;}, extra_message);
}

bool simple_wallet::sign_multisig(const std::vector<std::string> &args)
{
  sign_multisig_main(args, false);
  return true;
}

bool simple_wallet::sign_multisig_main(const std::vector<std::string> &args, bool called_by_mms)
{
  bool ready;
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return false;
  }
  if(!m_wallet->multisig(&ready))
  {
    fail_msg_writer() << tr("This is not a multisig wallet");
    return false;
  }
  if (!ready)
  {
    fail_msg_writer() << tr("This multisig wallet is not yet finalized");
    return false;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_SIGN_MULTISIG);
    return false;
  }

  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return false;);

  std::string filename = args[0];
  std::vector<crypto::hash> txids;
  uint32_t signers = 0;
  try
  {
    if (called_by_mms)
    {
      tools::wallet2::multisig_tx_set exported_txs;
      std::string ciphertext;
      bool r = m_wallet->load_multisig_tx(args[0], exported_txs, [&](const tools::wallet2::multisig_tx_set &tx){ signers = tx.m_signers.size(); return accept_loaded_tx(tx); });
      if (r)
      {
        r = m_wallet->sign_multisig_tx(exported_txs, txids);
      }
      if (r)
      {
        ciphertext = m_wallet->save_multisig_tx(exported_txs);
        if (ciphertext.empty())
        {
          r = false;
        }
      }
      if (r)
      {
        mms::message_type message_type = mms::message_type::fully_signed_tx;
        if (txids.empty())
        {
          message_type = mms::message_type::partially_signed_tx;
        }
        get_message_store().process_wallet_created_data(get_multisig_wallet_state(), message_type, ciphertext);
        filename = "MMS";   // for the messages below
      }
      else
      {
        fail_msg_writer() << tr("Failed to sign multisig transaction");
        return false;
      }
    }
    else
    {
      bool r = m_wallet->sign_multisig_tx_from_file(filename, txids, [&](const tools::wallet2::multisig_tx_set &tx){ signers = tx.m_signers.size(); return accept_loaded_tx(tx); });
      if (!r)
      {
        fail_msg_writer() << tr("Failed to sign multisig transaction");
        return false;
      }
    }
  }
  catch (const tools::error::multisig_export_needed& e)
  {
    fail_msg_writer() << tr("Multisig error: ") << e.what();
    return false;
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to sign multisig transaction: ") << e.what();
    return false;
  }

  if (txids.empty())
  {
    uint32_t threshold;
    m_wallet->multisig(NULL, &threshold);
    uint32_t signers_needed = threshold - signers - 1;
    success_msg_writer(true) << tr("Transaction successfully signed to file ") << filename << ", "
        << signers_needed << " more signer(s) needed";
    return true;
  }
  else
  {
    std::string txids_as_text;
    for (const auto &txid: txids)
    {
      if (!txids_as_text.empty())
        txids_as_text += (", ");
      txids_as_text += epee::string_tools::pod_to_hex(txid);
    }
    success_msg_writer(true) << tr("Transaction successfully signed to file ") << filename << ", txid " << txids_as_text;
    success_msg_writer(true) << tr("It may be relayed to the network with submit_multisig");
  }
  return true;
}

bool simple_wallet::submit_multisig(const std::vector<std::string> &args)
{
  submit_multisig_main(args, false);
  return true;
}

bool simple_wallet::submit_multisig_main(const std::vector<std::string> &args, bool called_by_mms)
{
  bool ready;
  uint32_t threshold;
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return false;
  }
  if (!m_wallet->multisig(&ready, &threshold))
  {
    fail_msg_writer() << tr("This is not a multisig wallet");
    return false;
  }
  if (!ready)
  {
    fail_msg_writer() << tr("This multisig wallet is not yet finalized");
    return false;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_SUBMIT_MULTISIG);
    return false;
  }

  if (!try_connect_to_daemon())
    return false;

  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return false;);

  std::string filename = args[0];
  try
  {
    tools::wallet2::multisig_tx_set txs;
    if (called_by_mms)
    {
      bool r = m_wallet->load_multisig_tx(args[0], txs, [&](const tools::wallet2::multisig_tx_set &tx){ return accept_loaded_tx(tx); });
      if (!r)
      {
        fail_msg_writer() << tr("Failed to load multisig transaction from MMS");
        return false;
      }
    }
    else
    {
      bool r = m_wallet->load_multisig_tx_from_file(filename, txs, [&](const tools::wallet2::multisig_tx_set &tx){ return accept_loaded_tx(tx); });
      if (!r)
      {
        fail_msg_writer() << tr("Failed to load multisig transaction from file");
        return false;
      }
    }
    if (txs.m_signers.size() < threshold)
    {
      fail_msg_writer() << (boost::format(tr("Multisig transaction signed by only %u signers, needs %u more signatures"))
          % txs.m_signers.size() % (threshold - txs.m_signers.size())).str();
      return false;
    }

    // actually commit the transactions
    for (auto &ptx: txs.m_ptx)
    {
      m_wallet->commit_tx(ptx);
      success_msg_writer(true) << tr("Transaction successfully submitted, transaction ") << get_transaction_hash(ptx.tx) << ENDL
          << tr("You can check its status by using the `show_transfers` command.");
    }
  }
  catch (const std::exception &e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
    return false;
  }

  return true;
}

bool simple_wallet::export_raw_multisig(const std::vector<std::string> &args)
{
  bool ready;
  uint32_t threshold;
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (!m_wallet->multisig(&ready, &threshold))
  {
    fail_msg_writer() << tr("This is not a multisig wallet");
    return true;
  }
  if (!ready)
  {
    fail_msg_writer() << tr("This multisig wallet is not yet finalized");
    return true;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_EXPORT_RAW_MULTISIG_TX);
    return true;
  }

  std::string filename = args[0];
  if (m_wallet->confirm_export_overwrite() && !check_file_overwrite(filename))
    return true;

  SCOPED_WALLET_UNLOCK();

  try
  {
    tools::wallet2::multisig_tx_set txs;
    bool r = m_wallet->load_multisig_tx_from_file(filename, txs, [&](const tools::wallet2::multisig_tx_set &tx){ return accept_loaded_tx(tx); });
    if (!r)
    {
      fail_msg_writer() << tr("Failed to load multisig transaction from file");
      return true;
    }
    if (txs.m_signers.size() < threshold)
    {
      fail_msg_writer() << (boost::format(tr("Multisig transaction signed by only %u signers, needs %u more signatures"))
          % txs.m_signers.size() % (threshold - txs.m_signers.size())).str();
      return true;
    }

    // save the transactions
    std::string filenames;
    for (auto &ptx: txs.m_ptx)
    {
      const crypto::hash txid = cryptonote::get_transaction_hash(ptx.tx);
      const std::string filename = std::string("raw_multisig_antd_tx_") + epee::string_tools::pod_to_hex(txid);
      if (!filenames.empty())
        filenames += ", ";
      filenames += filename;
      if (!epee::file_io_utils::save_string_to_file(filename, cryptonote::tx_to_blob(ptx.tx)))
      {
        fail_msg_writer() << tr("Failed to export multisig transaction to file ") << filename;
        return true;
      }
    }
    success_msg_writer() << tr("Saved exported multisig transaction file(s): ") << filenames;
  }
  catch (const std::exception& e)
  {
    LOG_ERROR("unexpected error: " << e.what());
    fail_msg_writer() << tr("unexpected error: ") << e.what();
  }
  catch (...)
  {
    LOG_ERROR("Unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}

bool simple_wallet::print_ring(const std::vector<std::string> &args)
{
  crypto::key_image key_image;
  crypto::hash txid;
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_PRINT_RING);
    return true;
  }

  if (!epee::string_tools::hex_to_pod(args[0], key_image))
  {
    fail_msg_writer() << tr("Invalid key image");
    return true;
  }
  // this one will always work, they're all 32 byte hex
  if (!epee::string_tools::hex_to_pod(args[0], txid))
  {
    fail_msg_writer() << tr("Invalid txid");
    return true;
  }

  std::vector<uint64_t> ring;
  std::vector<std::pair<crypto::key_image, std::vector<uint64_t>>> rings;
  try
  {
    if (m_wallet->get_ring(key_image, ring))
      rings.push_back({key_image, ring});
    else if (!m_wallet->get_rings(txid, rings))
    {
      fail_msg_writer() << tr("Key image either not spent, or spent with mixin 0");
      return true;
    }

    for (const auto &ring: rings)
    {
      std::stringstream str;
      for (const auto &x: ring.second)
        str << x<< " ";
      // do NOT translate this "absolute" below, the lin can be used as input to set_ring
      success_msg_writer() << epee::string_tools::pod_to_hex(ring.first) <<  " absolute " << str.str();
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to get key image ring: ") << e.what();
  }

  return true;
}

bool simple_wallet::set_ring(const std::vector<std::string> &args)
{
  crypto::key_image key_image;

  // try filename first
  if (args.size() == 1)
  {
    if (!epee::file_io_utils::is_file_exist(args[0]))
    {
      fail_msg_writer() << tr("File doesn't exist");
      return true;
    }

    char str[4096];
    std::unique_ptr<FILE, tools::close_file> f(fopen(args[0].c_str(), "r"));
    if (f)
    {
      while (!feof(f.get()))
      {
        if (!fgets(str, sizeof(str), f.get()))
          break;
        const size_t len = strlen(str);
        if (len > 0 && str[len - 1] == '\n')
          str[len - 1] = 0;
        if (!str[0])
          continue;
        char key_image_str[65], type_str[9];
        int read_after_key_image = 0, read = 0;
        int fields = sscanf(str, "%64[abcdefABCDEF0123456789] %n%8s %n", key_image_str, &read_after_key_image, type_str, &read);
        if (fields != 2)
        {
          fail_msg_writer() << tr("Invalid ring specification: ") << str;
          continue;
        }
        key_image_str[64] = 0;
        type_str[8] = 0;
        crypto::key_image key_image;
        if (read_after_key_image == 0 || !epee::string_tools::hex_to_pod(key_image_str, key_image))
        {
          fail_msg_writer() << tr("Invalid key image: ") << str;
          continue;
        }
        if (read == read_after_key_image+8 || (strcmp(type_str, "absolute") && strcmp(type_str, "relative")))
        {
          fail_msg_writer() << tr("Invalid ring type, expected relative or abosolute: ") << str;
          continue;
        }
        bool relative = !strcmp(type_str, "relative");
        if (read < 0 || (size_t)read > strlen(str))
        {
          fail_msg_writer() << tr("Error reading line: ") << str;
          continue;
        }
        bool valid = true;
        std::vector<uint64_t> ring;
        const char *ptr = str + read;
        while (*ptr)
        {
          unsigned long offset;
          int elements = sscanf(ptr, "%lu %n", &offset, &read);
          if (elements == 0 || read <= 0 || (size_t)read > strlen(str))
          {
            fail_msg_writer() << tr("Error reading line: ") << str;
            valid = false;
            break;
          }
          ring.push_back(offset);
          ptr += read;
        }
        if (!valid)
          continue;
        if (ring.empty())
        {
          fail_msg_writer() << tr("Invalid ring: ") << str;
          continue;
        }
        if (relative)
        {
          for (size_t n = 1; n < ring.size(); ++n)
          {
            if (ring[n] <= 0)
            {
              fail_msg_writer() << tr("Invalid relative ring: ") << str;
              valid = false;
              break;
            }
          }
        }
        else
        {
          for (size_t n = 1; n < ring.size(); ++n)
          {
            if (ring[n] <= ring[n-1])
            {
              fail_msg_writer() << tr("Invalid absolute ring: ") << str;
              valid = false;
              break;
            }
          }
        }
        if (!valid)
          continue;
        if (!m_wallet->set_ring(key_image, ring, relative))
          fail_msg_writer() << tr("Failed to set ring for key image: ") << key_image << ". " << tr("Continuing.");
      }
      f.reset();
    }
    return true;
  }

  if (args.size() < 3)
  {
    PRINT_USAGE(USAGE_SET_RING);
    return true;
  }

  if (!epee::string_tools::hex_to_pod(args[0], key_image))
  {
    fail_msg_writer() << tr("Invalid key image");
    return true;
  }

  bool relative;
  if (args[1] == "absolute")
  {
    relative = false;
  }
  else if (args[1] == "relative")
  {
    relative = true;
  }
  else
  {
    fail_msg_writer() << tr("Missing absolute or relative keyword");
    return true;
  }

  std::vector<uint64_t> ring;
  for (size_t n = 2; n < args.size(); ++n)
  {
    ring.resize(ring.size() + 1);
    if (!string_tools::get_xtype_from_string(ring.back(), args[n]))
    {
      fail_msg_writer() << tr("invalid index: must be a strictly positive unsigned integer");
      return true;
    }
    if (relative)
    {
      if (ring.size() > 1 && !ring.back())
      {
        fail_msg_writer() << tr("invalid index: must be a strictly positive unsigned integer");
        return true;
      }
      uint64_t sum = 0;
      for (uint64_t out: ring)
      {
        if (out > std::numeric_limits<uint64_t>::max() - sum)
        {
          fail_msg_writer() << tr("invalid index: indices wrap");
          return true;
        }
        sum += out;
      }
    }
    else
    {
      if (ring.size() > 1 && ring[ring.size() - 2] >= ring[ring.size() - 1])
      {
        fail_msg_writer() << tr("invalid index: indices should be in strictly ascending order");
        return true;
      }
    }
  }
  if (!m_wallet->set_ring(key_image, ring, relative))
  {
    fail_msg_writer() << tr("failed to set ring");
    return true;
  }

  return true;
}

bool simple_wallet::blackball(const std::vector<std::string> &args)
{
  uint64_t amount = std::numeric_limits<uint64_t>::max(), offset, num_offsets;
  if (args.size() == 0)
  {
    PRINT_USAGE(USAGE_MARK_OUTPUT_SPENT);
    return true;
  }

  try
  {
    if (sscanf(args[0].c_str(), "%" PRIu64 "/%" PRIu64, &amount, &offset) == 2)
    {
      m_wallet->blackball_output(std::make_pair(amount, offset));
    }
    else if (epee::file_io_utils::is_file_exist(args[0]))
    {
      std::vector<std::pair<uint64_t, uint64_t>> outputs;
      char str[256];

      std::unique_ptr<FILE, tools::close_file> f(fopen(args[0].c_str(), "r"));
      if (f)
      {
        while (!feof(f.get()))
        {
          if (!fgets(str, sizeof(str), f.get()))
            break;
          const size_t len = strlen(str);
          if (len > 0 && str[len - 1] == '\n')
            str[len - 1] = 0;
          if (!str[0])
            continue;
          if (sscanf(str, "@%" PRIu64, &amount) == 1)
          {
            continue;
          }
          if (amount == std::numeric_limits<uint64_t>::max())
          {
            fail_msg_writer() << tr("First line is not an amount");
            return true;
          }
          if (sscanf(str, "%" PRIu64 "*%" PRIu64, &offset, &num_offsets) == 2 && num_offsets <= std::numeric_limits<uint64_t>::max() - offset)
          {
            while (num_offsets--)
              outputs.push_back(std::make_pair(amount, offset++));
          }
          else if (sscanf(str, "%" PRIu64, &offset) == 1)
          {
            outputs.push_back(std::make_pair(amount, offset));
          }
          else
          {
            fail_msg_writer() << tr("Invalid output: ") << str;
            return true;
          }
        }
        f.reset();
        bool add = false;
        if (args.size() > 1)
        {
          if (args[1] != "add")
          {
            fail_msg_writer() << tr("Bad argument: ") + args[1] + ": " + tr("should be \"add\"");
            return true;
          }
          add = true;
        }
        m_wallet->set_blackballed_outputs(outputs, add);
      }
      else
      {
        fail_msg_writer() << tr("Failed to open file");
        return true;
      }
    }
    else
    {
      fail_msg_writer() << tr("Invalid output key, and file doesn't exist");
      return true;
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to mark output spent: ") << e.what();
  }

  return true;
}

bool simple_wallet::unblackball(const std::vector<std::string> &args)
{
  std::pair<uint64_t, uint64_t> output;
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_MARK_OUTPUT_UNSPENT);
    return true;
  }

  if (sscanf(args[0].c_str(), "%" PRIu64 "/%" PRIu64, &output.first, &output.second) != 2)
  {
    fail_msg_writer() << tr("Invalid output");
    return true;
  }

  try
  {
    m_wallet->unblackball_output(output);
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to mark output unspent: ") << e.what();
  }

  return true;
}

bool simple_wallet::blackballed(const std::vector<std::string> &args)
{
  std::pair<uint64_t, uint64_t> output;
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_IS_OUTPUT_SPENT);
    return true;
  }

  if (sscanf(args[0].c_str(), "%" PRIu64 "/%" PRIu64, &output.first, &output.second) != 2)
  {
    fail_msg_writer() << tr("Invalid output");
    return true;
  }

  try
  {
    if (m_wallet->is_output_blackballed(output))
      message_writer() << tr("Spent: ") << output.first << "/" << output.second;
    else
      message_writer() << tr("Not spent: ") << output.first << "/" << output.second;
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to check whether output is spent: ") << e.what();
  }

  return true;
}

bool simple_wallet::save_known_rings(const std::vector<std::string> &args)
{
  try
  {
    LOCK_IDLE_SCOPE();
    m_wallet->find_and_save_rings();
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to save known rings: ") << e.what();
  }
  return true;
}

bool simple_wallet::version(const std::vector<std::string> &args)
{
  message_writer() << "Antd '" << ANTD_RELEASE_NAME << "' (v" << ANTD_VERSION_FULL << ")";
  return true;
}

bool simple_wallet::cold_sign_tx(const std::vector<tools::wallet2::pending_tx>& ptx_vector, tools::wallet2::signed_tx_set &exported_txs, std::vector<cryptonote::address_parse_info> &dsts_info, std::function<bool(const tools::wallet2::signed_tx_set &)> accept_func)
{
  std::vector<std::string> tx_aux;

  message_writer(console_color_white, false) << tr("Please confirm the transaction on the device");

  m_wallet->cold_sign_tx(ptx_vector, exported_txs, dsts_info, tx_aux);

  if (accept_func && !accept_func(exported_txs))
  {
    MERROR("Transactions rejected by callback");
    return false;
  }

  // aux info
  m_wallet->cold_tx_aux_import(exported_txs.ptx, tx_aux);

  // import key images
  return m_wallet->import_key_images(exported_txs.key_images);
}

bool simple_wallet::set_always_confirm_transfers(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->always_confirm_transfers(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_print_ring_members(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->print_ring_members(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_store_tx_info(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and cannot transfer");
    return true;
  }
 
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->store_tx_info(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_default_priority(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  uint32_t priority = 0;
  try
  {
    if (strchr(args[1].c_str(), '-'))
    {
      fail_msg_writer() << tr("priority must be either 0, 1, 2, 3, or 4, or one of: ") << join_priority_strings(", ");
      return true;
    }
    if (args[1] == "0")
    {
      priority = 0;
    }
    else
    {
      bool found = false;
      for (size_t n = 0; n < tools::allowed_priority_strings.size(); ++n)
      {
        if (tools::allowed_priority_strings[n] == args[1])
        {
          found = true;
          priority = n;
        }
      }
      if (!found)
      {
        priority = boost::lexical_cast<int>(args[1]);
        if (priority < 1 || priority > 4)
        {
          fail_msg_writer() << tr("priority must be either 0, 1, 2, 3, or 4, or one of: ") << join_priority_strings(", ");
          return true;
        }
      }
    }
 
    const auto pwd_container = get_and_verify_password();
    if (pwd_container)
    {
      m_wallet->set_default_priority(priority);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    }
    return true;
  }
  catch(const boost::bad_lexical_cast &)
  {
    fail_msg_writer() << tr("priority must be either 0, 1, 2, 3, or 4, or one of: ") << join_priority_strings(", ");
    return true;
  }
  catch(...)
  {
    fail_msg_writer() << tr("could not change default priority");
    return true;
  }
}

bool simple_wallet::set_auto_refresh(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool auto_refresh) {
      m_auto_refresh_enabled.store(false, std::memory_order_relaxed);
      m_wallet->auto_refresh(auto_refresh);
      m_idle_mutex.lock();
      m_auto_refresh_enabled.store(auto_refresh, std::memory_order_relaxed);
      m_idle_cond.notify_one();
      m_idle_mutex.unlock();

      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_refresh_type(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  tools::wallet2::RefreshType refresh_type;
  if (!parse_refresh_type(args[1], refresh_type))
  {
    return true;
  }
 
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    m_wallet->set_refresh_type(refresh_type);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_confirm_missing_payment_id(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  LONG_PAYMENT_ID_SUPPORT_CHECK();

  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->confirm_missing_payment_id(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_ask_password(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    tools::wallet2::AskPasswordType ask = tools::wallet2::AskPasswordToDecrypt;
    if (args[1] == "never" || args[1] == "0")
      ask = tools::wallet2::AskPasswordNever;
    else if (args[1] == "action" || args[1] == "1")
      ask = tools::wallet2::AskPasswordOnAction;
    else if (args[1] == "encrypt" || args[1] == "decrypt" || args[1] == "2")
      ask = tools::wallet2::AskPasswordToDecrypt;
    else
    {
      fail_msg_writer() << tr("invalid argument: must be either 0/never, 1/action, or 2/encrypt/decrypt");
      return true;
    }

    const tools::wallet2::AskPasswordType cur_ask = m_wallet->ask_password();
    if (!m_wallet->watch_only())
    {
      if (cur_ask == tools::wallet2::AskPasswordToDecrypt && ask != tools::wallet2::AskPasswordToDecrypt)
        m_wallet->decrypt_keys(pwd_container->password());
      else if (cur_ask != tools::wallet2::AskPasswordToDecrypt && ask == tools::wallet2::AskPasswordToDecrypt)
        m_wallet->encrypt_keys(pwd_container->password());
    }
    m_wallet->ask_password(ask);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_unit(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const std::string &unit = args[1];
  unsigned int decimal_point = CRYPTONOTE_DISPLAY_DECIMAL_POINT;

  if (unit == "antd")
    decimal_point = CRYPTONOTE_DISPLAY_DECIMAL_POINT;
  else if (unit == "megarok")
    decimal_point = CRYPTONOTE_DISPLAY_DECIMAL_POINT - 3;
  else if (unit == "kilorok")
    decimal_point = CRYPTONOTE_DISPLAY_DECIMAL_POINT - 6;
  else if (unit == "rok")
    decimal_point = 0;
  else
  {
    fail_msg_writer() << tr("invalid unit");
    return true;
  }

  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    cryptonote::set_default_decimal_point(decimal_point);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_min_output_count(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  uint32_t count;
  if (!string_tools::get_xtype_from_string(count, args[1]))
  {
    fail_msg_writer() << tr("invalid count: must be an unsigned integer");
    return true;
  }

  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    m_wallet->set_min_output_count(count);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_min_output_value(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  uint64_t value;
  if (!cryptonote::parse_amount(value, args[1]))
  {
    fail_msg_writer() << tr("invalid value");
    return true;
  }

  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    m_wallet->set_min_output_value(value);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_merge_destinations(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->merge_destinations(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_confirm_backlog(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->confirm_backlog(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_confirm_backlog_threshold(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  uint32_t threshold;
  if (!string_tools::get_xtype_from_string(threshold, args[1]))
  {
    fail_msg_writer() << tr("invalid count: must be an unsigned integer");
    return true;
  }

  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    m_wallet->set_confirm_backlog_threshold(threshold);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_confirm_export_overwrite(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->confirm_export_overwrite(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_refresh_from_block_height(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    uint64_t height;
    if (!epee::string_tools::get_xtype_from_string(height, args[1]))
    {
      fail_msg_writer() << tr("Invalid height");
      return true;
    }
    m_wallet->set_refresh_from_block_height(height);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_auto_low_priority(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->auto_low_priority(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_segregate_pre_fork_outputs(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->segregate_pre_fork_outputs(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_key_reuse_mitigation2(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->key_reuse_mitigation2(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_subaddress_lookahead(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    auto lookahead = parse_subaddress_lookahead(args[1]);
    if (lookahead)
    {
      m_wallet->set_subaddress_lookahead(lookahead->first, lookahead->second);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    }
  }
  return true;
}

bool simple_wallet::set_segregation_height(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    uint64_t height;
    if (!epee::string_tools::get_xtype_from_string(height, args[1]))
    {
      fail_msg_writer() << tr("Invalid height");
      return true;
    }
    m_wallet->segregation_height(height);
    m_wallet->rewrite(m_wallet_file, pwd_container->password());
  }
  return true;
}

bool simple_wallet::set_ignore_fractional_outputs(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->ignore_fractional_outputs(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_track_uses(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    parse_bool_and_use(args[1], [&](bool r) {
      m_wallet->track_uses(r);
      m_wallet->rewrite(m_wallet_file, pwd_container->password());
    });
  }
  return true;
}

bool simple_wallet::set_device_name(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  const auto pwd_container = get_and_verify_password();
  if (pwd_container)
  {
    if (args.size() == 0){
      fail_msg_writer() << tr("Device name not specified");
      return true;
    }

    m_wallet->device_name(args[0]);
    bool r = false;
    try {
      r = m_wallet->reconnect_device();
      if (!r){
        fail_msg_writer() << tr("Device reconnect failed");
      }

    } catch(const std::exception & e){
      MWARNING("Device reconnect failed: " << e.what());
      fail_msg_writer() << tr("Device reconnect failed: ") << e.what();
    }

  }
  return true;
}

bool simple_wallet::help(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if(args.empty())
  {
    success_msg_writer() << get_commands_str();
  }
  else if ((args.size() == 2) && (args.front() == "mms"))
  {
    // Little hack to be able to do "help mms <subcommand>"
    std::vector<std::string> mms_args(1, args.front() + " " + args.back());
    success_msg_writer() << get_command_usage(mms_args);
  }
  else
  {
    success_msg_writer() << get_command_usage(args);
  }
  return true;
}

simple_wallet::simple_wallet()
  : m_allow_mismatched_daemon_version(false)
  , m_refresh_progress_reporter(*this)
  , m_idle_run(true)
  , m_auto_refresh_enabled(false)
  , m_auto_refresh_refreshing(false)
  , m_in_manual_refresh(false)
    , m_current_subaddress_account(0)
{
  m_cmd_binder.set_handler("start_mining",
                           boost::bind(&simple_wallet::start_mining, this, _1),
                           tr(USAGE_START_MINING),
                           tr("Start mining in the daemon (bg_mining and ignore_battery are optional booleans)."));
  m_cmd_binder.set_handler("stop_mining",
      boost::bind(&simple_wallet::stop_mining, this, _1),
      tr("Stop mining in the daemon."));
  m_cmd_binder.set_handler("set_daemon",
                           boost::bind(&simple_wallet::set_daemon, this, _1),
                           tr(USAGE_SET_DAEMON),
                           tr("Set another daemon to connect to."));
  m_cmd_binder.set_handler("save_bc",
      boost::bind(&simple_wallet::save_bc, this, _1),
      tr("Save the current blockchain data."));
  m_cmd_binder.set_handler("refresh",
      boost::bind(&simple_wallet::refresh, this, _1),
      tr("Synchronize the transactions and balance."));
  m_cmd_binder.set_handler("balance",
                           boost::bind(&simple_wallet::show_balance, this, _1),
                           tr(USAGE_SHOW_BALANCE),
                           tr("Show the wallet's balance of the currently selected account."));
  m_cmd_binder.set_handler("incoming_transfers",
                           boost::bind(&simple_wallet::show_incoming_transfers, this, _1),
                           tr(USAGE_INCOMING_TRANSFERS),
                           tr("Show the incoming transfers, all or filtered by availability and address index.\n\n"
                              "Output format:\n"
                              "Amount, Spent(\"T\"|\"F\"), \"locked\"|\"unlocked\", RingCT, Global Index, Transaction Hash, Address Index, [Public Key, Key Image] "));
  m_cmd_binder.set_handler("payments",
                           boost::bind(&simple_wallet::show_payments, this, _1),
                           tr(USAGE_PAYMENTS),
                           tr("Show the payments for the given payment IDs."));
  m_cmd_binder.set_handler("bc_height",
      boost::bind(&simple_wallet::show_blockchain_height, this, _1),
      tr("Show the blockchain height."));
  m_cmd_binder.set_handler("transfer", boost::bind(&simple_wallet::transfer, this, _1),
                           tr(USAGE_TRANSFER),
                           tr("Transfer <amount> to <address>. If the parameter \"index=<N1>[,<N2>,...]\" is specified, the wallet uses outputs received by addresses of those indices. If omitted, the wallet randomly chooses address indices to be used. In any case, it tries its best not to combine outputs across multiple addresses. <priority> is the priority of the transaction. The higher the priority, the higher the transaction fee. Valid values in priority order (from lowest to highest) are: unimportant, normal, elevated, priority. If omitted, the default value (see the command \"set priority\") is used. Multiple payments can be made at once by adding <address_2> <amount_2> etcetera (before the payment ID, if it's included)"));
  m_cmd_binder.set_handler("locked_transfer",
                           boost::bind(&simple_wallet::locked_transfer, this, _1),
                           tr(USAGE_LOCKED_TRANSFER),
                           tr("Transfer <amount> to <address> and lock it for <lockblocks> (max. 1000000). If the parameter \"index=<N1>[,<N2>,...]\" is specified, the wallet uses outputs received by addresses of those indices. If omitted, the wallet randomly chooses address indices to be used. In any case, it tries its best not to combine outputs across multiple addresses. <priority> is the priority of the transaction. The higher the priority, the higher the transaction fee. Valid values in priority order (from lowest to highest) are: unimportant, normal, elevated, priority. If omitted, the default value (see the command \"set priority\") is used. Multiple payments can be made at once by adding URI_2 or <address_2> <amount_2> etcetera (before the payment ID, if it's included)"));
  m_cmd_binder.set_handler("locked_sweep_all",
                           boost::bind(&simple_wallet::locked_sweep_all, this, _1),
                           tr(USAGE_LOCKED_SWEEP_ALL),
                           tr("Send all unlocked balance to an address and lock it for <lockblocks> (max. 1000000). If the parameter \"index<N1>[,<N2>,...]\" is specified, the wallet sweeps outputs received by those address indices. If omitted, the wallet randomly chooses an address index to be used. <priority> is the priority of the sweep. The higher the priority, the higher the transaction fee. Valid values in priority order (from lowest to highest) are: unimportant, normal, elevated, priority. If omitted, the default value (see the command \"set priority\") is used."));
  m_cmd_binder.set_handler("sweep_unmixable",
                           boost::bind(&simple_wallet::sweep_unmixable, this, _1),
                           tr("Deprecated"));
  m_cmd_binder.set_handler("sweep_all", boost::bind(&simple_wallet::sweep_all, this, _1),
                           tr(USAGE_SWEEP_ALL),
                           tr("Send all unlocked balance to an address. If the parameter \"index<N1>[,<N2>,...]\" is specified, the wallet sweeps outputs received by those address indices. If omitted, the wallet randomly chooses an address index to be used. If the parameter \"outputs=<N>\" is specified and  N > 0, wallet splits the transaction into N even outputs."
                              " If \"use_v1_tx\" is placed at the end, sweep_all will include version 1 transactions into the sweeping process as well, otherwise exclude them"
                             ));
  m_cmd_binder.set_handler("sweep_below",
                           boost::bind(&simple_wallet::sweep_below, this, _1),
                           tr(USAGE_SWEEP_BELOW),
                           tr("Send all unlocked outputs below the threshold to an address."));
  m_cmd_binder.set_handler("sweep_single",
                           boost::bind(&simple_wallet::sweep_single, this, _1),
                           tr(USAGE_SWEEP_SINGLE),
                           tr("Send a single output of the given key image to an address without change."));
  m_cmd_binder.set_handler("sweep_unmixable",
                           boost::bind(&simple_wallet::sweep_unmixable, this, _1),
                           tr("Deprecated"));
  m_cmd_binder.set_handler("sign_transfer",
                           boost::bind(&simple_wallet::sign_transfer, this, _1),
                           tr(USAGE_SIGN_TRANSFER),
                           tr("Sign a transaction from a file. If the parameter \"export_raw\" is specified, transaction raw hex data suitable for the daemon RPC /sendrawtransaction is exported."));
  m_cmd_binder.set_handler("submit_transfer",
                           boost::bind(&simple_wallet::submit_transfer, this, _1),
                           tr("Submit a signed transaction from a file."));
  m_cmd_binder.set_handler("set_log",
                           boost::bind(&simple_wallet::set_log, this, _1),
                           tr(USAGE_SET_LOG),
                           tr("Change the current log detail (level must be <0-4>)."));
  m_cmd_binder.set_handler("account",
                           boost::bind(&simple_wallet::account, this, _1),
                           tr(USAGE_ACCOUNT),
                           tr("If no arguments are specified, the wallet shows all the existing accounts along with their balances.\n"
                              "If the \"new\" argument is specified, the wallet creates a new account with its label initialized by the provided label text (which can be empty).\n"
                              "If the \"switch\" argument is specified, the wallet switches to the account specified by <index>.\n"
                              "If the \"label\" argument is specified, the wallet sets the label of the account specified by <index> to the provided label text.\n"
                              "If the \"tag\" argument is specified, a tag <tag_name> is assigned to the specified accounts <account_index_1>, <account_index_2>, ....\n"
                              "If the \"untag\" argument is specified, the tags assigned to the specified accounts <account_index_1>, <account_index_2> ..., are removed.\n"
                              "If the \"tag_description\" argument is specified, the tag <tag_name> is assigned an arbitrary text <description>."));
  m_cmd_binder.set_handler("address",
                           boost::bind(&simple_wallet::print_address, this, _1),
                           tr(USAGE_ADDRESS),
                           tr("If no arguments are specified or <index> is specified, the wallet shows the default or specified address. If \"all\" is specified, the wallet shows all the existing addresses in the currently selected account. If \"new \" is specified, the wallet creates a new address with the provided label text (which can be empty). If \"label\" is specified, the wallet sets the label of the address specified by <index> to the provided label text."));
  m_cmd_binder.set_handler("integrated_address",
                           boost::bind(&simple_wallet::print_integrated_address, this, _1),
                           tr(USAGE_INTEGRATED_ADDRESS),
                           tr("Encode a payment ID into an integrated address for the current wallet public address (no argument uses a random payment ID), or decode an integrated address to standard address and payment ID"));
  m_cmd_binder.set_handler("address_book",
                           boost::bind(&simple_wallet::address_book, this, _1),
                           tr(USAGE_ADDRESS_BOOK),
                           tr("Print all entries in the address book, optionally adding/deleting an entry to/from it."));
  m_cmd_binder.set_handler("save",
                           boost::bind(&simple_wallet::save, this, _1),
                           tr("Save the wallet data."));
  m_cmd_binder.set_handler("save_watch_only",
                           boost::bind(&simple_wallet::save_watch_only, this, _1),
                           tr("Save a watch-only keys file."));
  m_cmd_binder.set_handler("viewkey",
                           boost::bind(&simple_wallet::viewkey, this, _1),
                           tr("Display the private view key."));
  m_cmd_binder.set_handler("spendkey",
                           boost::bind(&simple_wallet::spendkey, this, _1),
                           tr("Display the private spend key."));
  m_cmd_binder.set_handler("seed",
                           boost::bind(&simple_wallet::seed, this, _1),
                           tr("Display the Electrum-style mnemonic seed"));
  m_cmd_binder.set_handler("set",
                           boost::bind(&simple_wallet::set_variable, this, _1),
                           tr(USAGE_SET_VARIABLE),
                           tr("Available options:\n "
                                  "seed language\n "
                                  "  Set the wallet's seed language.\n "
                                  "always-confirm-transfers <1|0>\n "
                                  "  Whether to confirm unsplit txes.\n "
                                  "print-ring-members <1|0>\n "
                                  "  Whether to print detailed information about ring members during confirmation.\n "
                                  "store-tx-info <1|0>\n "
                                  "  Whether to store outgoing tx info (destination address, payment ID, tx secret key) for future reference.\n "
                                  "auto-refresh <1|0>\n "
                                  "  Whether to automatically synchronize new blocks from the daemon.\n "
                                  "refresh-type <full|optimize-coinbase|no-coinbase|default>\n "
                                  "  Set the wallet's refresh behaviour.\n "
                                  "priority [0|1|2|3|4]\n "
                                  "  Set the fee to default/unimportant/normal/elevated/priority.\n "
                                  "confirm-missing-payment-id <1|0>\n "
                                  "ask-password <0|1|2   (or never|action|decrypt)>\n "
                                  "unit <antd|megarok|kilorok|rok>\n "
                                  "  Set the default antd (sub-)unit.\n "
                                  "min-outputs-count [n]\n "
                                  "  Try to keep at least that many outputs of value at least min-outputs-value.\n "
                                  "min-outputs-value [n]\n "
                                  "  Try to keep at least min-outputs-count outputs of at least that value.\n "
                                  "merge-destinations <1|0>\n "
                                  "  Whether to merge multiple payments to the same destination address.\n "
                                  "confirm-backlog <1|0>\n "
                                  "  Whether to warn if there is transaction backlog.\n "
                                  "confirm-backlog-threshold [n]\n "
                                  "  Set a threshold for confirm-backlog to only warn if the transaction backlog is greater than n blocks.\n "
                                  "refresh-from-block-height [n]\n "
                                  "  Set the height before which to ignore blocks.\n "
                                  "auto-low-priority <1|0>\n "
                                  "  Whether to automatically use the low priority fee level when it's safe to do so.\n "
                                  "segregate-pre-fork-outputs <1|0>\n "
                                  "  Set this if you intend to spend outputs on both Antd AND a key reusing fork.\n "
                                  "key-reuse-mitigation2 <1|0>\n "
                                  "  Set this if you are not sure whether you will spend on a key reusing Antd fork later.\n"
                                  "subaddress-lookahead <major>:<minor>\n "
                                  "  Set the lookahead sizes for the subaddress hash table.\n "
                                  "  Set this if you are not sure whether you will spend on a key reusing Antd fork later.\n "
                                  "segregation-height <n>\n "
                                  "  Set to the height of a key reusing fork you want to use, 0 to use default."));
  m_cmd_binder.set_handler("encrypted_seed",
                           boost::bind(&simple_wallet::encrypted_seed, this, _1),
                           tr("Display the encrypted Electrum-style mnemonic seed."));
  m_cmd_binder.set_handler("rescan_spent",
                           boost::bind(&simple_wallet::rescan_spent, this, _1),
                           tr("Rescan the blockchain for spent outputs."));
  m_cmd_binder.set_handler("get_tx_key",
                           boost::bind(&simple_wallet::get_tx_key, this, _1),
                           tr(USAGE_GET_TX_KEY),
                           tr("Get the transaction key (r) for a given <txid>."));
  m_cmd_binder.set_handler("set_tx_key",
                           boost::bind(&simple_wallet::set_tx_key, this, _1),
                           tr(USAGE_SET_TX_KEY),
                           tr("Set the transaction key (r) for a given <txid> in case the tx was made by some other device or 3rd party wallet."));
  m_cmd_binder.set_handler("check_tx_key",
                           boost::bind(&simple_wallet::check_tx_key, this, _1),
                           tr(USAGE_CHECK_TX_KEY),
                           tr("Check the amount going to <address> in <txid>."));
  m_cmd_binder.set_handler("get_tx_proof",
                           boost::bind(&simple_wallet::get_tx_proof, this, _1),
                           tr(USAGE_GET_TX_PROOF),
                           tr("Generate a signature proving funds sent to <address> in <txid>, optionally with a challenge string <message>, using either the transaction secret key (when <address> is not your wallet's address) or the view secret key (otherwise), which does not disclose the secret key."));
  m_cmd_binder.set_handler("check_tx_proof",
                           boost::bind(&simple_wallet::check_tx_proof, this, _1),
                           tr(USAGE_CHECK_TX_PROOF),
                           tr("Check the proof for funds going to <address> in <txid> with the challenge string <message> if any."));
  m_cmd_binder.set_handler("get_spend_proof",
                           boost::bind(&simple_wallet::get_spend_proof, this, _1),
                           tr(USAGE_GET_SPEND_PROOF),
                           tr("Generate a signature proving that you generated <txid> using the spend secret key, optionally with a challenge string <message>."));
  m_cmd_binder.set_handler("check_spend_proof",
                           boost::bind(&simple_wallet::check_spend_proof, this, _1),
                           tr(USAGE_CHECK_SPEND_PROOF),
                           tr("Check a signature proving that the signer generated <txid>, optionally with a challenge string <message>."));
  m_cmd_binder.set_handler("get_reserve_proof",
                           boost::bind(&simple_wallet::get_reserve_proof, this, _1),
                           tr(USAGE_GET_RESERVE_PROOF),
                           tr("Generate a signature proving that you own at least this much, optionally with a challenge string <message>.\n"
                              "If 'all' is specified, you prove the entire sum of all of your existing accounts' balances.\n"
                              "Otherwise, you prove the reserve of the smallest possible amount above <amount> available in your current account."));
  m_cmd_binder.set_handler("check_reserve_proof",
                           boost::bind(&simple_wallet::check_reserve_proof, this, _1),
                           tr(USAGE_CHECK_RESERVE_PROOF),
                           tr("Check a signature proving that the owner of <address> holds at least this much, optionally with a challenge string <message>."));
  m_cmd_binder.set_handler("show_transfers",
                           boost::bind(&simple_wallet::show_transfers, this, _1),
                           tr(USAGE_SHOW_TRANSFERS),
                           // Seemingly broken formatting to compensate for the backslash before the quotes.
                           tr("Show the incoming/outgoing transfers within an optional height range.\n\n"
                              "Output format:\n"
                              "In or Coinbase:    Block Number, \"block\"|\"in\",                Time, Amount,  Transaction Hash, Payment ID, Subaddress Index,                     \"-\", Note\n"
                              "Out:               Block Number, \"out\",                         Time, Amount*, Transaction Hash, Payment ID, Fee, Destinations, Input addresses**, \"-\", Note\n"
                              "Pool:                            \"pool\", \"in\",                Time, Amount,  Transaction Hash, Payment Id, Subaddress Index,                     \"-\", Note, Double Spend Note\n"
                              "Pending or Failed:               \"failed\"|\"pending\", \"out\", Time, Amount*, Transaction Hash, Payment ID, Fee, Input addresses**,               \"-\", Note\n\n"
                              "* Excluding change and fee.\n"
                              "** Set of address indices used as inputs in this transfer."));
   m_cmd_binder.set_handler("export_transfers",
                           boost::bind(&simple_wallet::export_transfers, this, _1),
                           tr(USAGE_EXPORT_TRANSFERS),
                           tr("Export to CSV the incoming/outgoing transfers within an optional height range."));
  m_cmd_binder.set_handler("unspent_outputs",
                           boost::bind(&simple_wallet::unspent_outputs, this, _1),
                           tr(USAGE_UNSPENT_OUTPUTS),
                           tr("Show the unspent outputs of a specified address within an optional amount range."));
  m_cmd_binder.set_handler("rescan_bc",
                           boost::bind(&simple_wallet::rescan_blockchain, this, _1),
                           tr(USAGE_RESCAN_BC),
                           tr("Rescan the blockchain from scratch. If \"hard\" is specified, you will lose any information which can not be recovered from the blockchain itself."));
  m_cmd_binder.set_handler("set_tx_note",
                           boost::bind(&simple_wallet::set_tx_note, this, _1),
                           tr(USAGE_SET_TX_NOTE),
                           tr("Set an arbitrary string note for a <txid>."));
  m_cmd_binder.set_handler("get_tx_note",
                           boost::bind(&simple_wallet::get_tx_note, this, _1),
                           tr(USAGE_GET_TX_NOTE),
                           tr("Get a string note for a txid."));
  m_cmd_binder.set_handler("set_description",
                           boost::bind(&simple_wallet::set_description, this, _1),
                           tr(USAGE_SET_DESCRIPTION),
                           tr("Set an arbitrary description for the wallet."));
  m_cmd_binder.set_handler("get_description",
                           boost::bind(&simple_wallet::get_description, this, _1),
                           tr(USAGE_GET_DESCRIPTION),
                           tr("Get the description of the wallet."));
  m_cmd_binder.set_handler("status",
                           boost::bind(&simple_wallet::status, this, _1),
                           tr("Show the wallet's status."));
  m_cmd_binder.set_handler("wallet_info",
                           boost::bind(&simple_wallet::wallet_info, this, _1),
                           tr("Show the wallet's information."));
  m_cmd_binder.set_handler("sign",
                           boost::bind(&simple_wallet::sign, this, _1),
                           tr(USAGE_SIGN),
                           tr("Sign the contents of a file."));
  m_cmd_binder.set_handler("verify",
                           boost::bind(&simple_wallet::verify, this, _1),
                           tr(USAGE_VERIFY),
                           tr("Verify a signature on the contents of a file."));
  m_cmd_binder.set_handler("export_key_images",
                           boost::bind(&simple_wallet::export_key_images, this, _1),
                           tr(USAGE_EXPORT_KEY_IMAGES),
                           tr("Export a signed set of key images to a <filename>. By default exports all key images. If 'requested-only' is specified export key images for outputs not previously imported."));
  m_cmd_binder.set_handler("import_key_images",
                           boost::bind(&simple_wallet::import_key_images, this, _1),
                           tr(USAGE_IMPORT_KEY_IMAGES),
                           tr("Import a signed key images list and verify their spent status."));
  m_cmd_binder.set_handler("hw_key_images_sync",
                           boost::bind(&simple_wallet::hw_key_images_sync, this, _1),
                           tr(USAGE_HW_KEY_IMAGES_SYNC),
                           tr("Synchronizes key images with the hw wallet."));
  m_cmd_binder.set_handler("hw_reconnect",
                           boost::bind(&simple_wallet::hw_reconnect, this, _1),
                           tr(USAGE_HW_RECONNECT),
                           tr("Attempts to reconnect HW wallet."));
  m_cmd_binder.set_handler("export_outputs",
                           boost::bind(&simple_wallet::export_outputs, this, _1),
                           tr(USAGE_EXPORT_OUTPUTS),
                           tr("Export a set of outputs owned by this wallet."));
  m_cmd_binder.set_handler("import_outputs",
                           boost::bind(&simple_wallet::import_outputs, this, _1),
                           tr(USAGE_IMPORT_OUTPUTS),
                           tr("Import a set of outputs owned by this wallet."));
  m_cmd_binder.set_handler("show_transfer",
                           boost::bind(&simple_wallet::show_transfer, this, _1),
                           tr(USAGE_SHOW_TRANSFER),
                           tr("Show information about a transfer to/from this address."));
  m_cmd_binder.set_handler("password",
                           boost::bind(&simple_wallet::change_password, this, _1),
                           tr("Change the wallet's password."));
  m_cmd_binder.set_handler("payment_id",
                           boost::bind(&simple_wallet::payment_id, this, _1),
                           tr(USAGE_PAYMENT_ID),
                           tr("Generate a new random full size payment id (obsolete). These will be unencrypted on the blockchain, see integrated_address for encrypted short payment ids."));
  m_cmd_binder.set_handler("fee",
                           boost::bind(&simple_wallet::print_fee_info, this, _1),
                           tr("Print the information about the current fee and transaction backlog."));
  m_cmd_binder.set_handler("prepare_multisig", boost::bind(&simple_wallet::prepare_multisig, this, _1),
                           tr("Export data needed to create a multisig wallet"));
  m_cmd_binder.set_handler("make_multisig", boost::bind(&simple_wallet::make_multisig, this, _1),
                           tr(USAGE_MAKE_MULTISIG),
                           tr("Turn this wallet into a multisig wallet"));
  m_cmd_binder.set_handler("finalize_multisig",
                           boost::bind(&simple_wallet::finalize_multisig, this, _1),
                           tr(USAGE_FINALIZE_MULTISIG),
                           tr("Turn this wallet into a multisig wallet, extra step for N-1/N wallets"));
  m_cmd_binder.set_handler("exchange_multisig_keys",
                           boost::bind(&simple_wallet::exchange_multisig_keys, this, _1),
                           tr(USAGE_EXCHANGE_MULTISIG_KEYS),
                           tr("Performs extra multisig keys exchange rounds. Needed for arbitrary M/N multisig wallets"));
  m_cmd_binder.set_handler("export_multisig_info",
                           boost::bind(&simple_wallet::export_multisig, this, _1),
                           tr(USAGE_EXPORT_MULTISIG_INFO),
                           tr("Export multisig info for other participants"));
  m_cmd_binder.set_handler("import_multisig_info",
                           boost::bind(&simple_wallet::import_multisig, this, _1),
                           tr(USAGE_IMPORT_MULTISIG_INFO),
                           tr("Import multisig info from other participants"));
  m_cmd_binder.set_handler("sign_multisig",
                           boost::bind(&simple_wallet::sign_multisig, this, _1),
                           tr(USAGE_SIGN_MULTISIG),
                           tr("Sign a multisig transaction from a file"));
  m_cmd_binder.set_handler("submit_multisig",
                           boost::bind(&simple_wallet::submit_multisig, this, _1),
                           tr(USAGE_SUBMIT_MULTISIG),
                           tr("Submit a signed multisig transaction from a file"));
  m_cmd_binder.set_handler("export_raw_multisig_tx",
                           boost::bind(&simple_wallet::export_raw_multisig, this, _1),
                           tr(USAGE_EXPORT_RAW_MULTISIG_TX),
                           tr("Export a signed multisig transaction to a file"));
  m_cmd_binder.set_handler("mms",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS),
                           tr("Interface with the MMS (Multisig Messaging System)\n"
                              "<subcommand> is one of:\n"
                              "  init, info, signer, list, next, sync, transfer, delete, send, receive, export, note, show, set, help\n"
                              "  send_signer_config, start_auto_config, stop_auto_config, auto_config\n"
                              "Get help about a subcommand with: help mms <subcommand>, or mms help <subcommand>"));
  m_cmd_binder.set_handler("mms init",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_INIT),
                           tr("Initialize and configure the MMS for M/N = number of required signers/number of authorized signers multisig"));
  m_cmd_binder.set_handler("mms info",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_INFO),
                           tr("Display current MMS configuration"));
  m_cmd_binder.set_handler("mms signer",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_SIGNER),
                           tr("Set or modify authorized signer info (single-word label, transport address, Antd address), or list all signers"));
  m_cmd_binder.set_handler("mms list",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_LIST),
                           tr("List all messages"));
  m_cmd_binder.set_handler("mms next",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_NEXT),
                           tr("Evaluate the next possible multisig-related action(s) according to wallet state, and execute or offer for choice\n"
                              "By using 'sync' processing of waiting messages with multisig sync info can be forced regardless of wallet state"));
  m_cmd_binder.set_handler("mms sync",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_SYNC),
                           tr("Force generation of multisig sync info regardless of wallet state, to recover from special situations like \"stale data\" errors"));
  m_cmd_binder.set_handler("mms transfer",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_TRANSFER),
                           tr("Initiate transfer with MMS support; arguments identical to normal 'transfer' command arguments, for info see there"));
  m_cmd_binder.set_handler("mms delete",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_DELETE),
                           tr("Delete a single message by giving its id, or delete all messages by using 'all'"));
  m_cmd_binder.set_handler("mms send",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_SEND),
                           tr("Send a single message by giving its id, or send all waiting messages"));
  m_cmd_binder.set_handler("mms receive",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_RECEIVE),
                           tr("Check right away for new messages to receive"));
  m_cmd_binder.set_handler("mms export",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_EXPORT),
                           tr("Write the content of a message to a file \"mms_message_content\""));
  m_cmd_binder.set_handler("mms note",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_NOTE),
                           tr("Send a one-line message to an authorized signer, identified by its label, or show any waiting unread notes"));
  m_cmd_binder.set_handler("mms show",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_SHOW),
                           tr("Show detailed info about a single message"));
  m_cmd_binder.set_handler("mms set",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_SET),
                           tr("Available options:\n "
                                  "auto-send <1|0>\n "
                                  "  Whether to automatically send newly generated messages right away.\n "));
  m_cmd_binder.set_handler("mms send_message_config",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_SEND_SIGNER_CONFIG),
                           tr("Send completed signer config to all other authorized signers"));
  m_cmd_binder.set_handler("mms start_auto_config",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_START_AUTO_CONFIG),
                           tr("Start auto-config at the auto-config manager's wallet by issuing auto-config tokens and optionally set others' labels"));
  m_cmd_binder.set_handler("mms stop_auto_config",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_STOP_AUTO_CONFIG),
                           tr("Delete any auto-config tokens and abort a auto-config process"));
  m_cmd_binder.set_handler("mms auto_config",
                           boost::bind(&simple_wallet::mms, this, _1),
                           tr(USAGE_MMS_AUTO_CONFIG),
                           tr("Start auto-config by using the token received from the auto-config manager"));
  m_cmd_binder.set_handler("print_ring",
                           boost::bind(&simple_wallet::print_ring, this, _1),
                           tr(USAGE_PRINT_RING),
                           tr("Print the ring(s) used to spend a given key image or transaction (if the ring size is > 1)\n\n"
                              "Output format:\n"
                              "Key Image, \"absolute\", list of rings"));
  m_cmd_binder.set_handler("set_ring",
                           boost::bind(&simple_wallet::set_ring, this, _1),
                           tr(USAGE_SET_RING),
                           tr("Set the ring used for a given key image, so it can be reused in a fork"));
  m_cmd_binder.set_handler("save_known_rings",
                           boost::bind(&simple_wallet::save_known_rings, this, _1),
                           tr(USAGE_SAVE_KNOWN_RINGS),
                           tr("Save known rings to the shared rings database"));
  m_cmd_binder.set_handler("mark_output_spent",
                           boost::bind(&simple_wallet::blackball, this, _1),
                           tr(USAGE_MARK_OUTPUT_SPENT),
                           tr("Mark output(s) as spent so they never get selected as fake outputs in a ring"));
  m_cmd_binder.set_handler("mark_output_unspent",
                           boost::bind(&simple_wallet::unblackball, this, _1),
                           tr(USAGE_MARK_OUTPUT_UNSPENT),
                           tr("Marks an output as unspent so it may get selected as a fake output in a ring"));
  m_cmd_binder.set_handler("is_output_spent",
                           boost::bind(&simple_wallet::blackballed, this, _1),
                           tr(USAGE_IS_OUTPUT_SPENT),
                           tr("Checks whether an output is marked as spent"));
  m_cmd_binder.set_handler("version",
                           boost::bind(&simple_wallet::version, this, _1),
                           tr(USAGE_VERSION),
                           tr("Returns version information"));
  m_cmd_binder.set_handler("help",
                           boost::bind(&simple_wallet::help, this, _1),
                           tr(USAGE_HELP),
                           tr("Show the help section or the documentation about a <command>."));

  //
  // Antd
  //
  m_cmd_binder.set_handler("show_article",
                           boost::bind(&simple_wallet::show_article, this, _1),
                           tr("Show article"));
  m_cmd_binder.set_handler("add_article",
                           boost::bind(&simple_wallet::add_article, this, _1),
                           tr(USAGE_ADD_ARTICLE),
                           tr("Create article"));
  m_cmd_binder.set_handler("register_full_node",
                           boost::bind(&simple_wallet::register_full_node, this, _1),
                           tr(USAGE_REGISTER_FULL_NODE),
                           tr("Send <amount> to this wallet's main account, locked for the required staking time plus a small buffer. If the parameter \"index<N1>[,<N2>,...]\" is specified, the wallet stakes outputs received by those address indices. <priority> is the priority of the stake. The higher the priority, the higher the transaction fee. Valid values in priority order (from lowest to highest) are: unimportant, normal, elevated, priority. If omitted, the default value (see the command \"set priority\") is used."));
  m_cmd_binder.set_handler("stake",
                           boost::bind(&simple_wallet::stake, this, _1),
                           tr(USAGE_STAKE),
                           tr("Send all unlocked balance to an address. If the parameter \"index<N1>[,<N2>,...]\" is specified, the wallet sweeps outputs received by those address indices. If omitted, the wallet randomly chooses an address index to be used. If the parameter \"outputs=<N>\" is specified and  N > 0, wallet splits the transaction into N even outputs."));
  m_cmd_binder.set_handler("request_stake_unlock",
                           boost::bind(&simple_wallet::request_stake_unlock, this, _1),
                           tr(USAGE_REQUEST_STAKE_UNLOCK),
                           tr("Request a stake currently locked in a Full Node to be unlocked on the network"));
  m_cmd_binder.set_handler("print_locked_stakes",
                           boost::bind(&simple_wallet::print_locked_stakes, this, _1),
                           tr(USAGE_PRINT_LOCKED_STAKES),
                           tr("Print stakes currently locked on the Full Node network"));
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_variable(const std::vector<std::string> &args)
{
  if (args.empty())
  {
    std::string seed_language = m_wallet->get_seed_language();
    if (m_use_english_language_names)
      seed_language = crypto::ElectrumWords::get_english_name_for(seed_language);
    std::string priority_string = "invalid";
    uint32_t priority = m_wallet->get_default_priority();
    if (priority < tools::allowed_priority_strings.size())
      priority_string = tools::allowed_priority_strings[priority];
    std::string ask_password_string = "invalid";
    switch (m_wallet->ask_password())
    {
      case tools::wallet2::AskPasswordNever: ask_password_string = "never"; break;
      case tools::wallet2::AskPasswordOnAction: ask_password_string = "action"; break;
      case tools::wallet2::AskPasswordToDecrypt: ask_password_string = "decrypt"; break;
    }
    success_msg_writer() << "seed = " << seed_language;
    success_msg_writer() << "always-confirm-transfers = " << m_wallet->always_confirm_transfers();
    success_msg_writer() << "print-ring-members = " << m_wallet->print_ring_members();
    success_msg_writer() << "store-tx-info = " << m_wallet->store_tx_info();
    success_msg_writer() << "auto-refresh = " << m_wallet->auto_refresh();
    success_msg_writer() << "refresh-type = " << get_refresh_type_name(m_wallet->get_refresh_type());
    success_msg_writer() << "priority = " << priority<< " (" << priority_string << ")";
    success_msg_writer() << "confirm-missing-payment-id = " << m_wallet->confirm_missing_payment_id();
    success_msg_writer() << "ask-password = " << m_wallet->ask_password() << " (" << ask_password_string << ")";
    success_msg_writer() << "unit = " << cryptonote::get_unit(cryptonote::get_default_decimal_point());
    success_msg_writer() << "min-outputs-count = " << m_wallet->get_min_output_count();
    success_msg_writer() << "min-outputs-value = " << cryptonote::print_money(m_wallet->get_min_output_value());
    success_msg_writer() << "merge-destinations = " << m_wallet->merge_destinations();
    success_msg_writer() << "confirm-backlog = " << m_wallet->confirm_backlog();
    success_msg_writer() << "confirm-backlog-threshold = " << m_wallet->get_confirm_backlog_threshold();
    success_msg_writer() << "confirm-export-overwrite = " << m_wallet->confirm_export_overwrite();
    success_msg_writer() << "refresh-from-block-height = " << m_wallet->get_refresh_from_block_height();
    success_msg_writer() << "auto-low-priority = " << m_wallet->auto_low_priority();
    success_msg_writer() << "segregate-pre-fork-outputs = " << m_wallet->segregate_pre_fork_outputs();
    success_msg_writer() << "key-reuse-mitigation2 = " << m_wallet->key_reuse_mitigation2();
    const std::pair<size_t, size_t> lookahead = m_wallet->get_subaddress_lookahead();
    success_msg_writer() << "subaddress-lookahead = " << lookahead.first << ":" << lookahead.second;
    success_msg_writer() << "segregation-height = " << m_wallet->segregation_height();
    success_msg_writer() << "ignore-fractional-outputs = " << m_wallet->ignore_fractional_outputs();
    success_msg_writer() << "track-uses = " << m_wallet->track_uses();
    success_msg_writer() << "device_name = " << m_wallet->device_name();
    return true;
  }
  else
  {

#define CHECK_SIMPLE_VARIABLE(name, f, help) do \
  if (args[0] == name) { \
    if (args.size() <= 1) \
    { \
      fail_msg_writer() << "set " << #name << ": " << tr("needs an argument") << " (" << help << ")"; \
      return true; \
    } \
    else \
    { \
      f(args); \
      return true; \
    } \
  } while(0)

    if (args[0] == "seed")
    {
      if (args.size() == 1)
      {
        fail_msg_writer() << tr("set seed: needs an argument. available options: language");
        return true;
      }
      else if (args[1] == "language")
      {
        seed_set_language(args);
        return true;
      }
    }
    CHECK_SIMPLE_VARIABLE("always-confirm-transfers", set_always_confirm_transfers, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("print-ring-members", set_print_ring_members, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("store-tx-info", set_store_tx_info, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("auto-refresh", set_auto_refresh, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("refresh-type", set_refresh_type, tr("full (slowest, no assumptions); optimize-coinbase (fast, assumes the whole coinbase is paid to a single address); no-coinbase (fastest, assumes we receive no coinbase transaction), default (same as optimize-coinbase)"));
    CHECK_SIMPLE_VARIABLE("priority", set_default_priority, tr("0, 1, 2, 3, or 4, or one of ") << join_priority_strings(", "));
    CHECK_SIMPLE_VARIABLE("confirm-missing-payment-id", set_confirm_missing_payment_id, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("ask-password", set_ask_password, tr("0|1|2 (or never|action|decrypt)"));
    CHECK_SIMPLE_VARIABLE("unit", set_unit, tr("antd, megarok, kilorok, rok"));
    CHECK_SIMPLE_VARIABLE("min-outputs-count", set_min_output_count, tr("unsigned integer"));
    CHECK_SIMPLE_VARIABLE("min-outputs-value", set_min_output_value, tr("amount"));
    CHECK_SIMPLE_VARIABLE("merge-destinations", set_merge_destinations, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("confirm-backlog", set_confirm_backlog, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("confirm-backlog-threshold", set_confirm_backlog_threshold, tr("unsigned integer"));
    CHECK_SIMPLE_VARIABLE("confirm-export-overwrite", set_confirm_export_overwrite, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("refresh-from-block-height", set_refresh_from_block_height, tr("block height"));
    CHECK_SIMPLE_VARIABLE("auto-low-priority", set_auto_low_priority, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("segregate-pre-fork-outputs", set_segregate_pre_fork_outputs, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("key-reuse-mitigation2", set_key_reuse_mitigation2, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("subaddress-lookahead", set_subaddress_lookahead, tr("<major>:<minor>"));
    CHECK_SIMPLE_VARIABLE("segregation-height", set_segregation_height, tr("unsigned integer"));
    CHECK_SIMPLE_VARIABLE("ignore-fractional-outputs", set_ignore_fractional_outputs, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("track-uses", set_track_uses, tr("0 or 1"));
    CHECK_SIMPLE_VARIABLE("device-name", set_device_name, tr("<device_name[:device_spec]>"));
  }
  fail_msg_writer() << tr("set: unrecognized argument(s)");
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_log(const std::vector<std::string> &args)
{
  if(args.size() > 1)
  {
    PRINT_USAGE(USAGE_SET_LOG);
    return true;
  }
  if(!args.empty())
  {
    uint16_t level = 0;
    if(epee::string_tools::get_xtype_from_string(level, args[0]))
    {
      if(4 < level)
      {
        fail_msg_writer() << boost::format(tr("wrong number range, use: %s")) % USAGE_SET_LOG;
        return true;
      }
      mlog_set_log_level(level);
    }
    else
    {
      mlog_set_log(args[0].c_str());
    }
  }
  
  success_msg_writer() << "New log categories: " << mlog_get_categories();
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::ask_wallet_create_if_needed()
{
  LOG_PRINT_L3("simple_wallet::ask_wallet_create_if_needed() started");
  std::string wallet_path;
  std::string confirm_creation;
  bool wallet_name_valid = false;
  bool keys_file_exists;
  bool wallet_file_exists;

  do{
      LOG_PRINT_L3("User asked to specify wallet file name.");
      wallet_path = input_line(
        tr(m_restoring ? "Specify a new wallet file name for your restored wallet (e.g., MyWallet).\n"
        "Wallet file name (or Ctrl-C to quit)" :
        "Specify wallet file name (e.g., MyWallet). If the wallet doesn't exist, it will be created.\n"
        "Wallet file name (or Ctrl-C to quit)")
      );
      if(std::cin.eof())
      {
        LOG_ERROR("Unexpected std::cin.eof() - Exited simple_wallet::ask_wallet_create_if_needed()");
        return false;
      }
      if(!tools::wallet2::wallet_valid_path_format(wallet_path))
      {
        fail_msg_writer() << tr("Wallet name not valid. Please try again or use Ctrl-C to quit.");
        wallet_name_valid = false;
      }
      else
      {
        tools::wallet2::wallet_exists(wallet_path, keys_file_exists, wallet_file_exists);
        LOG_PRINT_L3("wallet_path: " << wallet_path << "");
        LOG_PRINT_L3("keys_file_exists: " << std::boolalpha << keys_file_exists << std::noboolalpha
        << "  wallet_file_exists: " << std::boolalpha << wallet_file_exists << std::noboolalpha);

        if((keys_file_exists || wallet_file_exists) && (!m_generate_new.empty() || m_restoring))
        {
          fail_msg_writer() << tr("Attempting to generate or restore wallet, but specified file(s) exist.  Exiting to not risk overwriting.");
          return false;
        }
        if(wallet_file_exists && keys_file_exists) //Yes wallet, yes keys
        {
          success_msg_writer() << tr("Wallet and key files found, loading...");
          m_wallet_file = wallet_path;
          return true;
        }
        else if(!wallet_file_exists && keys_file_exists) //No wallet, yes keys
        {
          success_msg_writer() << tr("Key file found but not wallet file. Regenerating...");
          m_wallet_file = wallet_path;
          return true;
        }
        else if(wallet_file_exists && !keys_file_exists) //Yes wallet, no keys
        {
          fail_msg_writer() << tr("Key file not found. Failed to open wallet: ") << "\"" << wallet_path << "\". Exiting.";
          return false;
        }
        else if(!wallet_file_exists && !keys_file_exists) //No wallet, no keys
        {
          bool ok = true;
          if (!m_restoring)
          {
            std::string prompt = tr("No wallet found with that name. Confirm creation of new wallet named: ");
            prompt += "\"" + wallet_path + "\"";
            confirm_creation = input_line(prompt, true);
            if(std::cin.eof())
            {
              LOG_ERROR("Unexpected std::cin.eof() - Exited simple_wallet::ask_wallet_create_if_needed()");
              return false;
            }
            ok = command_line::is_yes(confirm_creation);
          }
          if (ok)
          {
            success_msg_writer() << tr("Generating new wallet...");
            m_generate_new = wallet_path;
            return true;
          }
        }
      }
    } while(!wallet_name_valid);

  LOG_ERROR("Failed out of do-while loop in ask_wallet_create_if_needed()");
  return false;
}

/*!
 * \brief Prints the seed with a nice message
 * \param seed seed to print
 */
void simple_wallet::print_seed(const epee::wipeable_string &seed)
{
  success_msg_writer(true) << "\n" << boost::format(tr("NOTE: the following %s can be used to recover access to your wallet. "
    "Write them down and store them somewhere safe and secure. Please do not store them in "
    "your email or on file storage services outside of your immediate control.\n")) % (m_wallet->multisig() ? tr("string") : tr("25 words"));
  // don't log
  int space_index = 0;
  size_t len  = seed.size();
  for (const char *ptr = seed.data(); len--; ++ptr)
  {
    if (*ptr == ' ')
    {
      if (space_index == 15 || space_index == 7)
        putchar('\n');
      else
        putchar(*ptr);
      ++space_index;
    }
    else
      putchar(*ptr);
  }
  putchar('\n');
  fflush(stdout);
}
//----------------------------------------------------------------------------------------------------
static bool might_be_partial_seed(const epee::wipeable_string &words)
{
  std::vector<epee::wipeable_string> seed;

  words.split(seed);
  return seed.size() < 24;
}
//----------------------------------------------------------------------------------------------------
static bool datestr_to_int(const std::string &heightstr, uint16_t &year, uint8_t &month, uint8_t &day)
{
  if (heightstr.size() != 10 || heightstr[4] != '-' || heightstr[7] != '-')
  {
    fail_msg_writer() << tr("date format must be YYYY-MM-DD");
    return false;
  }
  try
  {
    year  = boost::lexical_cast<uint16_t>(heightstr.substr(0,4));
    // lexical_cast<uint8_t> won't work because uint8_t is treated as character type
    month = boost::lexical_cast<uint16_t>(heightstr.substr(5,2));
    day   = boost::lexical_cast<uint16_t>(heightstr.substr(8,2));
  }
  catch (const boost::bad_lexical_cast &)
  {
    fail_msg_writer() << tr("bad height parameter: ") << heightstr;
    return false;
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::init(const boost::program_options::variables_map& vm)
{
  epee::misc_utils::auto_scope_leave_caller scope_exit_handler = epee::misc_utils::create_scope_leave_handler([&](){
    m_electrum_seed.wipe();
  });

  const bool testnet = tools::wallet2::has_testnet_option(vm);
  const bool stagenet = tools::wallet2::has_stagenet_option(vm);
  if (testnet && stagenet)
  {
    fail_msg_writer() << tr("Can't specify more than one of --testnet and --stagenet");
    return false;
  }
  network_type const nettype = testnet ? TESTNET : stagenet ? STAGENET : MAINNET;

  epee::wipeable_string multisig_keys;

  if (!handle_command_line(vm))
    return false;

  if((!m_generate_new.empty()) + (!m_wallet_file.empty()) + (!m_generate_from_device.empty()) + (!m_generate_from_view_key.empty()) + (!m_generate_from_spend_key.empty()) + (!m_generate_from_keys.empty()) + (!m_generate_from_multisig_keys.empty()) + (!m_generate_from_json.empty()) > 1)
  {
    fail_msg_writer() << tr("can't specify more than one of --generate-new-wallet=\"wallet_name\", --wallet-file=\"wallet_name\", --generate-from-view-key=\"wallet_name\", --generate-from-spend-key=\"wallet_name\", --generate-from-keys=\"wallet_name\", --generate-from-multisig-keys=\"wallet_name\", --generate-from-json=\"jsonfilename\" and --generate-from-device=\"wallet_name\"");
    return false;
  }
  else if (m_generate_new.empty() && m_wallet_file.empty() && m_generate_from_device.empty() && m_generate_from_view_key.empty() && m_generate_from_spend_key.empty() && m_generate_from_keys.empty() && m_generate_from_multisig_keys.empty() && m_generate_from_json.empty())
  {
    if(!ask_wallet_create_if_needed()) return false;
  }

  if (!m_generate_new.empty() || m_restoring)
  {
    if (!m_subaddress_lookahead.empty() && !parse_subaddress_lookahead(m_subaddress_lookahead))
      return false;

    std::string old_language;
    // check for recover flag.  if present, require electrum word list (only recovery option for now).
    if (m_restore_deterministic_wallet || m_restore_multisig_wallet)
    {
      if (m_non_deterministic)
      {
        fail_msg_writer() << tr("can't specify both --restore-deterministic-wallet or --restore-multisig-wallet and --non-deterministic");
        return false;
      }
      if (!m_wallet_file.empty())
      {
        if (m_restore_multisig_wallet)
          fail_msg_writer() << tr("--restore-multisig-wallet uses --generate-new-wallet, not --wallet-file");
        else
          fail_msg_writer() << tr("--restore-deterministic-wallet uses --generate-new-wallet, not --wallet-file");
        return false;
      }

      if (m_electrum_seed.empty())
      {
        if (m_restore_multisig_wallet)
        {
            const char *prompt = "Specify multisig seed";
            m_electrum_seed = input_secure_line(prompt);
            if (std::cin.eof())
              return false;
            if (m_electrum_seed.empty())
            {
              fail_msg_writer() << tr("specify a recovery parameter with the --electrum-seed=\"multisig seed here\"");
              return false;
            }
        }
        else
        {
          m_electrum_seed = "";
          do
          {
            const char *prompt = m_electrum_seed.empty() ? "Specify Electrum seed" : "Electrum seed continued";
            epee::wipeable_string electrum_seed = input_secure_line(prompt);
            if (std::cin.eof())
              return false;
            if (electrum_seed.empty())
            {
              fail_msg_writer() << tr("specify a recovery parameter with the --electrum-seed=\"words list here\"");
              return false;
            }
            m_electrum_seed += electrum_seed;
            m_electrum_seed += ' ';
          } while (might_be_partial_seed(m_electrum_seed));
        }
      }

      if (m_restore_multisig_wallet)
      {
        const boost::optional<epee::wipeable_string> parsed = m_electrum_seed.parse_hexstr();
        if (!parsed)
        {
          fail_msg_writer() << tr("Multisig seed failed verification");
          return false;
        }
        multisig_keys = *parsed;
      }
      else
      {
        if (!crypto::ElectrumWords::words_to_bytes(m_electrum_seed, m_recovery_key, old_language))
        {
          fail_msg_writer() << tr("Electrum-style word list failed verification");
          return false;
        }
      }

      auto pwd_container = password_prompter(tr("Enter seed offset passphrase, empty if none"), false);
      if (std::cin.eof() || !pwd_container)
        return false;
      epee::wipeable_string seed_pass = pwd_container->password();
      if (!seed_pass.empty())
      {
        if (m_restore_multisig_wallet)
        {
          crypto::secret_key key;
          crypto::cn_slow_hash(seed_pass.data(), seed_pass.size(), (crypto::hash&)key);
          sc_reduce32((unsigned char*)key.data);
          multisig_keys = m_wallet->decrypt<epee::wipeable_string>(std::string(multisig_keys.data(), multisig_keys.size()), key, true);
        }
        else
          m_recovery_key = cryptonote::decrypt_key(m_recovery_key, seed_pass);
      }
    }
    epee::wipeable_string password;
    if (!m_generate_from_view_key.empty())
    {
      m_wallet_file = m_generate_from_view_key;
      // parse address
      std::string address_string = input_line("Standard address");
      if (std::cin.eof())
        return false;
      if (address_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      cryptonote::address_parse_info info;
      if(!get_account_address_from_str(info, nettype, address_string))
      {
          fail_msg_writer() << tr("failed to parse address");
          return false;
      }
      if (info.is_subaddress)
      {
        fail_msg_writer() << tr("This address is a subaddress which cannot be used here.");
        return false;
      }

      // parse view secret key
      epee::wipeable_string viewkey_string = input_secure_line("Secret view key");
      if (std::cin.eof())
        return false;
      if (viewkey_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      crypto::secret_key viewkey;
      if (!viewkey_string.hex_to_pod(unwrap(unwrap(viewkey))))
      {
        fail_msg_writer() << tr("failed to parse view key secret key");
        return false;
      }

      m_wallet_file=m_generate_from_view_key;

      // check the view key matches the given address
      crypto::public_key pkey;
      if (!crypto::secret_key_to_public_key(viewkey, pkey)) {
        fail_msg_writer() << tr("failed to verify view key secret key");
        return false;
      }
      if (info.address.m_view_public_key != pkey) {
        fail_msg_writer() << tr("view key does not match standard address");
        return false;
      }

      auto r = new_wallet(vm, info.address, boost::none, viewkey);
      CHECK_AND_ASSERT_MES(r, false, tr("account creation failed"));
      password = *r;
    }
    else if (!m_generate_from_spend_key.empty())
    {
      m_wallet_file = m_generate_from_spend_key;
      // parse spend secret key
      epee::wipeable_string spendkey_string = input_secure_line("Secret spend key");
      if (std::cin.eof())
        return false;
      if (spendkey_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      if (!spendkey_string.hex_to_pod(unwrap(unwrap(m_recovery_key))))
      {
        fail_msg_writer() << tr("failed to parse spend key secret key");
        return false;
      }
      auto r = new_wallet(vm, m_recovery_key, true, false, "");
      CHECK_AND_ASSERT_MES(r, false, tr("account creation failed"));
      password = *r;
    }
    else if (!m_generate_from_keys.empty())
    {
      m_wallet_file = m_generate_from_keys;
      // parse address
      std::string address_string = input_line("Standard address");
      if (std::cin.eof())
        return false;
      if (address_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      cryptonote::address_parse_info info;
      if(!get_account_address_from_str(info, nettype, address_string))
      {
          fail_msg_writer() << tr("failed to parse address");
          return false;
      }
      if (info.is_subaddress)
      {
        fail_msg_writer() << tr("This address is a subaddress which cannot be used here.");
        return false;
      }

      // parse spend secret key
      epee::wipeable_string spendkey_string = input_secure_line("Secret spend key");
      if (std::cin.eof())
        return false;
      if (spendkey_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      crypto::secret_key spendkey;
      if (!spendkey_string.hex_to_pod(unwrap(unwrap(spendkey))))
      {
        fail_msg_writer() << tr("failed to parse spend key secret key");
        return false;
      }

      // parse view secret key
      epee::wipeable_string viewkey_string = input_secure_line("Secret view key");
      if (std::cin.eof())
        return false;
      if (viewkey_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      crypto::secret_key viewkey;
      if(!viewkey_string.hex_to_pod(unwrap(unwrap(viewkey))))
      {
        fail_msg_writer() << tr("failed to parse view key secret key");
        return false;
      }

      m_wallet_file=m_generate_from_keys;

      // check the spend and view keys match the given address
      crypto::public_key pkey;
      if (!crypto::secret_key_to_public_key(spendkey, pkey)) {
        fail_msg_writer() << tr("failed to verify spend key secret key");
        return false;
      }
      if (info.address.m_spend_public_key != pkey) {
        fail_msg_writer() << tr("spend key does not match standard address");
        return false;
      }
      if (!crypto::secret_key_to_public_key(viewkey, pkey)) {
        fail_msg_writer() << tr("failed to verify view key secret key");
        return false;
      }
      if (info.address.m_view_public_key != pkey) {
        fail_msg_writer() << tr("view key does not match standard address");
        return false;
      }
      auto r = new_wallet(vm, info.address, spendkey, viewkey);
      CHECK_AND_ASSERT_MES(r, false, tr("account creation failed"));
      password = *r;
    }
    
    // Asks user for all the data required to merge secret keys from multisig wallets into one master wallet, which then gets full control of the multisig wallet. The resulting wallet will be the same as any other regular wallet.
    else if (!m_generate_from_multisig_keys.empty())
    {
      m_wallet_file = m_generate_from_multisig_keys;
      unsigned int multisig_m;
      unsigned int multisig_n;
      
      // parse multisig type
      std::string multisig_type_string = input_line("Multisig type (input as M/N with M <= N and M > 1)");
      if (std::cin.eof())
        return false;
      if (multisig_type_string.empty())
      {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      if (sscanf(multisig_type_string.c_str(), "%u/%u", &multisig_m, &multisig_n) != 2)
      {
        fail_msg_writer() << tr("Error: expected M/N, but got: ") << multisig_type_string;
        return false;
      }
      if (multisig_m <= 1 || multisig_m > multisig_n)
      {
        fail_msg_writer() << tr("Error: expected N > 1 and N <= M, but got: ") << multisig_type_string;
        return false;
      }
      if (multisig_m != multisig_n)
      {
        fail_msg_writer() << tr("Error: M/N is currently unsupported. ");
        return false;
      }      
      message_writer() << boost::format(tr("Generating master wallet from %u of %u multisig wallet keys")) % multisig_m % multisig_n;
      
      // parse multisig address
      std::string address_string = input_line("Multisig wallet address");
      if (std::cin.eof())
        return false;
      if (address_string.empty()) {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      cryptonote::address_parse_info info;
      if(!get_account_address_from_str(info, nettype, address_string))
      {
          fail_msg_writer() << tr("failed to parse address");
          return false;
      }
      
      // parse secret view key
      epee::wipeable_string viewkey_string = input_secure_line("Secret view key");
      if (std::cin.eof())
        return false;
      if (viewkey_string.empty())
      {
        fail_msg_writer() << tr("No data supplied, cancelled");
        return false;
      }
      crypto::secret_key viewkey;
      if(!viewkey_string.hex_to_pod(unwrap(unwrap(viewkey))))
      {
        fail_msg_writer() << tr("failed to parse secret view key");
        return false;
      }
      
      // check that the view key matches the given address
      crypto::public_key pkey;
      if (!crypto::secret_key_to_public_key(viewkey, pkey))
      {
        fail_msg_writer() << tr("failed to verify secret view key");
        return false;
      }
      if (info.address.m_view_public_key != pkey)
      {
        fail_msg_writer() << tr("view key does not match standard address");
        return false;
      }
      
      // parse multisig spend keys
      crypto::secret_key spendkey;
      // parsing N/N
      if(multisig_m == multisig_n)
      {
        std::vector<crypto::secret_key> multisig_secret_spendkeys(multisig_n);
        epee::wipeable_string spendkey_string;
        cryptonote::blobdata spendkey_data;
        // get N secret spend keys from user
        for(unsigned int i=0; i<multisig_n; ++i)
        {
          spendkey_string = input_secure_line(tr((boost::format(tr("Secret spend key (%u of %u)")) % (i+1) % multisig_m).str().c_str()));
          if (std::cin.eof())
            return false;
          if (spendkey_string.empty())
          {
            fail_msg_writer() << tr("No data supplied, cancelled");
            return false;
          }
          if(!spendkey_string.hex_to_pod(unwrap(unwrap(multisig_secret_spendkeys[i]))))
          {
            fail_msg_writer() << tr("failed to parse spend key secret key");
            return false;
          }
        }
        
        // sum the spend keys together to get the master spend key
        spendkey = multisig_secret_spendkeys[0];
        for(unsigned int i=1; i<multisig_n; ++i)
          sc_add(reinterpret_cast<unsigned char*>(&spendkey), reinterpret_cast<unsigned char*>(&spendkey), reinterpret_cast<unsigned char*>(&multisig_secret_spendkeys[i]));
      }
      // parsing M/N
      else
      {
        fail_msg_writer() << tr("Error: M/N is currently unsupported");
        return false;
      }
      
      // check that the spend key matches the given address
      if (!crypto::secret_key_to_public_key(spendkey, pkey))
      {
        fail_msg_writer() << tr("failed to verify spend key secret key");
        return false;
      }
      if (info.address.m_spend_public_key != pkey)
      {
        fail_msg_writer() << tr("spend key does not match standard address");
        return false;
      }
      
      // create wallet
      auto r = new_wallet(vm, info.address, spendkey, viewkey);
      CHECK_AND_ASSERT_MES(r, false, tr("account creation failed"));
      password = *r;
    }
    
    else if (!m_generate_from_json.empty())
    {
      try
      {
        auto rc = tools::wallet2::make_from_json(vm, false, m_generate_from_json, password_prompter);
        m_wallet = std::move(rc.first);
        password = rc.second.password();
        m_wallet_file = m_wallet->path();
      }
      catch (const std::exception &e)
      {
        fail_msg_writer() << e.what();
        return false;
      }
      if (!m_wallet)
        return false;
    }
    else if (!m_generate_from_device.empty())
    {
      m_wallet_file = m_generate_from_device;
      // create wallet
      auto r = new_wallet(vm);
      CHECK_AND_ASSERT_MES(r, false, tr("account creation failed"));
      password = *r;
      // if no block_height is specified, assume its a new account and start it "now"
      if(m_wallet->get_refresh_from_block_height() == 0) {
        {
          tools::scoped_message_writer wrt = tools::msg_writer();
          wrt << tr("No restore height is specified.") << " ";
          wrt << tr("Assumed you are creating a new account, restore will be done from current estimated blockchain height.") << " ";
          wrt << tr("Use --restore-height or --restore-date if you want to restore an already setup account from a specific height.");
        }
        std::string confirm = input_line(tr("Is this okay?"), true);
        if (std::cin.eof() || !command_line::is_yes(confirm))
          CHECK_AND_ASSERT_MES(false, false, tr("account creation aborted"));

        m_wallet->set_refresh_from_block_height(m_wallet->estimate_blockchain_height());
        m_wallet->explicit_refresh_from_block_height(true);
        m_restore_height = m_wallet->get_refresh_from_block_height();
      }
    }
    else
    {
      if (m_generate_new.empty()) {
        fail_msg_writer() << tr("specify a wallet path with --generate-new-wallet (not --wallet-file)");
        return false;
      }
      m_wallet_file = m_generate_new;
      boost::optional<epee::wipeable_string> r;
      if (m_restore_multisig_wallet)
        r = new_wallet(vm, multisig_keys, old_language);
      else
        r = new_wallet(vm, m_recovery_key, m_restore_deterministic_wallet, m_non_deterministic, old_language);
      CHECK_AND_ASSERT_MES(r, false, tr("account creation failed"));
      password = *r;
    }

    if (m_restoring && m_generate_from_json.empty() && m_generate_from_device.empty())
    {
      m_wallet->explicit_refresh_from_block_height(!(command_line::is_arg_defaulted(vm, arg_restore_height) ||
        command_line::is_arg_defaulted(vm, arg_restore_date)));
      if (command_line::is_arg_defaulted(vm, arg_restore_height) && !command_line::is_arg_defaulted(vm, arg_restore_date))
      {
        uint16_t year;
        uint8_t month;
        uint8_t day;
        if (!datestr_to_int(m_restore_date, year, month, day))
          return false;
        try
        {
          m_restore_height = m_wallet->get_blockchain_height_by_date(year, month, day);
          success_msg_writer() << tr("Restore height is: ") << m_restore_height;
        }
        catch (const std::runtime_error& e)
        {
          fail_msg_writer() << e.what();
          return false;
        }
      }
    }
    if (!m_wallet->explicit_refresh_from_block_height() && m_restoring)
    {
      uint32_t version;
      bool connected = try_connect_to_daemon(false, &version);
      while (true)
      {
        std::string heightstr;
        if (!connected || version < MAKE_CORE_RPC_VERSION(1, 6))
          heightstr = input_line("Restore from specific blockchain height (optional, default 0)");
        else
          heightstr = input_line("Restore from specific blockchain height (optional, default 0),\nor alternatively from specific date (YYYY-MM-DD)");
        if (std::cin.eof())
          return false;
        if (heightstr.empty())
        {
          m_restore_height = 0;
          break;
        }
        try
        {
          m_restore_height = boost::lexical_cast<uint64_t>(heightstr);
          break;
        }
        catch (const boost::bad_lexical_cast &)
        {
          if (!connected || version < MAKE_CORE_RPC_VERSION(1, 6))
          {
            fail_msg_writer() << tr("bad m_restore_height parameter: ") << heightstr;
            continue;
          }
          uint16_t year;
          uint8_t month;  // 1, 2, ..., 12
          uint8_t day;    // 1, 2, ..., 31
          try
          {
            if (!datestr_to_int(heightstr, year, month, day))
              return false;
            m_restore_height = m_wallet->get_blockchain_height_by_date(year, month, day);
            success_msg_writer() << tr("Restore height is: ") << m_restore_height;
            std::string confirm = input_line(tr("Is this okay?"), true);
            if (std::cin.eof())
              return false;
            if(command_line::is_yes(confirm))
              break;
          }
          catch (const boost::bad_lexical_cast &)
          {
            fail_msg_writer() << tr("bad m_restore_height parameter: ") << heightstr;
          }
          catch (const std::runtime_error& e)
          {
            fail_msg_writer() << e.what();
          }
        }
      }
    }
    if (m_restoring)
    {
      uint64_t estimate_height = m_wallet->estimate_blockchain_height();
      if (m_restore_height >= estimate_height)
      {
        success_msg_writer() << tr("Restore height ") << m_restore_height << (" is not yet reached. The current estimated height is ") << estimate_height;
        std::string confirm = input_line(tr("Still apply restore height?"), true);
        if (std::cin.eof() || command_line::is_no(confirm))
          m_restore_height = 0;
      }
      m_wallet->set_refresh_from_block_height(m_restore_height);
    }
    m_wallet->rewrite(m_wallet_file, password);
  }
  else
  {
    assert(!m_wallet_file.empty());
    if (!m_subaddress_lookahead.empty())
    {
      fail_msg_writer() << tr("can't specify --subaddress-lookahead and --wallet-file at the same time");
      return false;
    }
    bool r = open_wallet(vm);
    CHECK_AND_ASSERT_MES(r, false, tr("failed to open account"));
  }
  if (!m_wallet)
  {
    fail_msg_writer() << tr("wallet is null");
    return false;
  }

  if (!m_wallet->is_trusted_daemon())
    message_writer() << (boost::format(tr("Warning: using an untrusted daemon at %s, privacy will be lessened")) % m_wallet->get_daemon_address()).str();

  if (m_wallet->get_ring_database().empty())
    fail_msg_writer() << tr("Failed to initialize ring database: privacy enhancing features will be inactive");

  m_wallet->callback(this);

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::deinit()
{
  if (!m_wallet.get())
    return true;

  return close_wallet();
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::handle_command_line(const boost::program_options::variables_map& vm)
{
  m_wallet_file                   = command_line::get_arg(vm, arg_wallet_file);
  m_generate_new                  = command_line::get_arg(vm, arg_generate_new_wallet);
  m_generate_from_device          = command_line::get_arg(vm, arg_generate_from_device);
  m_generate_from_view_key        = command_line::get_arg(vm, arg_generate_from_view_key);
  m_generate_from_spend_key       = command_line::get_arg(vm, arg_generate_from_spend_key);
  m_generate_from_keys            = command_line::get_arg(vm, arg_generate_from_keys);
  m_generate_from_multisig_keys   = command_line::get_arg(vm, arg_generate_from_multisig_keys);
  m_generate_from_json            = command_line::get_arg(vm, arg_generate_from_json);
  m_mnemonic_language             = command_line::get_arg(vm, arg_mnemonic_language);
  m_electrum_seed                 = command_line::get_arg(vm, arg_electrum_seed);
  m_restore_deterministic_wallet  = command_line::get_arg(vm, arg_restore_deterministic_wallet);
  m_restore_multisig_wallet       = command_line::get_arg(vm, arg_restore_multisig_wallet);
  m_non_deterministic             = command_line::get_arg(vm, arg_non_deterministic);
  m_allow_mismatched_daemon_version = command_line::get_arg(vm, arg_allow_mismatched_daemon_version);
  m_restore_height                = command_line::get_arg(vm, arg_restore_height);
  m_restore_date                  = command_line::get_arg(vm, arg_restore_date);
  m_do_not_relay                  = command_line::get_arg(vm, arg_do_not_relay);
  m_subaddress_lookahead          = command_line::get_arg(vm, arg_subaddress_lookahead);
  m_use_english_language_names    = command_line::get_arg(vm, arg_use_english_language_names);
  m_long_payment_id_support       = command_line::get_arg(vm, arg_long_payment_id_support);
  m_restoring                     = !m_generate_from_view_key.empty() ||
                                    !m_generate_from_spend_key.empty() ||
                                    !m_generate_from_keys.empty() ||
                                    !m_generate_from_multisig_keys.empty() ||
                                    !m_generate_from_json.empty() ||
                                    !m_generate_from_device.empty() ||
                                    m_restore_deterministic_wallet ||
                                    m_restore_multisig_wallet;

  if (!command_line::is_arg_defaulted(vm, arg_restore_date))
  {
    uint16_t year;
    uint8_t month, day;
    if (!datestr_to_int(m_restore_date, year, month, day))
      return false;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::try_connect_to_daemon(bool silent, uint32_t* version)
{
  uint32_t version_ = 0;
  if (!version)
    version = &version_;
  if (!m_wallet->check_connection(version))
  {
    if (!silent)
      fail_msg_writer() << tr("wallet failed to connect to daemon: ") << m_wallet->get_daemon_address() << ". " <<
        tr("Daemon either is not started or wrong port was passed. "
        "Please make sure daemon is running or change the daemon address using the 'set_daemon' command.");
    return false;
  }
  if (!m_allow_mismatched_daemon_version && ((*version >> 16) != CORE_RPC_VERSION_MAJOR))
  {
    if (!silent)
      fail_msg_writer() << boost::format(tr("Daemon uses a different RPC major version (%u) than the wallet (%u): %s. Either update one of them, or use --allow-mismatched-daemon-version.")) % (*version>>16) % CORE_RPC_VERSION_MAJOR % m_wallet->get_daemon_address();
    return false;
  }
  return true;
}

/*!
 * \brief Gets the word seed language from the user.
 * 
 * User is asked to choose from a list of supported languages.
 * 
 * \return The chosen language.
 */
std::string simple_wallet::get_mnemonic_language()
{
  std::vector<std::string> language_list_self, language_list_english;
  const std::vector<std::string> &language_list = m_use_english_language_names ? language_list_english : language_list_self;
  std::string language_choice;
  int language_number = -1;
  crypto::ElectrumWords::get_language_list(language_list_self, false);
  crypto::ElectrumWords::get_language_list(language_list_english, true);
  std::cout << tr("List of available languages for your wallet's seed:") << std::endl;
  std::cout << tr("If your display freezes, exit blind with ^C, then run again with --use-english-language-names") << std::endl;
  int ii;
  std::vector<std::string>::const_iterator it;
  for (it = language_list.begin(), ii = 0; it != language_list.end(); it++, ii++)
  {
    std::cout << ii << " : " << *it << std::endl;
  }
  while (language_number < 0)
  {
    language_choice = input_line(tr("Enter the number corresponding to the language of your choice"));
    if (std::cin.eof())
      return std::string();
    try
    {
      language_number = std::stoi(language_choice);
      if (!((language_number >= 0) && (static_cast<unsigned int>(language_number) < language_list.size())))
      {
        language_number = -1;
        fail_msg_writer() << tr("invalid language choice entered. Please try again.\n");
      }
    }
    catch (const std::exception &e)
    {
      fail_msg_writer() << tr("invalid language choice entered. Please try again.\n");
    }
  }
  return language_list_self[language_number];
}
//----------------------------------------------------------------------------------------------------
boost::optional<tools::password_container> simple_wallet::get_and_verify_password() const
{
  auto pwd_container = default_password_prompter(m_wallet_file.empty());
  if (!pwd_container)
    return boost::none;

  if (!m_wallet->verify_password(pwd_container->password()))
  {
    fail_msg_writer() << tr("invalid password");
    return boost::none;
  }
  return pwd_container;
}
//----------------------------------------------------------------------------------------------------
boost::optional<epee::wipeable_string> simple_wallet::new_wallet(const boost::program_options::variables_map& vm,
  const crypto::secret_key& recovery_key, bool recover, bool two_random, const std::string &old_language)
{
  auto rc = tools::wallet2::make_new(vm, false, password_prompter);
  m_wallet = std::move(rc.first);
  if (!m_wallet)
  {
    return {};
  }
  epee::wipeable_string password = rc.second.password();

  if (!m_subaddress_lookahead.empty())
  {
    auto lookahead = parse_subaddress_lookahead(m_subaddress_lookahead);
    assert(lookahead);
    m_wallet->set_subaddress_lookahead(lookahead->first, lookahead->second);
  }

  bool was_deprecated_wallet = m_restore_deterministic_wallet && ((old_language == crypto::ElectrumWords::old_language_name) ||
    crypto::ElectrumWords::get_is_old_style_seed(m_electrum_seed));

  std::string mnemonic_language = old_language;

  std::vector<std::string> language_list;
  crypto::ElectrumWords::get_language_list(language_list);
  if (mnemonic_language.empty() && std::find(language_list.begin(), language_list.end(), m_mnemonic_language) != language_list.end())
  {
    mnemonic_language = m_mnemonic_language;
  }

  // Ask for seed language if:
  // it's a deterministic wallet AND
  // a seed language is not already specified AND
  // (it is not a wallet restore OR if it was a deprecated wallet
  // that was earlier used before this restore)
  if ((!two_random) && (mnemonic_language.empty() || mnemonic_language == crypto::ElectrumWords::old_language_name) && (!m_restore_deterministic_wallet || was_deprecated_wallet))
  {
    if (was_deprecated_wallet)
    {
      // The user had used an older version of the wallet with old style mnemonics.
      message_writer(console_color_green, false) << "\n" << tr("You had been using "
        "a deprecated version of the wallet. Please use the new seed that we provide.\n");
    }
    mnemonic_language = get_mnemonic_language();
    if (mnemonic_language.empty())
      return {};
  }

  m_wallet->set_seed_language(mnemonic_language);

  bool create_address_file = command_line::get_arg(vm, arg_create_address_file);

  crypto::secret_key recovery_val;
  try
  {
    recovery_val = m_wallet->generate(m_wallet_file, std::move(rc.second).password(), recovery_key, recover, two_random, create_address_file);
    message_writer(console_color_white, true) << tr("Generated new wallet: ")
      << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
    PAUSE_READLINE();
    std::cout << tr("View key: ");
    print_secret_key(m_wallet->get_account().get_keys().m_view_secret_key);
    putchar('\n');
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << tr("failed to generate new wallet: ") << e.what();
    return {};
  }

  // convert rng value to electrum-style word list
  epee::wipeable_string electrum_words;

  crypto::ElectrumWords::bytes_to_words(recovery_val, electrum_words, mnemonic_language);

  success_msg_writer() <<
    "**********************************************************************\n" <<
    tr("Your wallet has been generated!\n"
    "To start synchronizing with the daemon, use the \"refresh\" command.\n"
    "Use the \"help\" command to see the list of available commands.\n"
    "Use \"help <command>\" to see a command's documentation.\n"
    "Always use the \"exit\" command when closing antd-wallet-cli to save \n"
    "your current session's state. Otherwise, you might need to synchronize \n"
    "your wallet again (your wallet keys are NOT at risk in any case).\n")
  ;

  if (!two_random)
  {
    print_seed(electrum_words);
  }
  success_msg_writer() << "**********************************************************************";

  return password;
}
//----------------------------------------------------------------------------------------------------
boost::optional<epee::wipeable_string> simple_wallet::new_wallet(const boost::program_options::variables_map& vm,
  const cryptonote::account_public_address& address, const boost::optional<crypto::secret_key>& spendkey,
  const crypto::secret_key& viewkey)
{
  auto rc = tools::wallet2::make_new(vm, false, password_prompter);
  m_wallet = std::move(rc.first);
  if (!m_wallet)
  {
    return {};
  }
  epee::wipeable_string password = rc.second.password();

  if (!m_subaddress_lookahead.empty())
  {
    auto lookahead = parse_subaddress_lookahead(m_subaddress_lookahead);
    assert(lookahead);
    m_wallet->set_subaddress_lookahead(lookahead->first, lookahead->second);
  }

  if (m_restore_height)
    m_wallet->set_refresh_from_block_height(m_restore_height);

  bool create_address_file = command_line::get_arg(vm, arg_create_address_file);

  try
  {
    if (spendkey)
    {
      m_wallet->generate(m_wallet_file, std::move(rc.second).password(), address, *spendkey, viewkey, create_address_file);
    }
    else
    {
      m_wallet->generate(m_wallet_file, std::move(rc.second).password(), address, viewkey, create_address_file);
    }
    message_writer(console_color_white, true) << tr("Generated new wallet: ")
      << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << tr("failed to generate new wallet: ") << e.what();
    return {};
  }


  return password;
}

//----------------------------------------------------------------------------------------------------
boost::optional<epee::wipeable_string> simple_wallet::new_wallet(const boost::program_options::variables_map& vm)
{
  auto rc = tools::wallet2::make_new(vm, false, password_prompter);
  m_wallet = std::move(rc.first);
  m_wallet->callback(this);
  if (!m_wallet)
  {
    return {};
  }
  epee::wipeable_string password = rc.second.password();

  if (!m_subaddress_lookahead.empty())
  {
    auto lookahead = parse_subaddress_lookahead(m_subaddress_lookahead);
    assert(lookahead);
    m_wallet->set_subaddress_lookahead(lookahead->first, lookahead->second);
  }

  if (m_restore_height)
    m_wallet->set_refresh_from_block_height(m_restore_height);

  auto device_desc = tools::wallet2::device_name_option(vm);
  auto device_derivation_path = tools::wallet2::device_derivation_path_option(vm);
  try
  {
    bool create_address_file = command_line::get_arg(vm, arg_create_address_file);
    m_wallet->device_derivation_path(device_derivation_path);
    m_wallet->restore(m_wallet_file, std::move(rc.second).password(), device_desc.empty() ? "Ledger" : device_desc, create_address_file);
    message_writer(console_color_white, true) << tr("Generated new wallet on hw device: ")
      << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << tr("failed to generate new wallet: ") << e.what();
    return {};
  }

  return password;
}
//----------------------------------------------------------------------------------------------------
boost::optional<epee::wipeable_string> simple_wallet::new_wallet(const boost::program_options::variables_map& vm,
    const epee::wipeable_string &multisig_keys, const std::string &old_language)
{
  auto rc = tools::wallet2::make_new(vm, false, password_prompter);
  m_wallet = std::move(rc.first);
  if (!m_wallet)
  {
    return {};
  }
  epee::wipeable_string password = rc.second.password();

  if (!m_subaddress_lookahead.empty())
  {
    auto lookahead = parse_subaddress_lookahead(m_subaddress_lookahead);
    assert(lookahead);
    m_wallet->set_subaddress_lookahead(lookahead->first, lookahead->second);
  }

  std::string mnemonic_language = old_language;

  std::vector<std::string> language_list;
  crypto::ElectrumWords::get_language_list(language_list);
  if (mnemonic_language.empty() && std::find(language_list.begin(), language_list.end(), m_mnemonic_language) != language_list.end())
  {
    mnemonic_language = m_mnemonic_language;
  }

  m_wallet->set_seed_language(mnemonic_language);

  bool create_address_file = command_line::get_arg(vm, arg_create_address_file);

  try
  {
    m_wallet->generate(m_wallet_file, std::move(rc.second).password(), multisig_keys, create_address_file);
    bool ready;
    uint32_t threshold, total;
    if (!m_wallet->multisig(&ready, &threshold, &total) || !ready)
    {
      fail_msg_writer() << tr("failed to generate new mutlisig wallet");
      return {};
    }
    message_writer(console_color_white, true) << boost::format(tr("Generated new %u/%u multisig wallet: ")) % threshold % total
      << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << tr("failed to generate new wallet: ") << e.what();
    return {};
  }

  return password;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::open_wallet(const boost::program_options::variables_map& vm)
{
  if (!tools::wallet2::wallet_valid_path_format(m_wallet_file))
  {
    fail_msg_writer() << tr("wallet file path not valid: ") << m_wallet_file;
    return false;
  }

  bool keys_file_exists;
  bool wallet_file_exists;

  tools::wallet2::wallet_exists(m_wallet_file, keys_file_exists, wallet_file_exists);
  if(!keys_file_exists)
  {
    fail_msg_writer() << tr("Key file not found. Failed to open wallet");
    return false;
  }
  
  epee::wipeable_string password;
  try
  {
    auto rc = tools::wallet2::make_from_file(vm, false, "", password_prompter);
    m_wallet = std::move(rc.first);
    password = std::move(std::move(rc.second).password());
    if (!m_wallet)
    {
      return false;
    }

    m_wallet->callback(this);
    m_wallet->load(m_wallet_file, password);
    std::string prefix;
    bool ready;
    uint32_t threshold, total;
    if (m_wallet->watch_only())
      prefix = tr("Opened watch-only wallet");
    else if (m_wallet->multisig(&ready, &threshold, &total))
      prefix = (boost::format(tr("Opened %u/%u multisig wallet%s")) % threshold % total % (ready ? "" : " (not yet finalized)")).str();
    else
      prefix = tr("Opened wallet");
    message_writer(console_color_white, true) <<
      prefix << ": " << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
    if (m_wallet->get_account().get_device()) {
       message_writer(console_color_white, true) << "Wallet is on device: " << m_wallet->get_account().get_device().get_name();
    }
    // If the wallet file is deprecated, we should ask for mnemonic language again and store
    // everything in the new format.
    // NOTE: this is_deprecated() refers to the wallet file format before becoming JSON. It does not refer to the "old english" seed words form of "deprecated" used elsewhere.
    if (m_wallet->is_deprecated())
    {
      bool is_deterministic;
      {
        SCOPED_WALLET_UNLOCK();
        is_deterministic = m_wallet->is_deterministic();
      }
      if (is_deterministic)
      {
        message_writer(console_color_green, false) << "\n" << tr("You had been using "
          "a deprecated version of the wallet. Please proceed to upgrade your wallet.\n");
        std::string mnemonic_language = get_mnemonic_language();
        if (mnemonic_language.empty())
          return false;
        m_wallet->set_seed_language(mnemonic_language);
        m_wallet->rewrite(m_wallet_file, password);

        // Display the seed
        epee::wipeable_string seed;
        m_wallet->get_seed(seed);
        print_seed(seed);
      }
      else
      {
        message_writer(console_color_green, false) << "\n" << tr("You had been using "
          "a deprecated version of the wallet. Your wallet file format is being upgraded now.\n");
        m_wallet->rewrite(m_wallet_file, password);
      }
    }
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << tr("failed to load wallet: ") << e.what();
    if (m_wallet)
    {
      // only suggest removing cache if the password was actually correct
      bool password_is_correct = false;
      try
      {
        password_is_correct = m_wallet->verify_password(password);
      }
      catch (...) { } // guard against I/O errors
      if (password_is_correct)
        fail_msg_writer() << boost::format(tr("You may want to remove the file \"%s\" and try again")) % m_wallet_file;
    }
    return false;
  }
  success_msg_writer() <<
    "**********************************************************************\n" <<
    tr("Use the \"help\" command to see the list of available commands.\n") <<
    tr("Use \"help <command>\" to see a command's documentation.\n") <<
    "**********************************************************************";
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::close_wallet()
{
  if (m_idle_run.load(std::memory_order_relaxed))
  {
    m_idle_run.store(false, std::memory_order_relaxed);
    m_wallet->stop();
    {
      boost::unique_lock<boost::mutex> lock(m_idle_mutex);
      m_idle_cond.notify_one();
    }
    m_idle_thread.join();
  }

  bool r = m_wallet->deinit();
  if (!r)
  {
    fail_msg_writer() << tr("failed to deinitialize wallet");
    return false;
  }

  try
  {
    m_wallet->store();
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << e.what();
    return false;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::save(const std::vector<std::string> &args)
{
  try
  {
    LOCK_IDLE_SCOPE();
    m_wallet->store();
    success_msg_writer() << tr("Wallet data saved");
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << e.what();
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::save_watch_only(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if (m_wallet->multisig())
  {
    fail_msg_writer() << tr("wallet is multisig and cannot save a watch-only version");
    return true;
  }

  const auto pwd_container = password_prompter(tr("Password for new watch-only wallet"), true);

  if (!pwd_container)
  {
    fail_msg_writer() << tr("failed to read wallet password");
    return true;
  }

  try
  {
    std::string new_keys_filename;
    m_wallet->write_watch_only_wallet(m_wallet_file, pwd_container->password(), new_keys_filename);
    success_msg_writer() << tr("Watch only wallet saved as: ") << new_keys_filename;
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to save watch only wallet: ") << e.what();
    return true;
  }
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::start_mining(const std::vector<std::string>& args)
{
  if (!m_wallet->is_trusted_daemon())
  {
    fail_msg_writer() << tr("this command requires a trusted daemon. Enable with --trusted-daemon");
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  if (!m_wallet)
  {
    fail_msg_writer() << tr("wallet is null");
    return true;
  }
  COMMAND_RPC_START_MINING::request req = AUTO_VAL_INIT(req); 
  req.miner_address = m_wallet->get_account().get_public_address_str(m_wallet->nettype());

  bool ok = true;
  size_t arg_size = args.size();
  if(arg_size >= 3)
  {
    if (!parse_bool_and_use(args[2], [&](bool r) { req.ignore_battery = r; }))
      return true;
  }
  if(arg_size >= 2)
  {
    if (!parse_bool_and_use(args[1], [&](bool r) { req.do_background_mining = r; }))
      return true;
  }
  if(arg_size >= 1)
  {
    uint16_t num = 1;
    ok = string_tools::get_xtype_from_string(num, args[0]);
    ok = ok && 1 <= num;
    req.threads_count = num;
  }
  else
  {
    req.threads_count = 1;
  }

  if (!ok)
  {
    PRINT_USAGE(USAGE_START_MINING);
    return true;
  }

  COMMAND_RPC_START_MINING::response res;
  bool r = m_wallet->invoke_http_json("/start_mining", req, res);
  std::string err = interpret_rpc_response(r, res.status);
  if (err.empty())
    success_msg_writer() << tr("Mining started in daemon");
  else
    fail_msg_writer() << tr("mining has NOT been started: ") << err;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::stop_mining(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  if (!m_wallet)
  {
    fail_msg_writer() << tr("wallet is null");
    return true;
  }

  COMMAND_RPC_STOP_MINING::request req;
  COMMAND_RPC_STOP_MINING::response res;
  bool r = m_wallet->invoke_http_json("/stop_mining", req, res);
  std::string err = interpret_rpc_response(r, res.status);
  if (err.empty())
    success_msg_writer() << tr("Mining stopped in daemon");
  else
    fail_msg_writer() << tr("mining has NOT been stopped: ") << err;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_daemon(const std::vector<std::string>& args)
{
  std::string daemon_url;

  if (args.size() < 1)
  {
    PRINT_USAGE(USAGE_SET_DAEMON);
    return true;
  }

  boost::regex rgx("^(.*://)?([A-Za-z0-9\\-\\.]+)(:[0-9]+)?");
  boost::cmatch match;
  // If user input matches URL regex
  if (boost::regex_match(args[0].c_str(), match, rgx))
  {
    if (match.length() < 4)
    {
      fail_msg_writer() << tr("Unexpected array length - Exited simple_wallet::set_daemon()");
      return true;
    }
    // If no port has been provided, use the default from config
    if (!match[3].length())
    {
      int daemon_port = get_config(m_wallet->nettype()).RPC_DEFAULT_PORT;
      daemon_url = match[1] + match[2] + std::string(":") + std::to_string(daemon_port);
    } else {
      daemon_url = args[0];
    }
    LOCK_IDLE_SCOPE();
    m_wallet->init(daemon_url);

    if (args.size() == 2)
    {
      if (args[1] == "trusted")
        m_wallet->set_trusted_daemon(true);
      else if (args[1] == "untrusted")
        m_wallet->set_trusted_daemon(false);
      else
      {
        fail_msg_writer() << tr("Expected trusted or untrusted, got ") << args[1] << ": assuming untrusted";
        m_wallet->set_trusted_daemon(false);
      }
    }
    else
    {
      m_wallet->set_trusted_daemon(false);
      try
      {
        if (tools::is_local_address(m_wallet->get_daemon_address()))
        {
          MINFO(tr("Daemon is local, assuming trusted"));
          m_wallet->set_trusted_daemon(true);
        }
      }
      catch (const std::exception &e) { }
    }
    success_msg_writer() << boost::format("Daemon set to %s, %s") % daemon_url % (m_wallet->is_trusted_daemon() ? tr("trusted") : tr("untrusted"));
  } else {
    fail_msg_writer() << tr("This does not seem to be a valid daemon URL.");
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::save_bc(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  if (!m_wallet)
  {
    fail_msg_writer() << tr("wallet is null");
    return true;
  }
  COMMAND_RPC_SAVE_BC::request req;
  COMMAND_RPC_SAVE_BC::response res;
  bool r = m_wallet->invoke_http_json("/save_bc", req, res);
  std::string err = interpret_rpc_response(r, res.status);
  if (err.empty())
    success_msg_writer() << tr("Blockchain saved");
  else
    fail_msg_writer() << tr("blockchain can't be saved: ") << err;
  return true;
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_new_block(uint64_t height, const cryptonote::block& block)
{
  if (!m_auto_refresh_refreshing)
    m_refresh_progress_reporter.update(height, false);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_money_received(uint64_t height, const crypto::hash &txid, const cryptonote::transaction& tx, uint64_t amount, const cryptonote::subaddress_index& subaddr_index)
{
  message_writer(console_color_green, false) << "\r" <<
    tr("Height ") << height << ", " <<
    tr("txid ") << txid << ", " <<
    print_money(amount) << ", " <<
    tr("idx ") << subaddr_index;

  const uint64_t warn_height = m_wallet->nettype() == TESTNET ? 1000000 : m_wallet->nettype() == STAGENET ? 50000 : 1650000;
  if (height >= warn_height)
  {
    std::vector<tx_extra_field> tx_extra_fields;
    parse_tx_extra(tx.extra, tx_extra_fields); // failure ok
    tx_extra_nonce extra_nonce;
    if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
    {
      crypto::hash payment_id = crypto::null_hash;
      if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        message_writer(console_color_red, false) <<
          (m_long_payment_id_support ? tr("WARNING: this transaction uses an unencrypted payment ID: consider using subaddresses instead.") : tr("WARNING: this transaction uses an unencrypted payment ID: these are obsolete. Support will be withdrawn in the future. Use subaddresses instead."));
   }
  }
  if (m_auto_refresh_refreshing)
    m_cmd_binder.print_prompt();
  else
    m_refresh_progress_reporter.update(height, true);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_unconfirmed_money_received(uint64_t height, const crypto::hash &txid, const cryptonote::transaction& tx, uint64_t amount, const cryptonote::subaddress_index& subaddr_index)
{
  // Not implemented in CLI wallet
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_money_spent(uint64_t height, const crypto::hash &txid, const cryptonote::transaction& in_tx, uint64_t amount, const cryptonote::transaction& spend_tx, const cryptonote::subaddress_index& subaddr_index)
{
  message_writer(console_color_magenta, false) << "\r" <<
    tr("Height ") << height << ", " <<
    tr("txid ") << txid << ", " <<
    tr("spent ") << print_money(amount) << ", " <<
    tr("idx ") << subaddr_index;
  if (m_auto_refresh_refreshing)
    m_cmd_binder.print_prompt();
  else
    m_refresh_progress_reporter.update(height, true);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_skip_transaction(uint64_t height, const crypto::hash &txid, const cryptonote::transaction& tx)
{
}
//----------------------------------------------------------------------------------------------------
boost::optional<epee::wipeable_string> simple_wallet::on_get_password(const char *reason)
{
  // can't ask for password from a background thread
  if (!m_in_manual_refresh.load(std::memory_order_relaxed))
  {
    message_writer(console_color_red, false) << boost::format(tr("Password needed (%s) - use the refresh command")) % reason;
    m_cmd_binder.print_prompt();
    return boost::none;
  }

#ifdef HAVE_READLINE
  rdln::suspend_readline pause_readline;
#endif
  std::string msg = tr("Enter password");
  if (reason && *reason)
    msg += std::string(" (") + reason + ")";
  auto pwd_container = tools::password_container::prompt(false, msg.c_str());
  if (!pwd_container)
  {
    MERROR("Failed to read password");
    return boost::none;
  }

  return pwd_container->password();
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_button_request()
{
  message_writer(console_color_white, false) << tr("Device requires attention");
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_pin_request(epee::wipeable_string & pin)
{
#ifdef HAVE_READLINE
  rdln::suspend_readline pause_readline;
#endif
  std::string msg = tr("Enter device PIN");
  auto pwd_container = tools::password_container::prompt(false, msg.c_str());
  THROW_WALLET_EXCEPTION_IF(!pwd_container, tools::error::password_entry_failed, tr("Failed to read device PIN"));
  pin = pwd_container->password();
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_passphrase_request(bool on_device, epee::wipeable_string & passphrase)
{
  if (on_device){
    message_writer(console_color_white, true) << tr("Please enter the device passphrase on the device");
    return;
  }

#ifdef HAVE_READLINE
  rdln::suspend_readline pause_readline;
#endif
  std::string msg = tr("Enter device passphrase");
  auto pwd_container = tools::password_container::prompt(false, msg.c_str());
  THROW_WALLET_EXCEPTION_IF(!pwd_container, tools::error::password_entry_failed, tr("Failed to read device passphrase"));
  passphrase = pwd_container->password();
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_refresh_finished(uint64_t start_height, uint64_t fetched_blocks, bool is_init, bool received_money)
{
  // Key image sync after the first refresh
  if (!m_wallet->get_account().get_device().has_tx_cold_sign()) {
    return;
  }

  if (!received_money || m_wallet->get_device_last_key_image_sync() != 0) {
    return;
  }

  // Finished first refresh for HW device and money received -> KI sync
  message_writer() << "\n" << tr("The first refresh has finished for the HW-based wallet with received money. hw_key_images_sync is needed. ");

  std::string accepted = input_line(tr("Do you want to do it now? (Y/Yes/N/No): "));
  if (std::cin.eof() || !command_line::is_yes(accepted)) {
    message_writer(console_color_red, false) << tr("hw_key_images_sync skipped. Run command manually before a transfer.");
    return;
  }

  key_images_sync_intern();
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::refresh_main(uint64_t start_height, enum ResetType reset, bool is_init)
{
  if (!try_connect_to_daemon(is_init))
    return true;

  LOCK_IDLE_SCOPE();

  if (reset != ResetNone)
    m_wallet->rescan_blockchain(reset == ResetHard, false);

#ifdef HAVE_READLINE
  rdln::suspend_readline pause_readline;
#endif

  message_writer() << tr("Starting refresh...");

  uint64_t fetched_blocks = 0;
  bool received_money = false;
  bool ok = false;
  std::ostringstream ss;
  try
  {
    m_in_manual_refresh.store(true, std::memory_order_relaxed);
    epee::misc_utils::auto_scope_leave_caller scope_exit_handler = epee::misc_utils::create_scope_leave_handler([&](){m_in_manual_refresh.store(false, std::memory_order_relaxed);});
    m_wallet->refresh(m_wallet->is_trusted_daemon(), start_height, fetched_blocks, received_money);
    m_has_locked_key_images = print_locked_stakes_main({}, false /*print_result*/);

    ok = true;
    // Clear line "Height xxx of xxx"
    std::cout << "\r                                                                \r";
    success_msg_writer(true) << tr("Refresh done, blocks received: ") << fetched_blocks;
    if (is_init)
      print_accounts();
    show_balance_unlocked();
    on_refresh_finished(start_height, fetched_blocks, is_init, received_money);
  }
  catch (const tools::error::daemon_busy&)
  {
    ss << tr("daemon is busy. Please try again later.");
  }
  catch (const tools::error::no_connection_to_daemon&)
  {
    ss << tr("no connection to daemon. Please make sure daemon is running.");
  }
  catch (const tools::error::wallet_rpc_error& e)
  {
    LOG_ERROR("RPC error: " << e.to_string());
    ss << tr("RPC error: ") << e.what();
  }
  catch (const tools::error::refresh_error& e)
  {
    LOG_ERROR("refresh error: " << e.to_string());
    ss << tr("refresh error: ") << e.what();
  }
  catch (const tools::error::wallet_internal_error& e)
  {
    LOG_ERROR("internal error: " << e.to_string());
    ss << tr("internal error: ") << e.what();
  }
  catch (const std::exception& e)
  {
    LOG_ERROR("unexpected error: " << e.what());
    ss << tr("unexpected error: ") << e.what();
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    ss << tr("unknown error");
  }

  if (!ok)
  {
    fail_msg_writer() << tr("refresh failed: ") << ss.str() << ". " << tr("Blocks received: ") << fetched_blocks;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::refresh(const std::vector<std::string>& args)
{
  uint64_t start_height = 0;
  if(!args.empty()){
    try
    {
        start_height = boost::lexical_cast<uint64_t>( args[0] );
    }
    catch(const boost::bad_lexical_cast &)
    {
        start_height = 0;
    }
  }
  return refresh_main(start_height, ResetNone);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_balance_unlocked(bool detailed)
{
  std::string extra;
  if (m_wallet->has_multisig_partial_key_images())
    extra = tr(" (Some owned outputs have partial key images - import_multisig_info needed)");
  else if (m_wallet->has_unknown_key_images())
    extra += tr(" (Some owned outputs have missing key images - import_key_images needed)");
  success_msg_writer() << tr("Currently selected account: [") << m_current_subaddress_account << tr("] ") << m_wallet->get_subaddress_label({m_current_subaddress_account, 0});
  const std::string tag = m_wallet->get_account_tags().second[m_current_subaddress_account];
  success_msg_writer() << tr("Tag: ") << (tag.empty() ? std::string{tr("(No tag assigned)")} : tag);
  success_msg_writer() << tr("Balance: ") << print_money(m_wallet->balance(m_current_subaddress_account)) << ", "
    << tr("unlocked balance: ") << print_money(m_wallet->unlocked_balance(m_current_subaddress_account)) << extra;
  std::map<uint32_t, uint64_t> balance_per_subaddress = m_wallet->balance_per_subaddress(m_current_subaddress_account);
  std::map<uint32_t, uint64_t> unlocked_balance_per_subaddress = m_wallet->unlocked_balance_per_subaddress(m_current_subaddress_account);
  if (!detailed || balance_per_subaddress.empty())
    return true;
  success_msg_writer() << tr("Balance per address:");
  success_msg_writer() << boost::format("%15s %21s %21s %7s %21s") % tr("Address") % tr("Balance") % tr("Unlocked balance") % tr("Outputs") % tr("Label");
  std::vector<tools::wallet2::transfer_details> transfers;
  m_wallet->get_transfers(transfers);
  for (const auto& i : balance_per_subaddress)
  {
    cryptonote::subaddress_index subaddr_index = {m_current_subaddress_account, i.first};
    std::string address_str = m_wallet->get_subaddress_as_str(subaddr_index).substr(0, 6);
    uint64_t num_unspent_outputs = std::count_if(transfers.begin(), transfers.end(), [&subaddr_index](const tools::wallet2::transfer_details& td) { return !td.m_spent && td.m_subaddr_index == subaddr_index; });
    success_msg_writer() << boost::format(tr("%8u %6s %21s %21s %7u %21s")) % i.first % address_str % print_money(i.second) % print_money(unlocked_balance_per_subaddress[i.first]) % num_unspent_outputs % m_wallet->get_subaddress_label(subaddr_index);
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_balance(const std::vector<std::string>& args/* = std::vector<std::string>()*/)
{
  if (args.size() > 1 || (args.size() == 1 && args[0] != "detail"))
  {
    PRINT_USAGE(USAGE_SHOW_BALANCE);
    return true;
  }
  LOCK_IDLE_SCOPE();
  show_balance_unlocked(args.size() == 1);
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_incoming_transfers(const std::vector<std::string>& args)
{
  if (args.size() > 3)
  {
    PRINT_USAGE(USAGE_INCOMING_TRANSFERS);
    return true;
  }
  auto local_args = args;
  LOCK_IDLE_SCOPE();

  bool filter = false;
  bool available = false;
  bool verbose = false;
  bool uses = false;
  if (local_args.size() > 0)
  {
    if (local_args[0] == "available")
    {
      filter = true;
      available = true;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "unavailable")
    {
      filter = true;
      available = false;
      local_args.erase(local_args.begin());
    }
  }
  while (local_args.size() > 0)
  {
    if (local_args[0] == "verbose")
      verbose = true;
    else if (local_args[0] == "uses")
      uses = true;
    else
    {
      fail_msg_writer() << tr("Invalid keyword: ") << local_args.front();
      break;
    }
    local_args.erase(local_args.begin());
  }

  const uint64_t blockchain_height = m_wallet->get_blockchain_current_height();

  PAUSE_READLINE();

  std::set<uint32_t> subaddr_indices;
  if (local_args.size() > 0 && local_args[0].substr(0, 6) == "index=")
  {
    std::string parse_subaddr_err;
    if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err))
    {
      fail_msg_writer() << parse_subaddr_err;
      return true;
    }
    local_args.erase(local_args.begin());
  }

  if (local_args.size() > 0)
  {
    PRINT_USAGE(USAGE_INCOMING_TRANSFERS);
    return true;
  }

  tools::wallet2::transfer_container transfers;
  m_wallet->get_transfers(transfers);

  size_t transfers_found = 0;
  for (const auto& td : transfers)
  {
    if (!filter || available != td.m_spent)
    {
      if (m_current_subaddress_account != td.m_subaddr_index.major || (!subaddr_indices.empty() && subaddr_indices.count(td.m_subaddr_index.minor) == 0))
        continue;
      if (!transfers_found)
      {
        std::string verbose_string;
        if (verbose)
          verbose_string = (boost::format("%68s%68s") % tr("pubkey") % tr("key image")).str();
        message_writer() << boost::format("%21s%8s%12s%8s%16s%68s%16s%s") % tr("amount") % tr("spent") % tr("unlocked") % tr("ringct") % tr("global index") % tr("tx id") % tr("addr index") % verbose_string;
      }
      std::string extra_string;
      if (verbose)
        extra_string += (boost::format("%68s%68s") % td.get_public_key() % (td.m_key_image_known ? epee::string_tools::pod_to_hex(td.m_key_image) : td.m_key_image_partial ? (epee::string_tools::pod_to_hex(td.m_key_image) + "/p") : std::string(64, '?'))).str();
      if (uses)
      {
        std::vector<uint64_t> heights;
        for (const auto &e: td.m_uses) heights.push_back(e.first);
        const std::pair<std::string, std::string> line = show_outputs_line(heights, blockchain_height, td.m_spent_height);
        extra_string += tr("Heights: ") + line.first + "\n" + line.second;
      }
      message_writer(td.m_spent ? console_color_magenta : console_color_green, false) <<
        boost::format("%21s%8s%12s%8s%16u%68s%16u%s") %
        print_money(td.amount()) %
        (td.m_spent ? tr("T") : tr("F")) %
        (m_wallet->is_transfer_unlocked(td) ? tr("unlocked") : tr("locked")) %
        (td.is_rct() ? tr("RingCT") : tr("-")) %
        td.m_global_output_index %
        td.m_txid %
        td.m_subaddr_index.minor %
        extra_string;
      ++transfers_found;
    }
  }

  if (!transfers_found)
  {
    if (!filter)
    {
      success_msg_writer() << tr("No incoming transfers");
    }
    else if (available)
    {
      success_msg_writer() << tr("No incoming available transfers");
    }
    else
    {
      success_msg_writer() << tr("No incoming unavailable transfers");
    }
  }
  else
  {
    success_msg_writer() << boost::format("Found %u/%u transfers") % transfers_found % transfers.size();
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_payments(const std::vector<std::string> &args)
{
  if(args.empty())
  {
    PRINT_USAGE(USAGE_PAYMENTS);
    return true;
  }

  LOCK_IDLE_SCOPE();

  PAUSE_READLINE();

  message_writer() << boost::format("%68s%68s%12s%21s%16s%16s") %
    tr("payment") % tr("transaction") % tr("height") % tr("amount") % tr("unlock time") % tr("addr index");

  bool payments_found = false;
  for(std::string arg : args)
  {
    crypto::hash payment_id;
    if(tools::wallet2::parse_payment_id(arg, payment_id))
    {
      std::list<tools::wallet2::payment_details> payments;
      m_wallet->get_payments(payment_id, payments);
      if(payments.empty())
      {
        success_msg_writer() << tr("No payments with id ") << payment_id;
        continue;
      }

      for (const tools::wallet2::payment_details& pd : payments)
      {
        if(!payments_found)
        {
          payments_found = true;
        }
        success_msg_writer(true) <<
          boost::format("%68s%68s%12s%21s%16s%16s") %
          payment_id %
          pd.m_tx_hash %
          pd.m_block_height %
          print_money(pd.m_amount) %
          pd.m_unlock_time %
          pd.m_subaddr_index.minor;
      }
    }
    else
    {
      fail_msg_writer() << tr("payment ID has invalid format, expected 16 or 64 character hex string: ") << arg;
    }
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
uint64_t simple_wallet::get_daemon_blockchain_height(std::string& err)
{
  if (!m_wallet)
  {
    throw std::runtime_error("simple_wallet null wallet");
  }
  return m_wallet->get_daemon_blockchain_height(err);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_blockchain_height(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  std::string err;
  uint64_t bc_height = get_daemon_blockchain_height(err);
  if (err.empty())
    success_msg_writer() << bc_height;
  else
    fail_msg_writer() << tr("failed to get blockchain height: ") << err;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::rescan_spent(const std::vector<std::string> &args)
{
  if (!m_wallet->is_trusted_daemon())
  {
    fail_msg_writer() << tr("this command requires a trusted daemon. Enable with --trusted-daemon");
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  try
  {
    LOCK_IDLE_SCOPE();
    m_wallet->rescan_spent();
  }
  catch (const tools::error::daemon_busy&)
  {
    fail_msg_writer() << tr("daemon is busy. Please try again later.");
  }
  catch (const tools::error::no_connection_to_daemon&)
  {
    fail_msg_writer() << tr("no connection to daemon. Please make sure daemon is running.");
  }
  catch (const tools::error::is_key_image_spent_error&)
  {
    fail_msg_writer() << tr("failed to get spent status");
  }
  catch (const tools::error::wallet_rpc_error& e)
  {
    LOG_ERROR("RPC error: " << e.to_string());
    fail_msg_writer() << tr("RPC error: ") << e.what();
  }
  catch (const std::exception& e)
  {
    LOG_ERROR("unexpected error: " << e.what());
    fail_msg_writer() << tr("unexpected error: ") << e.what();
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
std::pair<std::string, std::string> simple_wallet::show_outputs_line(const std::vector<uint64_t> &heights, uint64_t blockchain_height, uint64_t highlight_height) const
{
  std::stringstream ostr;

  for (uint64_t h: heights)
    blockchain_height = std::max(blockchain_height, h);

  for (size_t j = 0; j < heights.size(); ++j)
    ostr << (heights[j] == highlight_height ? " *" : " ") << heights[j];

  // visualize the distribution, using the code by moneroexamples onion-monero-viewer
  const uint64_t resolution = 79;
  std::string ring_str(resolution, '_');
  for (size_t j = 0; j < heights.size(); ++j)
  {
    uint64_t pos = (heights[j] * resolution) / blockchain_height;
    ring_str[pos] = 'o';
  }
  if (highlight_height < blockchain_height)
  {
    uint64_t pos = (highlight_height * resolution) / blockchain_height;
    ring_str[pos] = '*';
  }

  return std::make_pair(ostr.str(), ring_str);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_ring_members(const std::vector<tools::wallet2::pending_tx>& ptx_vector, std::ostream& ostr)
{
  uint32_t version;
  if (!try_connect_to_daemon(false, &version))
    return false;
  // available for RPC version 1.4 or higher
  if (version < MAKE_CORE_RPC_VERSION(1, 4))
    return true;
  std::string err;
  uint64_t blockchain_height = get_daemon_blockchain_height(err);
  if (!err.empty())
  {
    fail_msg_writer() << tr("failed to get blockchain height: ") << err;
    return false;
  }
  // for each transaction
  for (size_t n = 0; n < ptx_vector.size(); ++n)
  {
    const cryptonote::transaction& tx = ptx_vector[n].tx;
    const tools::wallet2::tx_construction_data& construction_data = ptx_vector[n].construction_data;
    ostr << boost::format(tr("\nTransaction %llu/%llu: txid=%s")) % (n + 1) % ptx_vector.size() % cryptonote::get_transaction_hash(tx);
    // for each input
    std::vector<uint64_t>     spent_key_height(tx.vin.size());
    std::vector<crypto::hash> spent_key_txid  (tx.vin.size());
    for (size_t i = 0; i < tx.vin.size(); ++i)
    {
      if (tx.vin[i].type() != typeid(cryptonote::txin_to_key))
        continue;
      const cryptonote::txin_to_key& in_key = boost::get<cryptonote::txin_to_key>(tx.vin[i]);
      const tools::wallet2::transfer_details &td = m_wallet->get_transfer_details(construction_data.selected_transfers[i]);
      const cryptonote::tx_source_entry *sptr = NULL;
      for (const auto &src: construction_data.sources)
        if (src.outputs[src.real_output].second.dest == td.get_public_key())
          sptr = &src;
      if (!sptr)
      {
        fail_msg_writer() << tr("failed to find construction data for tx input");
        return false;
      }
      const cryptonote::tx_source_entry& source = *sptr;

      ostr << boost::format(tr("\nInput %llu/%llu: amount=%s")) % (i + 1) % tx.vin.size() % print_money(source.amount);
      // convert relative offsets of ring member keys into absolute offsets (indices) associated with the amount
      std::vector<uint64_t> absolute_offsets = cryptonote::relative_output_offsets_to_absolute(in_key.key_offsets);
      // get block heights from which those ring member keys originated
      COMMAND_RPC_GET_OUTPUTS_BIN::request req = AUTO_VAL_INIT(req);
      req.outputs.resize(absolute_offsets.size());
      for (size_t j = 0; j < absolute_offsets.size(); ++j)
      {
        req.outputs[j].amount = in_key.amount;
        req.outputs[j].index = absolute_offsets[j];
      }
      COMMAND_RPC_GET_OUTPUTS_BIN::response res = AUTO_VAL_INIT(res);
      bool r = m_wallet->invoke_http_bin("/get_outs.bin", req, res);
      err = interpret_rpc_response(r, res.status);
      if (!err.empty())
      {
        fail_msg_writer() << tr("failed to get output: ") << err;
        return false;
      }
      // make sure that returned block heights are less than blockchain height
      for (auto& res_out : res.outs)
      {
        if (res_out.height >= blockchain_height)
        {
          fail_msg_writer() << tr("output key's originating block height shouldn't be higher than the blockchain height");
          return false;
        }
      }
      ostr << tr("\nOriginating block heights: ");
      spent_key_height[i] = res.outs[source.real_output].height;
      spent_key_txid  [i] = res.outs[source.real_output].txid;
      std::vector<uint64_t> heights(absolute_offsets.size(), 0);
      uint64_t highlight_height = std::numeric_limits<uint64_t>::max();
      for (size_t j = 0; j < absolute_offsets.size(); ++j)
      {
        heights[j] = res.outs[j].height;
        if (j == source.real_output)
          highlight_height = heights[j];
      }
      std::pair<std::string, std::string> ring_str = show_outputs_line(heights, highlight_height);
      ostr << ring_str.first << tr("\n|") << ring_str.second << tr("|\n");
    }
    // warn if rings contain keys originating from the same tx or temporally very close block heights
    bool are_keys_from_same_tx      = false;
    bool are_keys_from_close_height = false;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
      for (size_t j = i + 1; j < tx.vin.size(); ++j)
      {
        if (spent_key_txid[i] == spent_key_txid[j])
          are_keys_from_same_tx = true;
        if (std::abs((int64_t)(spent_key_height[i] - spent_key_height[j])) < (int64_t)5)
          are_keys_from_close_height = true;
      }
    }
    if (are_keys_from_same_tx || are_keys_from_close_height)
    {
      ostr
        << tr("\nWarning: Some input keys being spent are from ")
        << (are_keys_from_same_tx ? tr("the same transaction") : tr("blocks that are temporally very close"))
        << tr(", which can break the anonymity of ring signature. Make sure this is intentional!");
    }
    ostr << ENDL;
  }
  return true;
}

//----------------------------------------------------------------------------------------------------
static bool locked_blocks_arg_valid(const std::string& arg, uint64_t& duration)
{
  try
  {
    duration = boost::lexical_cast<uint64_t>(arg);
  }
  catch (const std::exception &e)
  {
    return false;
  }

  if (duration > 1000000)
  {
    fail_msg_writer() << tr("Locked blocks too high, max 1000000 (˜4 yrs)");
    return false;
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::transfer_main(int transfer_type, const std::vector<std::string> &args_, bool called_by_mms)
{
//  "transfer [index=<N1>[,<N2>,...]] [<priority>] <address> <amount> [<payment_id>]"
  if (!try_connect_to_daemon())
    return false;

  std::vector<std::string> local_args = args_;

  std::set<uint32_t> subaddr_indices;
  if (local_args.size() > 0 && local_args[0].substr(0, 6) == "index=")
  {
    std::string parse_subaddr_err;
    if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err))
    {
      fail_msg_writer() << parse_subaddr_err;
      return true;
    }
    local_args.erase(local_args.begin());
  }

  uint32_t priority = 0;
  if (local_args.size() > 0 && tools::parse_priority(local_args[0], priority))
    local_args.erase(local_args.begin());

  priority = m_wallet->adjust_priority(priority);

  const size_t min_args = (transfer_type == TransferLocked) ? 2 : 1;
  if(local_args.size() < min_args)
  {
     fail_msg_writer() << tr("wrong number of arguments");
     return false;
  }

  std::vector<uint8_t> extra;
  bool payment_id_seen = false;
  if (!local_args.empty())
  {
    std::string payment_id_str = local_args.back();
    crypto::hash payment_id;
    bool r = true;
    if (tools::wallet2::parse_long_payment_id(payment_id_str, payment_id))
    {
      LONG_PAYMENT_ID_SUPPORT_CHECK();

      std::string extra_nonce;
      set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
      r = add_extra_nonce_to_tx_extra(extra, extra_nonce);
      local_args.pop_back();
      payment_id_seen = true;
      message_writer() << tr("Unencrypted payment IDs are bad for privacy: ask the recipient to use subaddresses instead");
    }
    if(!r)
    {
      fail_msg_writer() << tr("payment id failed to encode");
      return false;
    }
  }

  uint64_t locked_blocks = 0;
  if (transfer_type == TransferLocked)
  {
    if (!locked_blocks_arg_valid(local_args.back(), locked_blocks))
    {
      return true;
    }
    local_args.pop_back();
  }

  vector<cryptonote::address_parse_info> dsts_info;
  vector<cryptonote::tx_destination_entry> dsts;
  size_t num_subaddresses = 0;
  for (size_t i = 0; i < local_args.size(); )
  {
    dsts_info.emplace_back();
    cryptonote::address_parse_info & info = dsts_info.back();
    cryptonote::tx_destination_entry de;
    bool r = true;

    // check for a URI
    std::string address_uri, payment_id_uri, tx_description, recipient_name, error;
    std::vector<std::string> unknown_parameters;
    uint64_t amount = 0;
    bool has_uri = m_wallet->parse_uri(local_args[i], address_uri, payment_id_uri, amount, tx_description, recipient_name, unknown_parameters, error);
    if (has_uri)
    {
      r = cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), address_uri, oa_prompter);
      if (payment_id_uri.size() == 16)
      {
        if (!tools::wallet2::parse_short_payment_id(payment_id_uri, info.payment_id))
        {
          fail_msg_writer() << tr("failed to parse short payment ID from URI");
          return false;
        }
        info.has_payment_id = true;
      }
      de.amount = amount;
      de.original = local_args[i];
      ++i;
    }
    else if (i + 1 < local_args.size())
    {
      r = cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), local_args[i], oa_prompter);
      bool ok = cryptonote::parse_amount(de.amount, local_args[i + 1]);
      if(!ok || 0 == de.amount)
      {
        fail_msg_writer() << tr("amount is wrong: ") << local_args[i] << ' ' << local_args[i + 1] <<
          ", " << tr("expected number from 0 to ") << print_money(std::numeric_limits<uint64_t>::max());
        return false;
      }
      de.original = local_args[i];
      i += 2;
    }
    else
    {
      if (boost::starts_with(local_args[i], "antd:"))
        fail_msg_writer() << tr("Invalid last argument: ") << local_args.back() << ": " << error;
      else
        fail_msg_writer() << tr("Invalid last argument: ") << local_args.back();
      return false;
    }

    if (!r)
    {
      fail_msg_writer() << tr("failed to parse address");
      return false;
    }
    de.addr = info.address;
    de.is_subaddress = info.is_subaddress;
    de.is_integrated = info.has_payment_id;
    num_subaddresses += info.is_subaddress;

    if (info.has_payment_id || !payment_id_uri.empty())
    {
      if (payment_id_seen)
      {
        fail_msg_writer() << tr("a single transaction cannot use more than one payment id");
        return false;
      }

      crypto::hash payment_id;
      std::string extra_nonce;
      if (info.has_payment_id)
      {
        set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, info.payment_id);
      }
      else if (tools::wallet2::parse_payment_id(payment_id_uri, payment_id))
      {
        LONG_PAYMENT_ID_SUPPORT_CHECK();
        set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
        message_writer() << tr("Unencrypted payment IDs are bad for privacy: ask the recipient to use subaddresses instead");
      }
      else
      {
        fail_msg_writer() << tr("failed to parse payment id, though it was detected");
        return false;
      }
      bool r = add_extra_nonce_to_tx_extra(extra, extra_nonce);
      if(!r)
      {
        fail_msg_writer() << tr("failed to set up payment id, though it was decoded correctly");
        return false;
      }
      payment_id_seen = true;
    }

    dsts.push_back(de);
  }

  // prompt is there is no payment id and confirmation is required
  if (m_long_payment_id_support && !payment_id_seen && m_wallet->confirm_missing_payment_id() && dsts.size() > num_subaddresses)
  {
     std::string accepted = input_line(tr("No payment id is included with this transaction. Is this okay?"), true);
     if (std::cin.eof())
       return false;
     if (!command_line::is_yes(accepted))
     {
       fail_msg_writer() << tr("transaction cancelled.");

       return false;
     }
  }

  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD(return false;);

  try
  {
    // figure out what tx will be necessary
    std::vector<tools::wallet2::pending_tx> ptx_vector;
    uint64_t bc_height, unlock_block = 0;
    std::string err;
    switch (transfer_type)
    {
      case TransferLocked:
        bc_height = get_daemon_blockchain_height(err);
        if (!err.empty())
        {
          fail_msg_writer() << tr("failed to get blockchain height: ") << err;
          return false;
        }
        unlock_block = bc_height + locked_blocks;
        ptx_vector = m_wallet->create_transactions_2(dsts, CRYPTONOTE_DEFAULT_TX_MIXIN, unlock_block /* unlock_time */, priority, extra, m_current_subaddress_account, subaddr_indices);
      break;
      default:
        LOG_ERROR("Unknown transfer method, using default");
        /* FALLTHRU */
      case Transfer:
        ptx_vector = m_wallet->create_transactions_2(dsts, CRYPTONOTE_DEFAULT_TX_MIXIN, 0 /* unlock_time */, priority, extra, m_current_subaddress_account, subaddr_indices);
      break;
    }

    if (ptx_vector.empty())
    {
      fail_msg_writer() << tr("No outputs found, or daemon is not ready");
      return false;
    }

    // if we need to check for backlog, check the worst case tx
    if (m_wallet->confirm_backlog())
    {
      std::stringstream prompt;
      double worst_fee_per_byte = std::numeric_limits<double>::max();
      for (size_t n = 0; n < ptx_vector.size(); ++n)
      {
        const uint64_t blob_size = cryptonote::tx_to_blob(ptx_vector[n].tx).size();
        const double fee_per_byte = ptx_vector[n].fee / (double)blob_size;
        if (fee_per_byte < worst_fee_per_byte)
        {
          worst_fee_per_byte = fee_per_byte;
        }
      }
      try
      {
        std::vector<std::pair<uint64_t, uint64_t>> nblocks = m_wallet->estimate_backlog({std::make_pair(worst_fee_per_byte, worst_fee_per_byte)});
        if (nblocks.size() != 1)
        {
          prompt << "Internal error checking for backlog. " << tr("Is this okay anyway?");
        }
        else
        {
          if (nblocks[0].first > m_wallet->get_confirm_backlog_threshold())
            prompt << (boost::format(tr("There is currently a %u block backlog at that fee level. Is this okay?")) % nblocks[0].first).str();
        }
      }
      catch (const std::exception &e)
      {
        prompt << tr("Failed to check for backlog: ") << e.what() << ENDL << tr("Is this okay anyway?");
      }

      std::string prompt_str = prompt.str();
      if (!prompt_str.empty())
      {
        std::string accepted = input_line(prompt_str, true);
        if (std::cin.eof())
          return false;
        if (!command_line::is_yes(accepted))
        {
          fail_msg_writer() << tr("transaction cancelled.");

          return false; 
        }
      }
    }

    // if more than one tx necessary, prompt user to confirm
    if (m_wallet->always_confirm_transfers() || ptx_vector.size() > 1)
    {
        uint64_t total_sent = 0;
        uint64_t total_fee = 0;
        uint64_t dust_not_in_fee = 0;
        uint64_t dust_in_fee = 0;
        for (size_t n = 0; n < ptx_vector.size(); ++n)
        {
          total_fee += ptx_vector[n].fee;
          for (auto i: ptx_vector[n].selected_transfers)
            total_sent += m_wallet->get_transfer_details(i).amount();
          total_sent -= ptx_vector[n].change_dts.amount + ptx_vector[n].fee;

          if (ptx_vector[n].dust_added_to_fee)
            dust_in_fee += ptx_vector[n].dust;
          else
            dust_not_in_fee += ptx_vector[n].dust;
        }

        std::stringstream prompt;
        for (size_t n = 0; n < ptx_vector.size(); ++n)
        {
          prompt << tr("\nTransaction ") << (n + 1) << "/" << ptx_vector.size() << ":\n";
          subaddr_indices.clear();
          for (uint32_t i : ptx_vector[n].construction_data.subaddr_indices)
            subaddr_indices.insert(i);
          for (uint32_t i : subaddr_indices)
            prompt << boost::format(tr("Spending from address index %d\n")) % i;
          if (subaddr_indices.size() > 1)
            prompt << tr("WARNING: Outputs of multiple addresses are being used together, which might potentially compromise your privacy.\n");
        }
        prompt << boost::format(tr("Sending %s.  ")) % print_money(total_sent);
        if (ptx_vector.size() > 1)
        {
          prompt << boost::format(tr("Your transaction needs to be split into %llu transactions.  "
            "This will result in a transaction fee being applied to each transaction, for a total fee of %s")) %
            ((unsigned long long)ptx_vector.size()) % print_money(total_fee);
        }
        else
        {
          prompt << boost::format(tr("The transaction fee is %s")) %
            print_money(total_fee);
        }
        if (dust_in_fee != 0) prompt << boost::format(tr(", of which %s is dust from change")) % print_money(dust_in_fee);
        if (dust_not_in_fee != 0)  prompt << tr(".") << ENDL << boost::format(tr("A total of %s from dust change will be sent to dust address")) 
                                                   % print_money(dust_not_in_fee);
        if (transfer_type == TransferLocked)
        {
          float days = locked_blocks / 720.0f;
          prompt << boost::format(tr(".\nThis transaction will unlock on block %llu, in approximately %s days (assuming 2 minutes per block)")) % ((unsigned long long)unlock_block) % days;
        }
        if (m_wallet->print_ring_members())
        {
          if (!print_ring_members(ptx_vector, prompt))
            return false;
        }
        bool default_ring_size = true;
        for (const auto &ptx: ptx_vector)
        {
          for (const auto &vin: ptx.tx.vin)
          {
            if (vin.type() == typeid(txin_to_key))
            {
              const txin_to_key& in_to_key = boost::get<txin_to_key>(vin);
              if (in_to_key.key_offsets.size() != CRYPTONOTE_DEFAULT_TX_MIXIN + 1)
                default_ring_size = false;
            }
          }
        }
        if (m_wallet->confirm_non_default_ring_size() && !default_ring_size)
        {
          prompt << tr("WARNING: this is a non default ring size, which may harm your privacy. Default is recommended.");
        }
        prompt << ENDL << tr("Is this okay?");
        
        std::string accepted = input_line(prompt.str(), true);
        if (std::cin.eof())
          return false;
        if (!command_line::is_yes(accepted))
        {
          fail_msg_writer() << tr("transaction cancelled.");

          return false;
        }
    }

    // actually commit the transactions
    if (m_wallet->multisig() && called_by_mms)
    {
      std::string ciphertext = m_wallet->save_multisig_tx(ptx_vector);
      if (!ciphertext.empty())
      {
        get_message_store().process_wallet_created_data(get_multisig_wallet_state(), mms::message_type::partially_signed_tx, ciphertext);
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to MMS");
      }
    }
    else if (m_wallet->multisig())
    {
      bool r = m_wallet->save_multisig_tx(ptx_vector, "multisig_antd_tx");
      if (!r)
      {
        fail_msg_writer() << tr("Failed to write transaction(s) to file");
        return false;
      }
      else
      {
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "multisig_antd_tx";
      }
    }
    else if (m_wallet->get_account().get_device().has_tx_cold_sign())
    {
      try
      {
        tools::wallet2::signed_tx_set signed_tx;
        if (!cold_sign_tx(ptx_vector, signed_tx, dsts_info, [&](const tools::wallet2::signed_tx_set &tx){ return accept_loaded_tx(tx); })){
          fail_msg_writer() << tr("Failed to cold sign transaction with HW wallet");
          return false;
        }

        commit_or_save(signed_tx.ptx, m_do_not_relay);
      }
      catch (const std::exception& e)
      {
        handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
        return false;
      }
      catch (...)
      {
        LOG_ERROR("Unknown error");
        fail_msg_writer() << tr("unknown error");
        return false;
      }
    }
    else if (m_wallet->watch_only())
    {
      bool r = m_wallet->save_tx(ptx_vector, "unsigned_antd_tx");
      if (!r)
      {
        fail_msg_writer() << tr("Failed to write transaction(s) to file");
        return false;
      }
      else
      {
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "unsigned_antd_tx";
      }
    }
    else
    {
      commit_or_save(ptx_vector, m_do_not_relay);
    }
  }
  catch (const std::exception &e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
    return false;
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
    return false;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::transfer(const std::vector<std::string> &args_)
{
  transfer_main(Transfer, args_, false);
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::locked_transfer(const std::vector<std::string> &args_)
{
  transfer_main(TransferLocked, args_, false);
  return true;
}

struct article_metadata {
  bool success;
  std::string error;
  std::string serialized_blob;
  std::string title;
  std::string content;
  std::string publisher;
  crypto::hash content_hash;

  article_metadata() : success(false), error(""), title(""), content(""), publisher(""), content_hash(crypto::null_hash) {}
};
//-------------------------------------------------------

article_metadata set_article_to_tx_extra(const std::string& title, const std::string& content, const std::string& publisher) {
  article_metadata result;
  message_writer() << tr("Debug: set_article_to_tx_extra: Starting");

  if (title.size() > 128 || content.size() > 2048 || publisher.size() > 64) {
    result.error = "Metadata too large";
    return result;
  }
  if (title.empty() || content.empty()) {
    result.error = "Missing title or content";
    return result;
  }
  tx_extra_article_info article_info = {title, content, publisher.empty() ? "Unknown" : publisher};

  //message_writer() << tr("Debug: set_article_to_tx_extra: Manually serializing");
  std::vector<uint8_t> serialized;
  try {
    size_t total_size = 1 + article_info.title.size() + 2 + article_info.content.size() + 1 + article_info.publisher.size();
    if (total_size + 4 > TX_EXTRA_NONCE_MAX_COUNT - 2) { // Account for ARTC (4), tag (1), length (1)
      result.error = "Serialized data too large: " + std::to_string(total_size + 4) + " bytes, max " + std::to_string(TX_EXTRA_NONCE_MAX_COUNT - 2);
    //  message_writer() << tr("Debug: set_article_to_tx_extra: ") << result.error;
      return result;
    }

    serialized.reserve(total_size);
    serialized.push_back(static_cast<uint8_t>(article_info.title.size()));
    serialized.insert(serialized.end(), article_info.title.begin(), article_info.title.end());
    serialized.push_back(static_cast<uint8_t>(article_info.content.size() >> 8)); // High byte
    serialized.push_back(static_cast<uint8_t>(article_info.content.size() & 0xFF)); // Low byte
    serialized.insert(serialized.end(), article_info.content.begin(), article_info.content.end());
    serialized.push_back(static_cast<uint8_t>(article_info.publisher.size()));
    serialized.insert(serialized.end(), article_info.publisher.begin(), article_info.publisher.end());

    result.serialized_blob = std::string("ARTC") + std::string(serialized.begin(), serialized.end());
    //message_writer() << tr("Debug: set_article_to_tx_extra: Serialization completed, size: ") << result.serialized_blob.size();
    //message_writer() << tr("Debug: set_article_to_tx_extra: Serialized data (hex): ") << epee::string_tools::buff_to_hex_nodelim(result.serialized_blob);
  } catch (const std::exception& e) {
    result.error = "Serialization exception: " + std::string(e.what());
    //message_writer() << tr("Debug: set_article_to_tx_extra: ") << result.error;
    return result;
  }

  if (result.serialized_blob.empty()) {
    result.error = "Serialized data empty";
    //message_writer() << tr("Debug: set_article_to_tx_extra: ") << result.error;
    return result;
  }

 // message_writer() << tr("Debug: set_article_to_tx_extra: Computing content hash");
  crypto::cn_fast_hash(content.data(), content.size(), result.content_hash);

  result.success = true;
  //message_writer() << tr("Debug: set_article_to_tx_extra: Completed");
  return result;
}
static bool parse_article_metadata(const std::string& extra_nonce, article_metadata& result) {
  result = article_metadata();
  //message_writer() << tr("Debug: parse_article_metadata: Starting, extra_nonce size: ") << extra_nonce.size();
 // message_writer() << tr("Debug: parse_article_metadata: extra_nonce (hex): ") << epee::string_tools::buff_to_hex_nodelim(extra_nonce);

  if (extra_nonce.size() < 4 || extra_nonce.substr(0, 4) != "ARTC") {
    result.error = "Invalid article metadata: missing ARTC prefix";
    //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
    return false;
  }

  // Try binary format (length-prefixed)
  {
    size_t pos = 4;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(extra_nonce.data());
    size_t size = extra_nonce.size();

    // Parse title
    if (pos + 1 > size) {
      result.error = "Invalid binary metadata: truncated title length";
      //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
      return false;
    }
    uint8_t title_len = data[pos++];
    if (pos + title_len > size || title_len > 128) {
      result.error = "Invalid binary metadata: invalid title length (" + std::to_string(title_len) + ")";
     // message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
      return false;
    }
    result.title = std::string(data + pos, data + pos + title_len);
    pos += title_len;

    // Parse content
    if (pos + 2 > size) {
      result.error = "Invalid binary metadata: truncated content length";
      //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
      return false;
    }
    uint16_t content_len = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
    pos += 2;
    if (pos + content_len > size || content_len > 2048) {
      result.error = "Invalid binary metadata: invalid content length (" + std::to_string(content_len) + ")";
      //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
      // Don't return false yet; try string format
    } else {
      result.content = std::string(data + pos, data + pos + content_len);
      pos += content_len;

      // Parse publisher
      if (pos + 1 > size) {
        result.error = "Invalid binary metadata: truncated publisher length";
       // message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
        return false;
      }
      uint8_t publisher_len = data[pos++];
      if (pos + publisher_len > size || publisher_len > 64) {
        result.error = "Invalid binary metadata: invalid publisher length (" + std::to_string(publisher_len) + ")";
        //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
        return false;
      }
      result.publisher = std::string(data + pos, data + pos + publisher_len);
      pos += publisher_len;

      if (pos != size) {
        result.error = "Invalid binary metadata: extra data after parsing";
        //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
        return false;
      }

      message_writer() << tr("Debug: parse_article_metadata: Computing content hash");
      crypto::cn_fast_hash(result.content.data(), result.content.size(), result.content_hash);
      result.success = true;
      //message_writer() << tr("Debug: parse_article_metadata: Completed (binary format)");
      return true;
    }
  }

  // Try string format (TITLE:<title>;CONTENT_HASH:<hash>;PUBLISHER:<publisher>)
  {
    const std::string title_tag = "TITLE:";
    const std::string content_hash_tag = ";CONTENT_HASH:";
    const std::string publisher_tag = ";PUBLISHER:";

    size_t t_pos = extra_nonce.find(title_tag);
    size_t c_pos = extra_nonce.find(content_hash_tag);
    size_t p_pos = extra_nonce.find(publisher_tag);

    if (t_pos != 4 || c_pos == std::string::npos || p_pos == std::string::npos || c_pos < t_pos + title_tag.length()) {
      result.error = "Invalid metadata: malformed string format";
      //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
      return false;
    }

    result.title = extra_nonce.substr(t_pos + title_tag.length(), c_pos - (t_pos + title_tag.length()));
    std::string content_hash_hex = extra_nonce.substr(c_pos + content_hash_tag.length(), p_pos - (c_pos + content_hash_tag.length()));
    result.publisher = extra_nonce.substr(p_pos + publisher_tag.length());

    // Load content from file
    const std::string filename = content_hash_hex + ".txt";
    std::ifstream file(filename);
    if (!file) {
      result.content = "[Error: Content file not found: " + filename + "]";
      //message_writer() << tr("Debug: parse_article_metadata: Content file not found: ") << filename;
    } else {
      std::stringstream buffer;
      buffer << file.rdbuf();
      result.content = buffer.str();
      //message_writer() << tr("Debug: parse_article_metadata: Loaded content from ") << filename;
    }

    // Compute content hash
    crypto::cn_fast_hash(result.content.data(), result.content.size(), result.content_hash);
    if (epee::string_tools::pod_to_hex(result.content_hash) != content_hash_hex) {
      result.error = "Content hash mismatch";
      //message_writer() << tr("Debug: parse_article_metadata: ") << result.error;
      return false;
    }

    result.success = true;
    //message_writer() << tr("Debug: parse_article_metadata: Completed (string format)");
    return true;
  }
}
//----------------------------------------------------------------------------------------------------
#include <common/util.h> // For epee::to_hex
#include <string_tools.h> // For epee::string_tools

bool simple_wallet::show_article(const std::vector<std::string>& args)
{
  // Check if transaction ID is provided
  if (args.empty())
  {
    message_writer() << tr("Error: Transaction ID not provided");
    return false;
  }

  // Parse transaction ID
  crypto::hash txid;
  if (!epee::string_tools::hex_to_pod(args[0], txid))
  {
    message_writer() << tr("Error: Invalid transaction ID format");
    return false;
  }

  // Retrieve transaction using RPC
  cryptonote::COMMAND_RPC_GET_TRANSACTIONS::request req{};
  cryptonote::COMMAND_RPC_GET_TRANSACTIONS::response res{};
  req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txid));
  req.decode_as_json = false;
  req.prune = false;
  req.split = false;

  if (!m_wallet->invoke_http_json("/get_transactions", req, res) || res.txs.empty())
  {
    fail_msg_writer() << tr("Failed to retrieve transaction");
    return false;
  }

  // Convert transaction hex to binary blob
  std::string tx_blob;
  if (!epee::string_tools::parse_hexstr_to_binbuff(res.txs[0].as_hex, tx_blob))
  {
    message_writer() << tr("Error: Failed to parse transaction hex");
    return false;
  }

  // Parse transaction from blob
  cryptonote::transaction tx;
  if (!cryptonote::parse_and_validate_tx_from_blob(tx_blob, tx))
  {
    message_writer() << tr("Error: Failed to parse transaction blob");
    return false;
  }

  // Log raw extra field for debugging
  //message_writer() << tr("Debug: Raw tx.extra (hex): ") << epee::to_hex::string(epee::span<const uint8_t>(tx.extra.data(), tx.extra.size()));

  // Parse tx_extra fields
  std::vector<cryptonote::tx_extra_field> tx_extra_fields;
  if (!cryptonote::parse_tx_extra(tx.extra, tx_extra_fields))
  {
    message_writer() << tr("Error: Failed to parse transaction extra data");
    return false;
  }

  // Log parsed extra fields for debugging
  //message_writer() << tr("Debug: Number of tx_extra_fields: ") << tx_extra_fields.size();
  for (size_t i = 0; i < tx_extra_fields.size(); ++i)
  {
    //message_writer() << tr("Debug: Extra field ") << i << tr(" type: ") << typeid(tx_extra_fields[i]).name();
  }

  // Extract tx_extra_nonce
  cryptonote::tx_extra_nonce nonce;
  if (!cryptonote::find_tx_extra_field_by_type(tx_extra_fields, nonce))
  {
    message_writer() << tr("Error: No nonce field found in transaction");
    return false;
  }

  // Check for ARTC prefix and deserialize article data
  std::string extra_str(nonce.nonce.begin(), nonce.nonce.end());
  //message_writer() << tr("Debug: Raw nonce (hex): ") << epee::to_hex::string(epee::span<const uint8_t>(reinterpret_cast<const uint8_t*>(extra_str.data()), extra_str.size()));

  cryptonote::tx_extra_article_info article_info;
  // Handle double ARTC prefix
  size_t pos = 0;
  if (extra_str.size() >= 8 && extra_str.substr(0, 4) == "ARTC" && extra_str.substr(4, 4) == "ARTC")
  {
    //message_writer() << tr("Debug: Found double ARTC prefix, skipping 8 bytes");
    pos = 8; // Skip both ARTC prefixes
  }
  else if (extra_str.size() >= 4 && extra_str.substr(0, 4) == "ARTC")
  {
    //message_writer() << tr("Debug: Found single ARTC prefix, skipping 4 bytes");
    pos = 4; // Skip single ARTC prefix
  }
  else
  {
    message_writer() << tr("Error: No article data (ARTC prefix) found in nonce");
    return false;
  }

  try
  {
    uint8_t title_len = static_cast<uint8_t>(extra_str[pos++]);
    if (pos + title_len > extra_str.size())
    {
      message_writer() << tr("Error: Invalid title length");
      return false;
    }
    article_info.title = extra_str.substr(pos, title_len);
    pos += title_len;

    if (pos + 2 > extra_str.size())
    {
      message_writer() << tr("Error: Incomplete content data");
      return false;
    }
    uint16_t content_len = (static_cast<uint8_t>(extra_str[pos]) << 8) | static_cast<uint8_t>(extra_str[pos + 1]);
    pos += 2;
    if (pos + content_len > extra_str.size())
    {
      message_writer() << tr("Error: Invalid content length");
      return false;
    }
    article_info.content = extra_str.substr(pos, content_len);
    pos += content_len;

    if (pos >= extra_str.size())
    {
      message_writer() << tr("Error: Incomplete publisher data");
      return false;
    }
    uint8_t publisher_len = static_cast<uint8_t>(extra_str[pos++]);
    if (pos + publisher_len > extra_str.size())
    {
      message_writer() << tr("Error: Invalid publisher length");
      return false;
    }
    article_info.publisher = extra_str.substr(pos, publisher_len);

    //message_writer() << tr("Debug: Manual deserialization successful");
  }
  catch (const std::exception& e)
  {
    message_writer() << tr("Error: Manual deserialization failed: ") << e.what();
    return false;
  }

  // Verify content hash
  crypto::hash content_hash;
  crypto::cn_fast_hash(article_info.content.data(), article_info.content.size(), content_hash);

  // Display article information
  message_writer() << tr("Article Information:");
  message_writer() << tr("Title: ") << article_info.title;
  message_writer() << tr("Publisher: ") << article_info.publisher;
  message_writer() << tr("Content: ") << article_info.content;
  message_writer() << tr("Content Hash: ") << epee::string_tools::pod_to_hex(content_hash);

  return true;
}
//---------------------------------------------------------------------
static void add_data_to_tx_extras(std::vector<uint8_t>& tx_extra, const std::string& data, uint8_t tag) {
  //message_writer() << tr("Debug: add_data_to_tx_extra: Starting, tag: ") << static_cast<int>(tag) << ", size: " << data.size();
  try {
    size_t pos = tx_extra.size();
    tx_extra.resize(tx_extra.size() + 1 + 1 + data.size()); // 1 for tag, 1 for length
    tx_extra[pos] = tag;
    tx_extra[pos + 1] = static_cast<uint8_t>(data.size());
    std::memcpy(&tx_extra[pos + 2], data.data(), data.size());
    //message_writer() << tr("Debug: add_data_to_tx_extra: Completed, new tx_extra size: ") << tx_extra.size();
  } catch (const std::exception& e) {
    //message_writer() << tr("Debug: add_data_to_tx_extra: Exception: ") << e.what();
    throw;
  }
}

bool simple_wallet::article_main(int transfer_type, const std::vector<std::string>& args_, bool called_by_mms) {
  //message_writer() << tr("Debug: Entering article_main, transfer_type: ") << transfer_type;
  if (!try_connect_to_daemon()) {
    //message_writer() << tr("Debug: Failed to connect to daemon");
    fail_msg_writer() << tr("Failed to connect to daemon");
    return false;
  }
  //message_writer() << tr("Debug: Connected to daemon");
  message_writer() << tr("Wallet network type: ") << (m_wallet->nettype() == cryptonote::MAINNET ? "mainnet" : m_wallet->nettype() == cryptonote::TESTNET ? "testnet" : "stagenet");

  std::vector<std::string> local_args = args_;
  std::string title, content, publisher, address, amount_str;

  // Handle interactive mode
  if (local_args.empty()) {
    //message_writer() << tr("Debug: No arguments provided, entering interactive mode");
    title = input_line(tr("Enter article title (max 128 characters):"));
    if (std::cin.eof()) {
      fail_msg_writer() << tr("Input cancelled");
      return false;
    }
    content = input_line(tr("Enter article content (max 2048 characters):"));
    if (std::cin.eof()) {
      fail_msg_writer() << tr("Input cancelled");
      return false;
    }
    publisher = input_line(tr("Enter article publisher (max 64 characters, optional):"));
    if (std::cin.eof()) {
      fail_msg_writer() << tr("Input cancelled");
      return false;
    }
    address = input_line(tr("Enter destination address:"));
    if (std::cin.eof()) {
      fail_msg_writer() << tr("Input cancelled");
      return false;
    }
    amount_str = input_line(tr("Enter amount:"));
    if (std::cin.eof()) {
      fail_msg_writer() << tr("Input cancelled");
      return false;
    }
  } else {
    // Parse non-interactive arguments
    //message_writer() << tr("Debug: Arguments provided, parsing args");
    if (args_.size() < 5) {
      fail_msg_writer() << tr("Usage: add_article <title words> : <content words> : <publisher words> <address> <amount>");
      return false;
    }
    size_t sep1 = std::find(local_args.begin(), local_args.end(), ":") - local_args.begin();
    size_t sep2 = std::find(local_args.begin() + sep1 + 1, local_args.end(), ":") - local_args.begin();
    if (sep1 >= local_args.size() || sep2 >= local_args.size() || sep2 <= sep1 + 1) {
      fail_msg_writer() << tr("Missing ':' separators. Use format: <title...> : <content...> : <publisher...> <address> <amount>");
      return false;
    }
    for (size_t i = 0; i < sep1; ++i) {
      title += local_args[i] + " ";
    }
    for (size_t i = sep1 + 1; i < sep2; ++i) {
      content += local_args[i] + " ";
    }
    for (size_t i = sep2 + 1; i < local_args.size() - 2; ++i) {
      publisher += local_args[i] + " ";
    }
    if (!title.empty()) title.pop_back();
    if (!content.empty()) content.pop_back();
    if (!publisher.empty()) publisher.pop_back();
    address = local_args[local_args.size() - 2];
    amount_str = local_args[local_args.size() - 1];
    local_args.erase(local_args.begin(), local_args.end() - 2);
  }

  // Validate article metadata
  //message_writer() << tr("Debug: Validating article metadata");
  if (title.empty() || content.empty()) {
    fail_msg_writer() << tr("Missing article metadata fields: title or content.");
    return false;
  }
  if (publisher.empty()) {
    publisher = "Unknown";
  }
  if (title.size() > 128 || content.size() > 2048 || publisher.size() > 64) {
    fail_msg_writer() << tr("Article metadata too long (title ≤ 128, content ≤ 2048, publisher ≤ 64)");
    return false;
  }

// Encode article metadata
std::vector<uint8_t> extra;
//message_writer() << tr("Debug: Before calling set_article_to_tx_extra");
article_metadata article_meta = set_article_to_tx_extra(title, content, publisher);
if (!article_meta.success) {
  fail_msg_writer() << tr("Failed to process article: ") << article_meta.error;
  return false;
}

//message_writer() << tr("Debug: Adding article metadata to tx_extra");
std::string extra_nonce = article_meta.serialized_blob; // Remove extra "ARTC"
if (!add_extra_nonce_to_tx_extra(extra, extra_nonce)) {
  fail_msg_writer() << tr("Failed to add article metadata to tx_extra");
  return false;
}
  //message_writer() << tr("Debug: Article metadata encoded");
  message_writer() << tr("Warning: Article metadata will be stored unencrypted in the transaction's tx_extra field and visible on the blockchain.");

  // Handle subaddress indices
  std::set<uint32_t> subaddr_indices;
  if (!local_args.empty() && local_args[0].substr(0, 6) == "index=") {
    //message_writer() << tr("Debug: Parsing subaddress indices");
    std::string parse_subaddr_err;
    if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err)) {
      fail_msg_writer() << parse_subaddr_err;
      return false;
    }
    local_args.erase(local_args.begin());
    //message_writer() << tr("Debug: Subaddress indices parsed: ") << subaddr_indices.size();
  }

  // Handle priority
  uint32_t priority = 0;
  if (!local_args.empty() && tools::parse_priority(local_args[0], priority)) {
    //message_writer() << tr("Debug: Parsing priority");
    local_args.erase(local_args.begin());
  }
  priority = m_wallet->adjust_priority(priority);
  //message_writer() << tr("Debug: Adjusted priority: ") << priority;

  // Check remaining arguments
  //message_writer() << tr("Debug: Checking remaining arguments, local_args.size: ") << local_args.size() << ", transfer_type: " << transfer_type;
  const size_t min_args = (transfer_type == TransferLocked) ? 2 : (local_args.empty() ? 0 : 1);
  if (local_args.size() < min_args) {
    fail_msg_writer() << tr("wrong number of arguments");
    return false;
  }

  // Handle payment ID
  bool payment_id_seen = false;
  if (!local_args.empty()) {
    std::string payment_id_str = local_args.back();
    crypto::hash payment_id;
    //message_writer() << tr("Debug: Checking for payment ID");
    if (tools::wallet2::parse_long_payment_id(payment_id_str, payment_id)) {
      //message_writer() << tr("Debug: Found payment ID: ") << payment_id_str;
      std::string extra_nonce;
      set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
      if (!add_extra_nonce_to_tx_extra(extra, extra_nonce)) {
        fail_msg_writer() << tr("payment id failed to encode");
        return false;
      }
      local_args.pop_back();
      payment_id_seen = true;
      //message_writer() << tr("Debug: Added payment ID to tx_extra");
      message_writer() << tr("Unencrypted payment IDs are bad for privacy: ask the recipient to use subaddresses instead");
    }
  }

  // Handle locked blocks for TransferLocked
  uint64_t locked_blocks = 0;
  if (transfer_type == TransferLocked) {
    //message_writer() << tr("Debug: Handling locked transfer");
    if (!local_args.empty() && !locked_blocks_arg_valid(local_args.back(), locked_blocks)) {
      fail_msg_writer() << tr("Invalid locked blocks value");
      return false;
    }
    if (!local_args.empty()) {
      local_args.pop_back();
    }
  }

  // Parse destination
  message_writer() << tr("Debug: Parsing destination");
  std::vector<cryptonote::address_parse_info> dsts_info;
  std::vector<cryptonote::tx_destination_entry> dsts;
  size_t num_subaddresses = 0;
  dsts_info.emplace_back();
  cryptonote::address_parse_info &info = dsts_info.back();
  cryptonote::tx_destination_entry de;
  bool r = true;

  // Handle antd: prefix
  if (boost::starts_with(address, "antd:")) {
    message_writer() << tr("Debug: Stripping antd: prefix from address");
    address = address.substr(5);
  }

  // Check for a URI
  std::string address_uri, payment_id_uri, tx_description, recipient_name, error;
  std::vector<std::string> unknown_parameters;
  uint64_t amount = 0;
  bool has_uri = m_wallet->parse_uri(address, address_uri, payment_id_uri, amount, tx_description, recipient_name, unknown_parameters, error);
  if (has_uri) {
    message_writer() << tr("Debug: Parsing URI: ") << address;
    r = cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), address_uri, oa_prompter);
    if (payment_id_uri.size() == 16) {
      if (!tools::wallet2::parse_short_payment_id(payment_id_uri, info.payment_id)) {
        fail_msg_writer() << tr("failed to parse short payment ID from URI");
        return false;
      }
      info.has_payment_id = true;
    }
    de.amount = amount;
    de.original = address;
  } else {
    message_writer() << tr("Debug: Parsing address: ") << address;
    r = cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), address, oa_prompter);
    bool ok = cryptonote::parse_amount(de.amount, amount_str);
    if (!ok || 0 == de.amount) {
      fail_msg_writer() << tr("amount is wrong: ") << address << ' ' << amount_str <<
        ", " << tr("expected number from 0 to ") << print_money(std::numeric_limits<uint64_t>::max());
      return false;
    }
    de.amount *= 1ULL; // Convert to atomic units (12 decimal places)
    de.original = address;
  }

  if (!r) {
    fail_msg_writer() << tr("failed to parse address");
    return false;
  }
  de.addr = info.address;
  de.is_subaddress = info.is_subaddress;
  de.is_integrated = info.has_payment_id;
  num_subaddresses += info.is_subaddress;
  dsts.push_back(de);

  if (info.has_payment_id || !payment_id_uri.empty()) {
    if (payment_id_seen) {
      fail_msg_writer() << tr("a single transaction cannot use more than one payment id");
      return false;
    }
    crypto::hash payment_id;
    std::string extra_nonce;
    if (info.has_payment_id) {
      set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, info.payment_id);
    } else if (tools::wallet2::parse_payment_id(payment_id_uri, payment_id)) {
      LONG_PAYMENT_ID_SUPPORT_CHECK();
      set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
      message_writer() << tr("Unencrypted payment IDs are bad for privacy: ask the recipient to use subaddresses instead");
    } else {
      fail_msg_writer() << tr("failed to parse payment id, though it was detected");
      return false;
    }
    if (!add_extra_nonce_to_tx_extra(extra, extra_nonce)) {
      fail_msg_writer() << tr("failed to set up payment id, though it was decoded correctly");
      return false;
    }
    payment_id_seen = true;
  }

  if (m_long_payment_id_support && !payment_id_seen && m_wallet->confirm_missing_payment_id() && dsts.size() > num_subaddresses && local_args.empty()) {
    message_writer() << tr("Debug: Prompting for missing payment ID");
    std::string accepted = input_line(tr("No payment id is included with this transaction. Is this okay?"), true);
    if (std::cin.eof()) return false;
    if (!command_line::is_yes(accepted)) {
      fail_msg_writer() << tr("transaction cancelled.");
      return false;
    }
  }

  // Check wallet balance
  message_writer() << tr("Debug: Checking wallet balance for account: ") << m_current_subaddress_account;
  uint64_t balance = m_wallet->balance(m_current_subaddress_account);
  uint64_t unlocked_balance = m_wallet->unlocked_balance(m_current_subaddress_account);
  message_writer() << tr("Debug: Wallet balance: ") << print_money(balance) << ", Unlocked balance: " << print_money(unlocked_balance);
  if (unlocked_balance < de.amount) {
    fail_msg_writer() << tr("Insufficient unlocked funds: balance ") << print_money(balance) << ", unlocked " << print_money(unlocked_balance) << ", required " << print_money(de.amount);
    return false;
  }

  message_writer() << tr("Debug: Unlocking wallet");
  SCOPED_WALLET_UNLOCK_ON_BAD_PASSWORD({
    fail_msg_writer() << tr("Invalid wallet password");
    return false;
  });
  try
  {
    // figure out what tx will be necessary
    std::vector<tools::wallet2::pending_tx> ptx_vector;
    uint64_t bc_height, unlock_block = 0;
    std::string err;
    switch (transfer_type)
    {
      case TransferLocked:
        bc_height = get_daemon_blockchain_height(err);
        if (!err.empty())
        {
          fail_msg_writer() << tr("failed to get blockchain height: ") << err;
          return false;
        }
        unlock_block = bc_height + locked_blocks;
        ptx_vector = m_wallet->create_transactions_2(dsts, CRYPTONOTE_DEFAULT_TX_MIXIN, unlock_block /* unlock_time */, priority, 
        extra, m_current_subaddress_account, subaddr_indices);
      break;
      default:
        LOG_ERROR("Unknown transfer method, using default");
        /* FALLTHRU */
      case Transfer:
        ptx_vector = m_wallet->create_transactions_2(dsts, CRYPTONOTE_DEFAULT_TX_MIXIN, 0 /* unlock_time */, priority, extra,
        m_current_subaddress_account, subaddr_indices);
      break;
    }

    if (ptx_vector.empty())
    {
      fail_msg_writer() << tr("No outputs found, or daemon is not ready");
      return false;
    }

    // if we need to check for backlog, check the worst case tx
    if (m_wallet->confirm_backlog())
    {
      std::stringstream prompt;
      double worst_fee_per_byte = std::numeric_limits<double>::max();
      for (size_t n = 0; n < ptx_vector.size(); ++n)
      {
        const uint64_t blob_size = cryptonote::tx_to_blob(ptx_vector[n].tx).size();
        const double fee_per_byte = ptx_vector[n].fee / (double)blob_size;
        if (fee_per_byte < worst_fee_per_byte)
        {
          worst_fee_per_byte = fee_per_byte;
        }
      }
      try
      {
        std::vector<std::pair<uint64_t, uint64_t>> nblocks = m_wallet->estimate_backlog({std::make_pair(worst_fee_per_byte, worst_fee_per_byte)});
        if (nblocks.size() != 1)
        {
          prompt << "Internal error checking for backlog. " << tr("Is this okay anyway?");
        }
        else
        {
          if (nblocks[0].first > m_wallet->get_confirm_backlog_threshold())
            prompt << (boost::format(tr("There is currently a %u block backlog at that fee level. Is this okay?")) % nblocks[0].first).str();
        }
      }
      catch (const std::exception &e)
      {
        prompt << tr("Failed to check for backlog: ") << e.what() << ENDL << tr("Is this okay anyway?");
      }

      std::string prompt_str = prompt.str();
      if (!prompt_str.empty())
      {
        std::string accepted = input_line(prompt_str, true);
        if (std::cin.eof())
          return false;
        if (!command_line::is_yes(accepted))
        {
          fail_msg_writer() << tr("transaction cancelled.");

          return false; 
        }
      }
    }

    // if more than one tx necessary, prompt user to confirm
    if (m_wallet->always_confirm_transfers() || ptx_vector.size() > 1)
    {
        uint64_t total_sent = 0;
        uint64_t total_fee = 0;
        uint64_t dust_not_in_fee = 0;
        uint64_t dust_in_fee = 0;
        for (size_t n = 0; n < ptx_vector.size(); ++n)
        {
          total_fee += ptx_vector[n].fee;
          for (auto i: ptx_vector[n].selected_transfers)
            total_sent += m_wallet->get_transfer_details(i).amount();
          total_sent -= ptx_vector[n].change_dts.amount + ptx_vector[n].fee;

          if (ptx_vector[n].dust_added_to_fee)
            dust_in_fee += ptx_vector[n].dust;
          else
            dust_not_in_fee += ptx_vector[n].dust;
        }

        std::stringstream prompt;
        for (size_t n = 0; n < ptx_vector.size(); ++n)
        {
          prompt << tr("\nTransaction ") << (n + 1) << "/" << ptx_vector.size() << ":\n";
          subaddr_indices.clear();
          for (uint32_t i : ptx_vector[n].construction_data.subaddr_indices)
            subaddr_indices.insert(i);
          for (uint32_t i : subaddr_indices)
            prompt << boost::format(tr("Spending from address index %d\n")) % i;
          if (subaddr_indices.size() > 1)
            prompt << tr("WARNING: Outputs of multiple addresses are being used together, which might potentially compromise your privacy.\n");
        }
        prompt << boost::format(tr("Sending %s.  ")) % print_money(total_sent);
        if (ptx_vector.size() > 1)
        {
          prompt << boost::format(tr("Your transaction needs to be split into %llu transactions.  "
            "This will result in a transaction fee being applied to each transaction, for a total fee of %s")) %
            ((unsigned long long)ptx_vector.size()) % print_money(total_fee);
        }
        else
        {
          prompt << boost::format(tr("The transaction fee is %s")) %
            print_money(total_fee);
        }
        if (dust_in_fee != 0) prompt << boost::format(tr(", of which %s is dust from change")) % print_money(dust_in_fee);
        if (dust_not_in_fee != 0)  prompt << tr(".") << ENDL << boost::format(tr("A total of %s from dust change will be sent to dust address")) 
                                                   % print_money(dust_not_in_fee);
        if (transfer_type == TransferLocked)
        {
          float days = locked_blocks / 720.0f;
          prompt << boost::format(tr(".\nThis transaction will unlock on block %llu, in approximately %s days (assuming 2 minutes per block)")) % ((unsigned long long)unlock_block) % days;
        }
        if (m_wallet->print_ring_members())
        {
          if (!print_ring_members(ptx_vector, prompt))
            return false;
        }
        bool default_ring_size = true;
        for (const auto &ptx: ptx_vector)
        {
          for (const auto &vin: ptx.tx.vin)
          {
            if (vin.type() == typeid(txin_to_key))
            {
              const txin_to_key& in_to_key = boost::get<txin_to_key>(vin);
              if (in_to_key.key_offsets.size() != CRYPTONOTE_DEFAULT_TX_MIXIN + 1)
                default_ring_size = false;
            }
          }
        }
        if (m_wallet->confirm_non_default_ring_size() && !default_ring_size)
        {
          prompt << tr("WARNING: this is a non default ring size, which may harm your privacy. Default is recommended.");
        }
        prompt << ENDL << tr("Is this okay?");
        
        std::string accepted = input_line(prompt.str(), true);
        if (std::cin.eof())
          return false;
        if (!command_line::is_yes(accepted))
        {
          fail_msg_writer() << tr("transaction cancelled.");

          return false;
        }
    }

    // actually commit the transactions
    if (m_wallet->multisig() && called_by_mms)
    {
      std::string ciphertext = m_wallet->save_multisig_tx(ptx_vector);
      if (!ciphertext.empty())
      {
        get_message_store().process_wallet_created_data(get_multisig_wallet_state(), mms::message_type::partially_signed_tx, ciphertext);
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to MMS");
      }
    }
    else if (m_wallet->multisig())
    {
      bool r = m_wallet->save_multisig_tx(ptx_vector, "multisig_antd_tx");
      if (!r)
      {
        fail_msg_writer() << tr("Failed to write transaction(s) to file");
        return false;
      }
      else
      {
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "multisig_antd_tx";
      }
    }
    else if (m_wallet->get_account().get_device().has_tx_cold_sign())
    {
      try
      {
        tools::wallet2::signed_tx_set signed_tx;
        if (!cold_sign_tx(ptx_vector, signed_tx, dsts_info, [&](const tools::wallet2::signed_tx_set &tx){ return accept_loaded_tx(tx); })){
          fail_msg_writer() << tr("Failed to cold sign transaction with HW wallet");
          return false;
        }

        commit_or_save(signed_tx.ptx, m_do_not_relay);
      }
      catch (const std::exception& e)
      {
        handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
        return false;
      }
      catch (...)
      {
        LOG_ERROR("Unknown error");
        fail_msg_writer() << tr("unknown error");
        return false;
      }
    }
    else if (m_wallet->watch_only())
    {
      bool r = m_wallet->save_tx(ptx_vector, "unsigned_antd_tx");
      if (!r)
      {
        fail_msg_writer() << tr("Failed to write transaction(s) to file");
        return false;
      }
      else
      {
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "unsigned_antd_tx";
      }
    }
    else
    {
      commit_or_save(ptx_vector, m_do_not_relay);
    }
  }
  catch (const std::exception &e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
    return false;
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
    return false;
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::add_article(const std::vector<std::string> &args_)
{
  article_main(Transfer, args_, false);
  return true;
}
//-------------------------------------------
bool simple_wallet::locked_sweep_all(const std::vector<std::string> &args_)
{
  return sweep_main(0, true, args_);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::register_full_node(const std::vector<std::string> &args_)
{
  if (!try_connect_to_daemon())
    return true;

  SCOPED_WALLET_UNLOCK()
  tools::wallet2::register_full_node_result result = m_wallet->create_register_full_node_tx(args_, m_current_subaddress_account);
  if (result.status != tools::wallet2::register_full_node_result_status::success)
  {
    fail_msg_writer() << result.msg;
    if (result.status == tools::wallet2::register_full_node_result_status::insufficient_num_args ||
        result.status == tools::wallet2::register_full_node_result_status::full_node_key_parse_fail ||
        result.status == tools::wallet2::register_full_node_result_status::full_node_signature_parse_fail ||
        result.status == tools::wallet2::register_full_node_result_status::subaddr_indices_parse_fail ||
        result.status == tools::wallet2::register_full_node_result_status::convert_registration_args_failed)
    {
      fail_msg_writer() << USAGE_REGISTER_FULL_NODE;
    }
    return true;
  }

  address_parse_info info = {};
  info.address            = m_wallet->get_address();
  try
  {
    std::vector<tools::wallet2::pending_tx> ptx_vector = {result.ptx};
    if (!sweep_main_internal(sweep_type_t::register_stake, ptx_vector, info))
    {
      fail_msg_writer() << tr("Sending register transaction failed");
      return true;
    }
  }
  catch (const std::exception& e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::stake(const std::vector<std::string> &args_)
{
  if (!try_connect_to_daemon())
    return true;

  //
  // Parse Arguments from Args
  //
  crypto::public_key full_node_key = {};
  uint32_t priority = 0;
  std::set<uint32_t> subaddr_indices = {};
  uint64_t amount = 0;
  double amount_fraction = 0;
  {
    std::vector<std::string> local_args = args_;
    if (local_args.size() > 0 && local_args[0].substr(0, 6) == "index=")
    {
      std::string parse_subaddr_err;
      if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err))
      {
        fail_msg_writer() << parse_subaddr_err;
        return true;
      }
      local_args.erase(local_args.begin());
    }

    if (local_args.size() > 0 && tools::parse_priority(local_args[0], priority))
      local_args.erase(local_args.begin());
    priority = m_wallet->adjust_priority(priority);

    if (local_args.size() < 2)
    {
      fail_msg_writer() << tr(USAGE_STAKE);
      return true;
    }

    if (!epee::string_tools::hex_to_pod(local_args[0], full_node_key))
    {
      fail_msg_writer() << tr("failed to parse fullnode pubkey");
      return true;
    }

    if (local_args[1].back() == '%')
    {
      local_args[1].pop_back();
      amount = 0;
      try
      {
        amount_fraction = boost::lexical_cast<double>(local_args[2]) / 100.0;
      }
      catch (const std::exception &e)
      {
        fail_msg_writer() << tr("Invalid percentage");
        return true;
      }
      if (amount_fraction < 0 || amount_fraction > 1)
      {
        fail_msg_writer() << tr("Invalid percentage");
        return true;
      }
    }
    else
    {
      if (!cryptonote::parse_amount(amount, local_args[1]) || amount == 0)
      {
        fail_msg_writer() << tr("amount is wrong: ") << local_args[2] <<
          ", " << tr("expected number from ") << print_money(1) << " to " << print_money(std::numeric_limits<uint64_t>::max());
        return true;
      }
    }
  }

  //
  // Try Staking
  //
  SCOPED_WALLET_UNLOCK()
  {
    m_wallet->refresh(false);
    try
    {
      address_parse_info info = {};
      info.address            = m_wallet->get_address();

      time_t begin_construct_time = time(nullptr);

      tools::wallet2::stake_result stake_result = m_wallet->create_stake_tx(full_node_key, info, amount, amount_fraction, priority, m_current_subaddress_account, subaddr_indices);
      if (stake_result.status != tools::wallet2::stake_result_status::success)
      {
        fail_msg_writer() << stake_result.msg;
        return true;
      }

      if (!stake_result.msg.empty()) // i.e. warnings
        tools::msg_writer() << stake_result.msg;

      std::vector<tools::wallet2::pending_tx> ptx_vector = {stake_result.ptx};
      if (!sweep_main_internal(sweep_type_t::stake, ptx_vector, info))
      {
        fail_msg_writer() << tr("Sending stake transaction failed");
        return true;
      }

      time_t end_construct_time = time(nullptr);
      time_t construct_time     = end_construct_time - begin_construct_time;
      if (construct_time > (60 * 10))
      {
        fail_msg_writer() << tr("Staking command has timed out due to waiting longer than 10 mins. This prevents the staking transaction from becoming invalid due to blocks mined interim. Please try again");
        return true;
      }
    }
    catch (const std::exception& e)
    {
      handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
    }
    catch (...)
    {
      LOG_ERROR("unknown error");
      fail_msg_writer() << tr("unknown error");
    }
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::request_stake_unlock(const std::vector<std::string> &args_)
{
  if (!try_connect_to_daemon())
    return true;

  if (args_.size() != 1)
  {
    fail_msg_writer() << tr(USAGE_REQUEST_STAKE_UNLOCK);
    return true;
  }

  crypto::public_key snode_key;
  if (!epee::string_tools::hex_to_pod(args_[0], snode_key))
  {
    fail_msg_writer() << tr("failed to parse fullnode pubkey: ") << args_[0];
    return true;
  }

  SCOPED_WALLET_UNLOCK();
  tools::wallet2::request_stake_unlock_result unlock_result = m_wallet->can_request_stake_unlock(snode_key);
  if (unlock_result.success)
  {
    tools::msg_writer() << unlock_result.msg;
  }
  else
  {
    fail_msg_writer() << unlock_result.msg;
    return true;
  }

  if (!command_line::is_yes(input_line("Is this okay?", true)))
    return true;

  // TODO(doyle): INF_STAKING(doyle): Do we support staking in these modes?
  if (m_wallet->multisig())
  {
    fail_msg_writer() << tr("Multi sig request stake unlock is unsupported");
    return true;
  }

  std::vector<tools::wallet2::pending_tx> ptx_vector = {unlock_result.ptx};
  if (m_wallet->watch_only())
  {
    if (m_wallet->save_tx(ptx_vector, "unsigned_antd_tx"))
      success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "unsigned_antd_tx";
    else
      fail_msg_writer() << tr("Failed to write transaction(s) to file");

    return true;
  }

  try
  {
    commit_or_save(ptx_vector, m_do_not_relay);
  }
  catch (const std::exception &e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_locked_stakes_main(const std::vector<std::string> &args_, bool print_result)
{
  if (!try_connect_to_daemon())
    return false;

  bool has_locked_stakes = false;
  std::string msg_buf;
  {
    using namespace cryptonote;
    boost::optional<std::string> failed;
    const std::vector<COMMAND_RPC_GET_FULL_NODES::response::entry> response = m_wallet->get_all_full_nodes(failed);
    if (failed)
    {
      fail_msg_writer() << *failed;
      return has_locked_stakes;
    }

    cryptonote::account_public_address const primary_address = m_wallet->get_address();
    for (COMMAND_RPC_GET_FULL_NODES::response::entry const &node_info : response)
    {
      bool only_once = true;
      for (COMMAND_RPC_GET_FULL_NODES::response::contributor const &contributor : node_info.contributors)
      {
        address_parse_info address_info = {};
        if (!cryptonote::get_account_address_from_str(address_info, m_wallet->nettype(), contributor.address))
        {
          fail_msg_writer() << tr("Failed to parse string representation of address: ") << contributor.address;
          continue;
        }

        if (primary_address != address_info.address)
          continue;

        for (size_t i = 0; i < contributor.locked_contributions.size(); ++i)
        {
          COMMAND_RPC_GET_FULL_NODES::response::contribution const &contribution = contributor.locked_contributions[i];
          has_locked_stakes = true;

          if (!print_result)
            continue;

          msg_buf.reserve(512);
          if (only_once)
          {
            only_once = false;
            msg_buf.append("Full Node: ");
            msg_buf.append(node_info.full_node_pubkey);
            msg_buf.append("\n");

            msg_buf.append("Unlock Height: ");
            if (node_info.requested_unlock_height == full_nodes::KEY_IMAGE_AWAITING_UNLOCK_HEIGHT)
                msg_buf.append("Unlock not requested yet");
            else
                msg_buf.append(std::to_string(node_info.requested_unlock_height));
            msg_buf.append("\n");

            msg_buf.append("Total Locked: ");
            msg_buf.append(cryptonote::print_money(contributor.amount));
            msg_buf.append("\n");

            msg_buf.append("Amount/Key Image: ");
          }

          msg_buf.append(cryptonote::print_money(contribution.amount));
          msg_buf.append("/");
          msg_buf.append(contribution.key_image);
          msg_buf.append("\n");

          if (i < (contributor.locked_contributions.size() - 1))
          {
            msg_buf.append("                  ");
          }
          else
          {
            msg_buf.append("\n");
          }
        }
      }
    }
  }

  {
    using namespace cryptonote;
    boost::optional<std::string> failed;
    const std::vector<cryptonote::COMMAND_RPC_GET_FULL_NODE_BLACKLISTED_KEY_IMAGES::entry> response = m_wallet->get_full_node_blacklisted_key_images(failed);
    if (failed)
    {
      fail_msg_writer() << *failed;
      return has_locked_stakes;
    }

    bool once_only = true;
    cryptonote::blobdata binary_buf;
    binary_buf.reserve(sizeof(crypto::key_image));
    for (size_t i = 0; i < response.size(); ++i)
    {
      COMMAND_RPC_GET_FULL_NODE_BLACKLISTED_KEY_IMAGES::entry const &entry = response[i];
      binary_buf.clear();
      if(!epee::string_tools::parse_hexstr_to_binbuff(entry.key_image, binary_buf) || binary_buf.size() != sizeof(crypto::key_image))
      {
        fail_msg_writer() << tr("Failed to parse hex representation of key image: ") << entry.key_image;
        continue;
      }

      if (!m_wallet->contains_key_image(*reinterpret_cast<const crypto::key_image*>(binary_buf.data())))
        continue;

      has_locked_stakes = true;
      if (!print_result)
        continue;

      msg_buf.reserve(512);
      if (once_only)
      {
        msg_buf.append("Blacklisted Stakes\n");
        once_only = false;
      }

      msg_buf.append("  Unlock Height/Key Image: ");
      msg_buf.append(std::to_string(entry.unlock_height));
      msg_buf.append("/");
      msg_buf.append(entry.key_image);
      msg_buf.append("\n");

      if (i < (response.size() - 1))
        msg_buf.append("\n");
    }
  }

  if (print_result)
  {
    if (has_locked_stakes)
    {
      tools::msg_writer() << msg_buf;
    }
    else
    {
      tools::msg_writer() << "No locked stakes known for this wallet on the network";
    }
  }

  return has_locked_stakes;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_locked_stakes(const std::vector<std::string> &args_)
{
  if (!try_connect_to_daemon())
    return false;
  SCOPED_WALLET_UNLOCK();

  print_locked_stakes_main(args_, true/*print_result*/);
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sweep_unmixable(const std::vector<std::string> &args_)
{
  if (!try_connect_to_daemon())
    return true;

  SCOPED_WALLET_UNLOCK();

  try
  {
    // figure out what tx will be necessary
    auto ptx_vector = m_wallet->create_unmixable_sweep_transactions();

    if (ptx_vector.empty())
    {
      fail_msg_writer() << tr("No unmixable outputs found");
      return true;
    }

    // give user total and fee, and prompt to confirm
    uint64_t total_fee = 0, total_unmixable = 0;
    for (size_t n = 0; n < ptx_vector.size(); ++n)
    {
      total_fee += ptx_vector[n].fee;
      for (auto i: ptx_vector[n].selected_transfers)
        total_unmixable += m_wallet->get_transfer_details(i).amount();
    }

    std::string prompt_str = tr("Sweeping ") + print_money(total_unmixable);
    if (ptx_vector.size() > 1) {
      prompt_str = (boost::format(tr("Sweeping %s in %llu transactions for a total fee of %s.  Is this okay?")) %
        print_money(total_unmixable) %
        ((unsigned long long)ptx_vector.size()) %
        print_money(total_fee)).str();
    }
    else {
      prompt_str = (boost::format(tr("Sweeping %s for a total fee of %s. Is this okay?")) %
        print_money(total_unmixable) %
        print_money(total_fee)).str();
    }
    std::string accepted = input_line(prompt_str, true);
    if (std::cin.eof())
      return true;
    if (!command_line::is_yes(accepted))
    {
      fail_msg_writer() << tr("transaction cancelled.");

      return true;
    }

    // actually commit the transactions
    if (m_wallet->multisig())
    {
      bool r = m_wallet->save_multisig_tx(ptx_vector, "multisig_antd_tx");
      if (!r)
      {
        fail_msg_writer() << tr("Failed to write transaction(s) to file");
      }
      else
      {
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "multisig_antd_tx";
      }
    }
    else if (m_wallet->watch_only())
    {
      bool r = m_wallet->save_tx(ptx_vector, "unsigned_antd_tx");
      if (!r)
      {
        fail_msg_writer() << tr("Failed to write transaction(s) to file");
      }
      else
      {
        success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "unsigned_antd_tx";
      }
    }
    else
    {
      commit_or_save(ptx_vector, m_do_not_relay);
    }
  }
  catch (const std::exception &e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sweep_main_internal(sweep_type_t sweep_type, std::vector<tools::wallet2::pending_tx> &ptx_vector, cryptonote::address_parse_info const &dest)
{
  if ((sweep_type == sweep_type_t::stake || sweep_type == sweep_type_t::register_stake) && ptx_vector.size() > 1)
  {
    fail_msg_writer() << tr("Too many outputs. Please sweep_all first");
    return true;
  }

  if (sweep_type == sweep_type_t::single)
  {
    if (ptx_vector.size() > 1)
    {
      fail_msg_writer() << tr("Multiple transactions are created, which is not supposed to happen");
      return true;
    }

    if (ptx_vector[0].selected_transfers.size() != 1)
    {
      fail_msg_writer() << tr("The transaction uses multiple or no inputs, which is not supposed to happen");
      return true;
    }
  }

  if (ptx_vector.empty())
  {
    fail_msg_writer() << tr("No outputs found, or daemon is not ready");
    return false;
  }

  // give user total and fee, and prompt to confirm
  uint64_t total_fee = 0, total_sent = 0;
  for (size_t n = 0; n < ptx_vector.size(); ++n)
  {
    total_fee += ptx_vector[n].fee;
    for (auto i: ptx_vector[n].selected_transfers)
      total_sent += m_wallet->get_transfer_details(i).amount();

    if (sweep_type == sweep_type_t::stake || sweep_type == sweep_type_t::register_stake)
    {
      ptx_vector[n].tx.version = std::max((size_t)transaction::version_3_per_output_unlock_times, ptx_vector[n].tx.version);
      total_sent -= ptx_vector[n].change_dts.amount + ptx_vector[n].fee;
    }
  }

  std::ostringstream prompt;
  std::set<uint32_t> subaddr_indices;
  for (size_t n = 0; n < ptx_vector.size(); ++n)
  {
    prompt << tr("\nTransaction ") << (n + 1) << "/" << ptx_vector.size() << ":\n";
    subaddr_indices.clear();
    for (uint32_t i : ptx_vector[n].construction_data.subaddr_indices)
      subaddr_indices.insert(i);
    for (uint32_t i : subaddr_indices)
      prompt << boost::format(tr("Spending from address index %d\n")) % i;
    if (subaddr_indices.size() > 1)
      prompt << tr("WARNING: Outputs of multiple addresses are being used together, which might potentially compromise your privacy.\n");
  }
  if (m_wallet->print_ring_members() && !print_ring_members(ptx_vector, prompt))
  {
    fail_msg_writer() << tr("Error printing ring members");
    return false;
  }

  const char *label = (sweep_type == sweep_type_t::stake || sweep_type == sweep_type_t::register_stake) ? "Staking" : "Sweeping";
  if (ptx_vector.size() > 1) {
    prompt << boost::format(tr("%s %s in %llu transactions for a total fee of %s. Is this okay?")) %
      label %
      print_money(total_sent) %
      ((unsigned long long)ptx_vector.size()) %
      print_money(total_fee);
  }
  else {
    prompt << boost::format(tr("%s %s for a total fee of %s. Is this okay?")) %
      label %
      print_money(total_sent) %
      print_money(total_fee);
  }
  std::string accepted = input_line(prompt.str(), true);
  if (std::cin.eof())
    return false;
  if (!command_line::is_yes(accepted))
  {
    fail_msg_writer() << tr("transaction cancelled.");
    return false;
  }

  // actually commit the transactions
  bool submitted_to_network = false;
  if (m_wallet->multisig())
  {
    bool r = m_wallet->save_multisig_tx(ptx_vector, "multisig_antd_tx");
    if (!r)
    {
      fail_msg_writer() << tr("Failed to write transaction(s) to file");
    }
    else
    {
      success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "multisig_antd_tx";
    }
  }
  else if (m_wallet->get_account().get_device().has_tx_cold_sign())
  {
    try
    {
      tools::wallet2::signed_tx_set signed_tx;
      std::vector<cryptonote::address_parse_info> dsts_info;
      dsts_info.push_back(dest);

      if (!cold_sign_tx(ptx_vector, signed_tx, dsts_info, [&](const tools::wallet2::signed_tx_set &tx){ return accept_loaded_tx(tx); })){
        fail_msg_writer() << tr("Failed to cold sign transaction with HW wallet");
        return true;
      }

      commit_or_save(signed_tx.ptx, m_do_not_relay);
    }
    catch (const std::exception& e)
    {
      handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
    }
    catch (...)
    {
      LOG_ERROR("Unknown error");
      fail_msg_writer() << tr("unknown error");
    }
  }
  else if (m_wallet->watch_only())
  {
    bool r = m_wallet->save_tx(ptx_vector, "unsigned_antd_tx");
    if (!r)
    {
      fail_msg_writer() << tr("Failed to write transaction(s) to file");
    }
    else
    {
      success_msg_writer(true) << tr("Unsigned transaction(s) successfully written to file: ") << "unsigned_antd_tx";
    }
  }
  else
  {
    commit_or_save(ptx_vector, m_do_not_relay);
    submitted_to_network = true;
  }

  if (sweep_type == sweep_type_t::register_stake && submitted_to_network)
  {
    success_msg_writer() << tr("Wait for transaction to be included in a block before registration is complete.\n")
                         << tr("Use the print_sn command in the daemon to check the status.");
  }

  return true;
}

bool simple_wallet::sweep_main(uint64_t below, bool locked, const std::vector<std::string> &args_, tools::wallet2::sweep_style_t sweep_style)
{
  auto print_usage = [below]()
  {
    if (below)
    {
      PRINT_USAGE(USAGE_SWEEP_BELOW);
    }
    else
    {
      PRINT_USAGE(USAGE_SWEEP_ALL);
    }
  };

  if (args_.size() == 0)
  {
    fail_msg_writer() << tr("No address given");
    print_usage();
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  std::vector<std::string> local_args = args_;

  std::set<uint32_t> subaddr_indices;
  if (local_args.size() > 0 && local_args[0].substr(0, 6) == "index=")
  {
    std::string parse_subaddr_err;
    if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err))
    {
      fail_msg_writer() << parse_subaddr_err;
      print_usage();
      return true;
    }
    local_args.erase(local_args.begin());
  }

  uint32_t priority = 0;
  if (local_args.size() > 0 && tools::parse_priority(local_args[0], priority))
    local_args.erase(local_args.begin());

  priority = m_wallet->adjust_priority(priority);
  uint64_t unlock_block = 0;
  if (locked) {
    uint64_t locked_blocks = 0;

    if (local_args.size() < 2) {
      fail_msg_writer() << tr("missing lockedblocks parameter");
      return true;
    }

    try
    {
      locked_blocks = boost::lexical_cast<uint64_t>(local_args[1]);
    }
    catch (const std::exception &e)
    {
      fail_msg_writer() << tr("bad locked_blocks parameter");
      return true;
    }
    if (locked_blocks > 1000000)
    {
      fail_msg_writer() << tr("Locked blocks too high, max 1000000 (˜4 yrs)");
      return true;
    }
    std::string err;
    uint64_t bc_height = get_daemon_blockchain_height(err);
    if (!err.empty())
    {
      fail_msg_writer() << tr("failed to get blockchain height: ") << err;
      return true;
    }
    unlock_block = bc_height + locked_blocks;

    local_args.erase(local_args.begin() + 1);
  }

  size_t outputs = 1;
  if (local_args.size() > 0 && local_args[0].substr(0, 8) == "outputs=")
  {
    if (!epee::string_tools::get_xtype_from_string(outputs, local_args[0].substr(8)))
    {
      fail_msg_writer() << tr("Failed to parse number of outputs");
      return true;
    }
    else if (outputs < 1)
    {
      fail_msg_writer() << tr("Amount of outputs should be greater than 0");
      return true;
    }
    else
    {
      local_args.erase(local_args.begin());
    }
  }

  std::vector<uint8_t> extra;
  bool payment_id_seen = false;
  if (local_args.size() >= 2)
  {
    std::string payment_id_str = local_args.back();

    crypto::hash payment_id;
    bool r = tools::wallet2::parse_long_payment_id(payment_id_str, payment_id);
    if(r)
    {
      LONG_PAYMENT_ID_SUPPORT_CHECK();

      std::string extra_nonce;
      set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
      r = add_extra_nonce_to_tx_extra(extra, extra_nonce);
      payment_id_seen = true;
    }

    if(!r && local_args.size() == 3)
    {
      fail_msg_writer() << tr("payment id has invalid format, expected 16 or 64 character hex string: ") << payment_id_str;
      print_usage();
      return true;
    }
    if (payment_id_seen)
      local_args.pop_back();
  }

  cryptonote::address_parse_info info;
  if (!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), local_args[0], oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    print_usage();
    return true;
  }

  if (info.has_payment_id)
  {
    if (payment_id_seen)
    {
      fail_msg_writer() << tr("a single transaction cannot use more than one payment id: ") << local_args[0];
      return true;
    }

    std::string extra_nonce;
    set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, info.payment_id);
    bool r = add_extra_nonce_to_tx_extra(extra, extra_nonce);
    if(!r)
    {
      fail_msg_writer() << tr("failed to set up payment id, though it was decoded correctly");
      return true;
    }
    payment_id_seen = true;
  }

  // prompt is there is no payment id and confirmation is required
  if (m_long_payment_id_support && !payment_id_seen && m_wallet->confirm_missing_payment_id() && !info.is_subaddress)
  {
     std::string accepted = input_line(tr("No payment id is included with this transaction. Is this okay?"), true);
     if (std::cin.eof())
       return true;
     if (!command_line::is_yes(accepted))
     {
       fail_msg_writer() << tr("transaction cancelled.");

       return true; 
     }
  }

  SCOPED_WALLET_UNLOCK();
  try
  {
    auto ptx_vector = m_wallet->create_transactions_all(below, info.address, info.is_subaddress, outputs, CRYPTONOTE_DEFAULT_TX_MIXIN, unlock_block /* unlock_time */, priority, extra, m_current_subaddress_account, subaddr_indices, false, sweep_style);
    sweep_main_internal(sweep_type_t::all_or_below, ptx_vector, info);
  }
  catch (const std::exception &e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sweep_single(const std::vector<std::string> &args_)
{
  if (!try_connect_to_daemon())
    return true;

  std::vector<std::string> local_args = args_;

  uint32_t priority = 0;
  if (local_args.size() > 0 && tools::parse_priority(local_args[0], priority))
    local_args.erase(local_args.begin());

  priority = m_wallet->adjust_priority(priority);

  size_t outputs = 1;
  if (local_args.size() > 0 && local_args[0].substr(0, 8) == "outputs=")
  {
    if (!epee::string_tools::get_xtype_from_string(outputs, local_args[0].substr(8)))
    {
      fail_msg_writer() << tr("Failed to parse number of outputs");
      return true;
    }
    else if (outputs < 1)
    {
      fail_msg_writer() << tr("Amount of outputs should be greater than 0");
      return true;
    }
    else
    {
      local_args.erase(local_args.begin());
    }
  }

  std::vector<uint8_t> extra;
  bool payment_id_seen = false;
  if (local_args.size() == 3)
  {
    crypto::hash payment_id;
    crypto::hash8 payment_id8;
    std::string extra_nonce;
    if (tools::wallet2::parse_long_payment_id(local_args.back(), payment_id))
    {
      LONG_PAYMENT_ID_SUPPORT_CHECK();
      set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
    }
    else
    {
      fail_msg_writer() << tr("failed to parse Payment ID");
      return true;
    }

    if (!add_extra_nonce_to_tx_extra(extra, extra_nonce))
    {
      fail_msg_writer() << tr("failed to set up payment id, though it was decoded correctly");
      return true;
    }

    local_args.pop_back();
    payment_id_seen = true;
  }

  if (local_args.size() != 2)
  {
    PRINT_USAGE(USAGE_SWEEP_SINGLE);
    return true;
  }

  crypto::key_image ki;
  if (!epee::string_tools::hex_to_pod(local_args[0], ki))
  {
    fail_msg_writer() << tr("failed to parse key image");
    return true;
  }

  cryptonote::address_parse_info info;
  if (!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), local_args[1], oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    return true;
  }

  if (info.has_payment_id)
  {
    if (payment_id_seen)
    {
      fail_msg_writer() << tr("a single transaction cannot use more than one payment id: ") << local_args[0];
      return true;
    }

    std::string extra_nonce;
    set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, info.payment_id);
    if (!add_extra_nonce_to_tx_extra(extra, extra_nonce))
    {
      fail_msg_writer() << tr("failed to set up payment id, though it was decoded correctly");
      return true;
    }
    payment_id_seen = true;
  }

  // prompt if there is no payment id and confirmation is required
  if (m_long_payment_id_support && !payment_id_seen && m_wallet->confirm_missing_payment_id() && !info.is_subaddress)
  {
     std::string accepted = input_line(tr("No payment id is included with this transaction. Is this okay?"), true);
     if (std::cin.eof())
       return true;
     if (!command_line::is_yes(accepted))
     {
       fail_msg_writer() << tr("transaction cancelled.");

       // would like to return false, because no tx made, but everything else returns true
       // and I don't know what returning false might adversely affect.  *sigh*
       return true; 
     }
  }

  SCOPED_WALLET_UNLOCK();

  try
  {
    // figure out what tx will be necessary
    auto ptx_vector = m_wallet->create_transactions_single(ki, info.address, info.is_subaddress, outputs, CRYPTONOTE_DEFAULT_TX_MIXIN, 0 /* unlock_time */, priority, extra);
    sweep_main_internal(sweep_type_t::single, ptx_vector, info);
  }
  catch (const std::exception& e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sweep_all(const std::vector<std::string> &args_)
{
  std::vector<std::string> copied_args = args_;

  tools::wallet2::sweep_style_t sweep_style = tools::wallet2::sweep_style_t::normal;
  if (args_.size() > 0)
  {
    std::string const &last_arg = copied_args.back();
    if (last_arg == "use_v1_tx")
    {
      sweep_style = tools::wallet2::sweep_style_t::use_v1_tx;
      copied_args.erase(copied_args.end() - 1);
    }
  }

  return sweep_main(0, false, copied_args, sweep_style);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sweep_below(const std::vector<std::string> &args_)
{
  uint64_t below = 0;
  if (args_.size() < 1)
  {
    fail_msg_writer() << tr("missing threshold amount");
    return true;
  }
  if (!cryptonote::parse_amount(below, args_[0]))
  {
    fail_msg_writer() << tr("invalid amount threshold");
    return true;
  }
  return sweep_main(below, false, std::vector<std::string>(++args_.begin(), args_.end()));
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::accept_loaded_tx(const std::function<size_t()> get_num_txes, const std::function<const tools::wallet2::tx_construction_data&(size_t)> &get_tx, const std::string &extra_message)
{
  // gather info to ask the user
  uint64_t amount = 0, amount_to_dests = 0, change = 0;
  size_t min_ring_size = ~0;
  std::unordered_map<cryptonote::account_public_address, std::pair<std::string, uint64_t>> dests;
  int first_known_non_zero_change_index = -1;
  std::string payment_id_string = "";
  for (size_t n = 0; n < get_num_txes(); ++n)
  {
    const tools::wallet2::tx_construction_data &cd = get_tx(n);

    std::vector<tx_extra_field> tx_extra_fields;
    bool has_encrypted_payment_id = false;
    crypto::hash8 payment_id8 = crypto::null_hash8;
    if (cryptonote::parse_tx_extra(cd.extra, tx_extra_fields))
    {
      tx_extra_nonce extra_nonce;
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash payment_id;
        if(get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
        {
          if (!payment_id_string.empty())
            payment_id_string += ", ";

          // if none of the addresses are integrated addresses, it's a dummy one
          bool is_dummy = true;
          for (const auto &e: cd.dests)
            if (e.is_integrated)
              is_dummy = false;

          if (is_dummy)
          {
            payment_id_string += std::string("dummy encrypted payment ID");
          }
          else
          {
            payment_id_string += std::string("encrypted payment ID ") + epee::string_tools::pod_to_hex(payment_id8);
            has_encrypted_payment_id = true;
          }
        }
        else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          if (!payment_id_string.empty())
            payment_id_string += ", ";
          payment_id_string += std::string("unencrypted payment ID ") + epee::string_tools::pod_to_hex(payment_id);
          payment_id_string += " (OBSOLETE)";
        }
      }
    }

    for (size_t s = 0; s < cd.sources.size(); ++s)
    {
      amount += cd.sources[s].amount;
      size_t ring_size = cd.sources[s].outputs.size();
      if (ring_size < min_ring_size)
        min_ring_size = ring_size;
    }
    for (size_t d = 0; d < cd.splitted_dsts.size(); ++d)
    {
      const tx_destination_entry &entry = cd.splitted_dsts[d];
      std::string address, standard_address = get_account_address_as_str(m_wallet->nettype(), entry.is_subaddress, entry.addr);
      if (has_encrypted_payment_id && !entry.is_subaddress)
      {
        address = get_account_integrated_address_as_str(m_wallet->nettype(), entry.addr, payment_id8);
        address += std::string(" (" + standard_address + " with encrypted payment id " + epee::string_tools::pod_to_hex(payment_id8) + ")");
      }
      else
        address = standard_address;
      auto i = dests.find(entry.addr);
      if (i == dests.end())
        dests.insert(std::make_pair(entry.addr, std::make_pair(address, entry.amount)));
      else
        i->second.second += entry.amount;
      amount_to_dests += entry.amount;
    }
    if (cd.change_dts.amount > 0)
    {
      auto it = dests.find(cd.change_dts.addr);
      if (it == dests.end())
      {
        fail_msg_writer() << tr("Claimed change does not go to a paid address");
        return false;
      }
      if (it->second.second < cd.change_dts.amount)
      {
        fail_msg_writer() << tr("Claimed change is larger than payment to the change address");
        return false;
      }
      if (cd.change_dts.amount > 0)
      {
        if (first_known_non_zero_change_index == -1)
          first_known_non_zero_change_index = n;
        if (memcmp(&cd.change_dts.addr, &get_tx(first_known_non_zero_change_index).change_dts.addr, sizeof(cd.change_dts.addr)))
        {
          fail_msg_writer() << tr("Change goes to more than one address");
          return false;
        }
      }
      change += cd.change_dts.amount;
      it->second.second -= cd.change_dts.amount;
      if (it->second.second == 0)
        dests.erase(cd.change_dts.addr);
    }
  }

  if (payment_id_string.empty())
    payment_id_string = "no payment ID";

  std::string dest_string;
  size_t n_dummy_outputs = 0;
  for (auto i = dests.begin(); i != dests.end(); )
  {
    if (i->second.second > 0)
    {
      if (!dest_string.empty())
        dest_string += ", ";
      dest_string += (boost::format(tr("sending %s to %s")) % print_money(i->second.second) % i->second.first).str();
    }
    else
      ++n_dummy_outputs;
    ++i;
  }
  if (n_dummy_outputs > 0)
  {
    if (!dest_string.empty())
      dest_string += ", ";
    dest_string += std::to_string(n_dummy_outputs) + tr(" dummy output(s)");
  }
  if (dest_string.empty())
    dest_string = tr("with no destinations");

  std::string change_string;
  if (change > 0)
  {
    std::string address = get_account_address_as_str(m_wallet->nettype(), get_tx(0).subaddr_account > 0, get_tx(0).change_dts.addr);
    change_string += (boost::format(tr("%s change to %s")) % print_money(change) % address).str();
  }
  else
    change_string += tr("no change");

  uint64_t fee = amount - amount_to_dests;
  std::string prompt_str = (boost::format(tr("Loaded %lu transactions, for %s, fee %s, %s, %s, with min ring size %lu, %s. %sIs this okay?")) % (unsigned long)get_num_txes() % print_money(amount) % print_money(fee) % dest_string % change_string % (unsigned long)min_ring_size % payment_id_string % extra_message).str();
  return command_line::is_yes(input_line(prompt_str, true));
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::accept_loaded_tx(const tools::wallet2::unsigned_tx_set &txs)
{
  std::string extra_message;
  if (!txs.transfers.second.empty())
    extra_message = (boost::format("%u outputs to import. ") % (unsigned)txs.transfers.second.size()).str();
  return accept_loaded_tx([&txs](){return txs.txes.size();}, [&txs](size_t n)->const tools::wallet2::tx_construction_data&{return txs.txes[n];}, extra_message);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::accept_loaded_tx(const tools::wallet2::signed_tx_set &txs)
{
  std::string extra_message;
  if (!txs.key_images.empty())
    extra_message = (boost::format("%u key images to import. ") % (unsigned)txs.key_images.size()).str();
  return accept_loaded_tx([&txs](){return txs.ptx.size();}, [&txs](size_t n)->const tools::wallet2::tx_construction_data&{return txs.ptx[n].construction_data;}, extra_message);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sign_transfer(const std::vector<std::string> &args_)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if(m_wallet->multisig())
  {
     fail_msg_writer() << tr("This is a multisig wallet, it can only sign with sign_multisig");
     return true;
  }
  if(m_wallet->watch_only())
  {
     fail_msg_writer() << tr("This is a watch only wallet");
     return true;
  }
  if (args_.size() > 1 || (args_.size() == 1 && args_[0] != "export_raw"))
  {
    PRINT_USAGE(USAGE_SIGN_TRANSFER);
    return true;
  }

  SCOPED_WALLET_UNLOCK();
  const bool export_raw = args_.size() == 1;

  std::vector<tools::wallet2::pending_tx> ptx;
  try
  {
    bool r = m_wallet->sign_tx("unsigned_antd_tx", "signed_antd_tx", ptx, [&](const tools::wallet2::unsigned_tx_set &tx){ return accept_loaded_tx(tx); }, export_raw);
    if (!r)
    {
      fail_msg_writer() << tr("Failed to sign transaction");
      return true;
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to sign transaction: ") << e.what();
    return true;
  }

  std::string txids_as_text;
  for (const auto &t: ptx)
  {
    if (!txids_as_text.empty())
      txids_as_text += (", ");
    txids_as_text += epee::string_tools::pod_to_hex(get_transaction_hash(t.tx));
  }
  success_msg_writer(true) << tr("Transaction successfully signed to file ") << "signed_antd_tx" << ", txid " << txids_as_text;
  if (export_raw)
  {
    std::string rawfiles_as_text;
    for (size_t i = 0; i < ptx.size(); ++i)
    {
      if (i > 0)
        rawfiles_as_text += ", ";
      rawfiles_as_text += "signed_antd_tx_raw" + (ptx.size() == 1 ? "" : ("_" + std::to_string(i)));
    }
    success_msg_writer(true) << tr("Transaction raw hex data exported to ") << rawfiles_as_text;
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::submit_transfer(const std::vector<std::string> &args_)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (!try_connect_to_daemon())
    return true;

  try
  {
    std::vector<tools::wallet2::pending_tx> ptx_vector;
    bool r = m_wallet->load_tx("signed_antd_tx", ptx_vector, [&](const tools::wallet2::signed_tx_set &tx){ return accept_loaded_tx(tx); });
    if (!r)
    {
      fail_msg_writer() << tr("Failed to load transaction from file");
      return true;
    }

    commit_or_save(ptx_vector, false);
  }
  catch (const std::exception& e)
  {
    handle_transfer_exception(std::current_exception(), m_wallet->is_trusted_daemon());
  }
  catch (...)
  {
    LOG_ERROR("Unknown error");
    fail_msg_writer() << tr("unknown error");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_tx_key(const std::vector<std::string> &args_)
{
  std::vector<std::string> local_args = args_;

  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if(local_args.size() != 1) {
    PRINT_USAGE(USAGE_GET_TX_KEY);
    return true;
  }

  crypto::hash txid;
  if (!epee::string_tools::hex_to_pod(local_args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  SCOPED_WALLET_UNLOCK();

  crypto::secret_key tx_key;
  std::vector<crypto::secret_key> additional_tx_keys;
  if (m_wallet->get_tx_key(txid, tx_key, additional_tx_keys))
  {
    ostringstream oss;
    oss << epee::string_tools::pod_to_hex(tx_key);
    for (size_t i = 0; i < additional_tx_keys.size(); ++i)
      oss << epee::string_tools::pod_to_hex(additional_tx_keys[i]);
    success_msg_writer() << tr("Tx key: ") << oss.str();
    return true;
  }
  else
  {
    fail_msg_writer() << tr("no tx keys found for this txid");
    return true;
  }
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_tx_key(const std::vector<std::string> &args_)
{
  std::vector<std::string> local_args = args_;

  if(local_args.size() != 2) {
    PRINT_USAGE(USAGE_SET_TX_KEY);
    return true;
  }

  crypto::hash txid;
  if (!epee::string_tools::hex_to_pod(local_args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  crypto::secret_key tx_key;
  std::vector<crypto::secret_key> additional_tx_keys;
  try
  {
    if (!epee::string_tools::hex_to_pod(local_args[1].substr(0, 64), tx_key))
    {
      fail_msg_writer() << tr("failed to parse tx_key");
      return true;
    }
    while(true)
    {
      local_args[1] = local_args[1].substr(64);
      if (local_args[1].empty())
        break;
      additional_tx_keys.resize(additional_tx_keys.size() + 1);
      if (!epee::string_tools::hex_to_pod(local_args[1].substr(0, 64), additional_tx_keys.back()))
      {
        fail_msg_writer() << tr("failed to parse tx_key");
        return true;
      }
    }
  }
  catch (const std::out_of_range &e)
  {
    fail_msg_writer() << tr("failed to parse tx_key");
    return true;
  }

  LOCK_IDLE_SCOPE();

  try
  {
    m_wallet->set_tx_key(txid, tx_key, additional_tx_keys);
    success_msg_writer() << tr("Tx key successfully stored.");
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to store tx key: ") << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_tx_proof(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (args.size() != 2 && args.size() != 3)
  {
    PRINT_USAGE(USAGE_GET_TX_PROOF);
    return true;
  }

  crypto::hash txid;
  if(!epee::string_tools::hex_to_pod(args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  cryptonote::address_parse_info info;
  if(!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), args[1], oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    return true;
  }

  SCOPED_WALLET_UNLOCK();

  try
  {
    std::string sig_str = m_wallet->get_tx_proof(txid, info.address, info.is_subaddress, args.size() == 3 ? args[2] : "");
    const std::string filename = "antd_tx_proof";
    if (epee::file_io_utils::save_string_to_file(filename, sig_str))
      success_msg_writer() << tr("signature file saved to: ") << filename;
    else
      fail_msg_writer() << tr("failed to save signature file");
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("error: ") << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::check_tx_key(const std::vector<std::string> &args_)
{
  std::vector<std::string> local_args = args_;

  if(local_args.size() != 3) {
    PRINT_USAGE(USAGE_CHECK_TX_KEY);
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  if (!m_wallet)
  {
    fail_msg_writer() << tr("wallet is null");
    return true;
  }
  crypto::hash txid;
  if(!epee::string_tools::hex_to_pod(local_args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  crypto::secret_key tx_key;
  std::vector<crypto::secret_key> additional_tx_keys;
  if(!epee::string_tools::hex_to_pod(local_args[1].substr(0, 64), tx_key))
  {
    fail_msg_writer() << tr("failed to parse tx key");
    return true;
  }
  local_args[1] = local_args[1].substr(64);
  while (!local_args[1].empty())
  {
    additional_tx_keys.resize(additional_tx_keys.size() + 1);
    if(!epee::string_tools::hex_to_pod(local_args[1].substr(0, 64), additional_tx_keys.back()))
    {
      fail_msg_writer() << tr("failed to parse tx key");
      return true;
    }
    local_args[1] = local_args[1].substr(64);
  }

  cryptonote::address_parse_info info;
  if(!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), local_args[2], oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    return true;
  }

  try
  {
    uint64_t received;
    bool in_pool;
    uint64_t confirmations;
    m_wallet->check_tx_key(txid, tx_key, additional_tx_keys, info.address, received, in_pool, confirmations);

    if (received > 0)
    {
      success_msg_writer() << get_account_address_as_str(m_wallet->nettype(), info.is_subaddress, info.address) << " " << tr("received") << " " << print_money(received) << " " << tr("in txid") << " " << txid;
      if (in_pool)
      {
        success_msg_writer() << tr("WARNING: this transaction is not yet included in the blockchain!");
      }
      else
      {
        if (confirmations != (uint64_t)-1)
        {
          success_msg_writer() << boost::format(tr("This transaction has %u confirmations")) % confirmations;
        }
        else
        {
          success_msg_writer() << tr("WARNING: failed to determine number of confirmations!");
        }
      }
    }
    else
    {
      fail_msg_writer() << get_account_address_as_str(m_wallet->nettype(), info.is_subaddress, info.address) << " " << tr("received nothing in txid") << " " << txid;
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("error: ") << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::check_tx_proof(const std::vector<std::string> &args)
{
  if(args.size() != 3 && args.size() != 4) {
    PRINT_USAGE(USAGE_CHECK_TX_PROOF);
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  // parse txid
  crypto::hash txid;
  if(!epee::string_tools::hex_to_pod(args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  // parse address
  cryptonote::address_parse_info info;
  if(!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), args[1], oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    return true;
  }

  // read signature file
  std::string sig_str;
  if (!epee::file_io_utils::load_file_to_string(args[2], sig_str))
  {
    fail_msg_writer() << tr("failed to load signature file");
    return true;
  }

  try
  {
    uint64_t received;
    bool in_pool;
    uint64_t confirmations;
    if (m_wallet->check_tx_proof(txid, info.address, info.is_subaddress, args.size() == 4 ? args[3] : "", sig_str, received, in_pool, confirmations))
    {
      success_msg_writer() << tr("Good signature");
      if (received > 0)
      {
        success_msg_writer() << get_account_address_as_str(m_wallet->nettype(), info.is_subaddress, info.address) << " " << tr("received") << " " << print_money(received) << " " << tr("in txid") << " " << txid;
        if (in_pool)
        {
          success_msg_writer() << tr("WARNING: this transaction is not yet included in the blockchain!");
        }
        else
        {
          if (confirmations != (uint64_t)-1)
          {
            success_msg_writer() << boost::format(tr("This transaction has %u confirmations")) % confirmations;
          }
          else
          {
            success_msg_writer() << tr("WARNING: failed to determine number of confirmations!");
          }
        }
      }
      else
      {
        fail_msg_writer() << get_account_address_as_str(m_wallet->nettype(), info.is_subaddress, info.address) << " " << tr("received nothing in txid") << " " << txid;
      }
    }
    else
    {
      fail_msg_writer() << tr("Bad signature");
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("error: ") << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_spend_proof(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if(args.size() != 1 && args.size() != 2) {
    PRINT_USAGE(USAGE_GET_SPEND_PROOF);
    return true;
  }

  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and cannot generate the proof");
    return true;
  }

  crypto::hash txid;
  if (!epee::string_tools::hex_to_pod(args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  SCOPED_WALLET_UNLOCK();

  try
  {
    const std::string sig_str = m_wallet->get_spend_proof(txid, args.size() == 2 ? args[1] : "");
    const std::string filename = "antd_spend_proof";
    if (epee::file_io_utils::save_string_to_file(filename, sig_str))
      success_msg_writer() << tr("signature file saved to: ") << filename;
    else
      fail_msg_writer() << tr("failed to save signature file");
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::check_spend_proof(const std::vector<std::string> &args)
{
  if(args.size() != 2 && args.size() != 3) {
    PRINT_USAGE(USAGE_CHECK_SPEND_PROOF);
    return true;
  }

  crypto::hash txid;
  if (!epee::string_tools::hex_to_pod(args[0], txid))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  std::string sig_str;
  if (!epee::file_io_utils::load_file_to_string(args[1], sig_str))
  {
    fail_msg_writer() << tr("failed to load signature file");
    return true;
  }

  try
  {
    if (m_wallet->check_spend_proof(txid, args.size() == 3 ? args[2] : "", sig_str))
      success_msg_writer() << tr("Good signature");
    else
      fail_msg_writer() << tr("Bad signature");
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_reserve_proof(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if(args.size() != 1 && args.size() != 2) {
    PRINT_USAGE(USAGE_GET_RESERVE_PROOF);
    return true;
  }

  if (m_wallet->watch_only() || m_wallet->multisig())
  {
    fail_msg_writer() << tr("The reserve proof can be generated only by a full wallet");
    return true;
  }

  boost::optional<std::pair<uint32_t, uint64_t>> account_minreserve;
  if (args[0] != "all")
  {
    account_minreserve = std::pair<uint32_t, uint64_t>();
    account_minreserve->first = m_current_subaddress_account;
    if (!cryptonote::parse_amount(account_minreserve->second, args[0]))
    {
      fail_msg_writer() << tr("amount is wrong: ") << args[0];
      return true;
    }
  }

  if (!try_connect_to_daemon())
    return true;

  SCOPED_WALLET_UNLOCK();

  try
  {
    const std::string sig_str = m_wallet->get_reserve_proof(account_minreserve, args.size() == 2 ? args[1] : "");
    const std::string filename = "antd_reserve_proof";
    if (epee::file_io_utils::save_string_to_file(filename, sig_str))
      success_msg_writer() << tr("signature file saved to: ") << filename;
    else
      fail_msg_writer() << tr("failed to save signature file");
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::check_reserve_proof(const std::vector<std::string> &args)
{
  if(args.size() != 2 && args.size() != 3) {
    PRINT_USAGE(USAGE_CHECK_RESERVE_PROOF);
    return true;
  }

  if (!try_connect_to_daemon())
    return true;

  cryptonote::address_parse_info info;
  if(!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), args[0], oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    return true;
  }
  if (info.is_subaddress)
  {
    fail_msg_writer() << tr("Address must not be a subaddress");
    return true;
  }

  std::string sig_str;
  if (!epee::file_io_utils::load_file_to_string(args[1], sig_str))
  {
    fail_msg_writer() << tr("failed to load signature file");
    return true;
  }

  LOCK_IDLE_SCOPE();

  try
  {
    uint64_t total, spent;
    if (m_wallet->check_reserve_proof(info.address, args.size() == 3 ? args[2] : "", sig_str, total, spent))
    {
      success_msg_writer() << boost::format(tr("Good signature -- total: %s, spent: %s, unspent: %s")) % print_money(total) % print_money(spent) % print_money(total - spent);
    }
    else
    {
      fail_msg_writer() << tr("Bad signature");
    }
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << e.what();
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
// mutates local_args as it parses and consumes arguments
bool simple_wallet::get_transfers(std::vector<std::string>& local_args, std::vector<transfer_view>& transfers)
{
  bool in = true;
  bool out = true;
  bool pending = true;
  bool failed = true;
  bool pool = true;
  bool coinbase = true;
  uint64_t min_height = 0;
  uint64_t max_height = (uint64_t)-1;

  // optional in/out selector
  if (local_args.size() > 0) {
    if (local_args[0] == "in" || local_args[0] == "incoming") {
      out = pending = failed = false;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "out" || local_args[0] == "outgoing") {
      in = pool = coinbase = false;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "pending") {
      in = out = failed = coinbase = false;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "failed") {
      in = out = pending = pool = coinbase = false;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "pool") {
      in = out = pending = failed = coinbase = false;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "coinbase") {
      in = out = pending = failed = pool = false;
      coinbase = true;
      local_args.erase(local_args.begin());
    }
    else if (local_args[0] == "all" || local_args[0] == "both") {
      local_args.erase(local_args.begin());
    }
  }

  // subaddr_index
  std::set<uint32_t> subaddr_indices;
  if (local_args.size() > 0 && local_args[0].substr(0, 6) == "index=")
  {
    std::string parse_subaddr_err;
    if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err))
    {
      fail_msg_writer() << parse_subaddr_err;
      return true;
    }
    local_args.erase(local_args.begin());
  }

  // min height
  if (local_args.size() > 0 && local_args[0].find('=') == std::string::npos) {
    try {
      min_height = boost::lexical_cast<uint64_t>(local_args[0]);
    }
    catch (const boost::bad_lexical_cast &) {
      fail_msg_writer() << tr("bad min_height parameter:") << " " << local_args[0];
      return false;
    }
    local_args.erase(local_args.begin());
  }

  // max height
  if (local_args.size() > 0 && local_args[0].find('=') == std::string::npos) {
    try {
      max_height = boost::lexical_cast<uint64_t>(local_args[0]);
    }
    catch (const boost::bad_lexical_cast &) {
      fail_msg_writer() << tr("bad max_height parameter:") << " " << local_args[0];
      return false;
    }
    local_args.erase(local_args.begin());
  }

  if (in || coinbase) {
    std::list<std::pair<crypto::hash, tools::wallet2::payment_details>> payments;
    m_wallet->get_payments(payments, min_height, max_height, m_current_subaddress_account, subaddr_indices);

    for (std::list<std::pair<crypto::hash, tools::wallet2::payment_details>>::const_iterator i = payments.begin(); i != payments.end(); ++i) {
      const tools::wallet2::payment_details &pd = i->second;
      if (!pd.is_coinbase() && !in)
        continue;
      std::string payment_id = string_tools::pod_to_hex(i->first);
      if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
        payment_id = payment_id.substr(0,16);
      std::string note = m_wallet->get_tx_note(pd.m_tx_hash);

      std::string destination = m_wallet->get_subaddress_as_str({m_current_subaddress_account, pd.m_subaddr_index.minor});
      transfers.push_back({
        pd.m_block_height,
        pd.m_timestamp,
        pd.m_type,
        true, // confirmed
        pd.m_amount,
        pd.m_tx_hash,
        payment_id,
        0,
        {{destination, pd.m_amount, pd.m_unlock_time}},
        {pd.m_subaddr_index.minor},
        note,
        m_wallet->is_transfer_unlocked(pd.m_unlock_time, pd.m_block_height),
      });
    }
  }

  if (out) {
    std::list<std::pair<crypto::hash, tools::wallet2::confirmed_transfer_details>> payments;
    m_wallet->get_payments_out(payments, min_height, max_height, m_current_subaddress_account, subaddr_indices);
    for (std::list<std::pair<crypto::hash, tools::wallet2::confirmed_transfer_details>>::const_iterator i = payments.begin(); i != payments.end(); ++i) {
      const tools::wallet2::confirmed_transfer_details &pd = i->second;
      uint64_t change = pd.m_change == (uint64_t)-1 ? 0 : pd.m_change; // change may not be known
      uint64_t fee = pd.m_amount_in - pd.m_amount_out;

      std::vector<transfer_view::dest_output> destinations(pd.m_dests.size());
      for (size_t dest_index  = 0; dest_index < pd.m_dests.size(); ++dest_index)
      {
        const tx_destination_entry &dest   = pd.m_dests[dest_index];
        transfer_view::dest_output &output = destinations[dest_index];
        output.wallet_addr                 = get_account_address_as_str(m_wallet->nettype(), dest.is_subaddress, dest.addr);
        output.amount                      = dest.amount;
        output.unlock_time                 = (dest_index < pd.m_unlock_times.size()) ? pd.m_unlock_times[dest_index] : 0;
      }

      // TODO(doyle): Broken lock time isnt used anymore.
      // NOTE(antd): Technically we don't allow custom unlock times per output
      // yet. So if we detect _any_ output that has the staking lock time, then
      // we can assume it's a staking transfer
      const uint64_t staking_duration = full_nodes::staking_num_lock_blocks(m_wallet->nettype());
      bool locked = false;

      tools::pay_type type = tools::pay_type::out;
      for (size_t unlock_index = 0; unlock_index < pd.m_unlock_times.size() && type != tools::pay_type::stake; ++unlock_index)
      {
        uint64_t unlock_time = pd.m_unlock_times[unlock_index];
        if (unlock_time < pd.m_block_height)
          continue;

        // NOTE: If any output is locked at all, consider the transfer locked.
        uint64_t lock_duration = unlock_time - pd.m_block_height;
        locked |= (!m_wallet->is_transfer_unlocked(unlock_time, pd.m_block_height));
        if (lock_duration >= staking_duration) type = tools::pay_type::stake;
      }

      std::string payment_id = string_tools::pod_to_hex(i->second.m_payment_id);
      if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
        payment_id = payment_id.substr(0,16);
      std::string note = m_wallet->get_tx_note(i->first);

      transfers.push_back({
        pd.m_block_height,
        pd.m_timestamp,
        type,
        true, // confirmed
        pd.m_amount_in - change - fee,
        i->first,
        payment_id,
        fee,
        destinations,
        pd.m_subaddr_indices,
        note,
        !locked,
      });
    }
  }

  if (pool) {
    try
    {
      m_in_manual_refresh.store(true, std::memory_order_relaxed);
      epee::misc_utils::auto_scope_leave_caller scope_exit_handler = epee::misc_utils::create_scope_leave_handler([&](){m_in_manual_refresh.store(false, std::memory_order_relaxed);});

      m_wallet->update_pool_state();
      std::list<std::pair<crypto::hash, tools::wallet2::pool_payment_details>> payments;
      m_wallet->get_unconfirmed_payments(payments, m_current_subaddress_account, subaddr_indices);
      for (std::list<std::pair<crypto::hash, tools::wallet2::pool_payment_details>>::const_iterator i = payments.begin(); i != payments.end(); ++i) {
        const tools::wallet2::payment_details &pd = i->second.m_pd;
        std::string payment_id = string_tools::pod_to_hex(i->first);
        if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
          payment_id = payment_id.substr(0,16);
        std::string note = m_wallet->get_tx_note(pd.m_tx_hash);
        std::string destination = m_wallet->get_subaddress_as_str({m_current_subaddress_account, pd.m_subaddr_index.minor});
        std::string double_spend_note;
        if (i->second.m_double_spend_seen)
          double_spend_note = tr("[Double spend seen on the network: this transaction may or may not end up being mined] ");

        transfers.push_back({
          "pool",
          pd.m_timestamp,
          tools::pay_type::in,
          false, // confirmed
          pd.m_amount,
          pd.m_tx_hash,
          payment_id,
          0,
          {{destination, pd.m_amount}},
          {pd.m_subaddr_index.minor},
          note + double_spend_note,
          false, // unlocked
        });
      }
    }
    catch (const std::exception& e)
    {
      fail_msg_writer() << "Failed to get pool state:" << e.what();
    }
  }

  // print unconfirmed last
  if (pending || failed) {
    std::list<std::pair<crypto::hash, tools::wallet2::unconfirmed_transfer_details>> upayments;
    m_wallet->get_unconfirmed_payments_out(upayments, m_current_subaddress_account, subaddr_indices);
    for (std::list<std::pair<crypto::hash, tools::wallet2::unconfirmed_transfer_details>>::const_iterator i = upayments.begin(); i != upayments.end(); ++i) {
      const tools::wallet2::unconfirmed_transfer_details &pd = i->second;
      uint64_t amount = pd.m_amount_in;
      uint64_t fee = amount - pd.m_amount_out;

      std::vector<transfer_view::dest_output> destinations(pd.m_dests.size());
      for (size_t dest_index  = 0; dest_index < pd.m_dests.size(); ++dest_index)
      {
        const tx_destination_entry &dest   = pd.m_dests[dest_index];
        transfer_view::dest_output &output = destinations[dest_index];
        output.wallet_addr                 = get_account_address_as_str(m_wallet->nettype(), dest.is_subaddress, dest.addr);
        output.amount                      = dest.amount;
      }

      std::string payment_id = string_tools::pod_to_hex(i->second.m_payment_id);
      if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
        payment_id = payment_id.substr(0,16);
      std::string note = m_wallet->get_tx_note(i->first);
      bool is_failed = pd.m_state == tools::wallet2::unconfirmed_transfer_details::failed;
      if ((failed && is_failed) || (!is_failed && pending)) {
        transfers.push_back({
          (is_failed ? "failed" : "pending"),
          pd.m_timestamp,
          tools::pay_type::out,
          false, // confirmed
          amount - pd.m_change - fee,
          i->first,
          payment_id,
          fee,
          destinations,
          pd.m_subaddr_indices,
          note,
          false, // unlocked
        });
      }
    }
  }
  // sort by block, then by timestamp (unconfirmed last)
  std::sort(transfers.begin(), transfers.end(), [](const transfer_view& a, const transfer_view& b) -> bool {
    if (a.confirmed && !b.confirmed)
      return true;
    if (a.block == b.block)
      return a.timestamp < b.timestamp;
    return a.block < b.block;
  });

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_transfers(const std::vector<std::string> &args_)
{
  std::vector<std::string> local_args = args_;

  if(local_args.size() > 4)
  {
    PRINT_USAGE(USAGE_SHOW_TRANSFERS);
    return true;
  }

  LOCK_IDLE_SCOPE();

  std::vector<transfer_view> all_transfers;

  if (!get_transfers(local_args, all_transfers))
    return true;

  PAUSE_READLINE();
  for (const auto& transfer : all_transfers)
  {
    enum console_colors color = console_color_white;
    if (transfer.confirmed)
    {
      switch (transfer.type)
      {
        case tools::pay_type::in:           color = console_color_green; break;
        case tools::pay_type::out:          color = console_color_yellow; break;
        case tools::pay_type::miner:        color = console_color_cyan; break;
        case tools::pay_type::control:   color = console_color_cyan; break;
        case tools::pay_type::stake:        color = console_color_blue; break;
        case tools::pay_type::full_node: color = console_color_cyan; break;
        default:                            color = console_color_magenta; break;
      }
    }

    if (transfer.block.type() == typeid(std::string))
    {
      const std::string& block_str = boost::get<std::string>(transfer.block);
      if (block_str == "failed") color = console_color_red;
    }

    std::string destinations = "-";
    if (!transfer.outputs.empty())
    {
      destinations = "";
      for (const auto& output : transfer.outputs)
      {
        if (!destinations.empty())
          destinations += ", ";

        if (transfer.type == tools::pay_type::in ||
            transfer.type == tools::pay_type::control ||
            transfer.type == tools::pay_type::full_node ||
            transfer.type == tools::pay_type::miner)
          destinations += output.wallet_addr.substr(0, 6);
        else
          destinations += output.wallet_addr;

        destinations += ":" + print_money(output.amount);
      }
    }
    
    auto formatter = boost::format("%8.8llu %6.6s %8.8s %16.16s %20.20s %s %s %14.14s %s %s - %s");

    char const *lock_str = (transfer.unlocked) ? "unlocked" : "locked";
    message_writer(color, false) << formatter
      % transfer.block
      % tools::pay_type_string(transfer.type)
      % lock_str
      % tools::get_human_readable_timestamp(transfer.timestamp)
      % print_money(transfer.amount)
      % string_tools::pod_to_hex(transfer.hash)
      % transfer.payment_id
      % print_money(transfer.fee)
      % destinations
      % boost::algorithm::join(transfer.index | boost::adaptors::transformed([](uint32_t i) { return std::to_string(i); }), ", ")
      % transfer.note;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::export_transfers(const std::vector<std::string>& args_)
{
  std::vector<std::string> local_args = args_;

  if(local_args.size() > 5)
  {
    PRINT_USAGE(USAGE_EXPORT_TRANSFERS);
    return true;
  }

  LOCK_IDLE_SCOPE();
  
  std::vector<transfer_view> all_transfers;

  // might consumes arguments in local_args
  if (!get_transfers(local_args, all_transfers))
    return true;

  // output filename
  std::string filename = (boost::format("output%u.csv") % m_current_subaddress_account).str();
  if (local_args.size() > 0 && local_args[0].substr(0, 7) == "output=")
  {
    filename = local_args[0].substr(7, -1);
    local_args.erase(local_args.begin());
  }

  std::ofstream file(filename);

  // header
  file <<
      boost::format("%8.8s,%9.9s,%8.8s,%16.16s,%20.20s,%20.20s,%64.64s,%16.16s,%14.14s,%100.100s,%20.20s,%s,%s") %
      tr("block") % tr("type") % tr("lock") % tr("timestamp") % tr("amount") % tr("running balance") % tr("hash") % tr("payment ID") % tr("fee") % tr("destination") % tr("amount") % tr("index") % tr("note")
      << std::endl;

  uint64_t running_balance = 0;
  auto formatter = boost::format("%8.8llu,%9.9s,%8.8s,%16.16s,%20.20s,%20.20s,%64.64s,%16.16s,%14.14s,%100.100s,%20.20s,\"%s\",%s");

  for (const auto& transfer : all_transfers)
  {
    // ignore unconfirmed transfers in running balance
    if (transfer.confirmed)
    {
      switch (transfer.type)
      {
        case tools::pay_type::in:
        case tools::pay_type::miner:
        case tools::pay_type::full_node:
        case tools::pay_type::control:
          running_balance += transfer.amount;
          break;
        case tools::pay_type::stake:
          running_balance -= transfer.fee;
          break;
        case tools::pay_type::out:
          running_balance -= transfer.amount + transfer.fee;
          break;
        default:
          fail_msg_writer() << tr("Warning: Unhandled pay type, this is most likely a developer error, please report it to the Antd developers.");
          break;
      }
    }

    char const UNLOCKED[] = "unlocked";
    char const LOCKED[]   = "locked";
    char const *lock_str = (transfer.unlocked) ? UNLOCKED : LOCKED;

    file << formatter
      % transfer.block
      % tools::pay_type_string(transfer.type)
      % lock_str
      % tools::get_human_readable_timestamp(transfer.timestamp)
      % print_money(transfer.amount)
      % print_money(running_balance)
      % string_tools::pod_to_hex(transfer.hash)
      % transfer.payment_id
      % print_money(transfer.fee)
      % (transfer.outputs.size() ? transfer.outputs[0].wallet_addr : "-")
      % (transfer.outputs.size() ? print_money(transfer.outputs[0].amount) : "")
      % boost::algorithm::join(transfer.index | boost::adaptors::transformed([](uint32_t i) { return std::to_string(i); }), ", ")
      % transfer.note
      << std::endl;

    for (size_t i = 1; i < transfer.outputs.size(); ++i)
    {
      file << formatter
        % ""
        % ""
        % ""
        % ""
        % ""
        % ""
        % ""
        % ""
        % transfer.outputs[i].wallet_addr
        % print_money(transfer.outputs[i].amount)
        % ""
        % ""
        << std::endl;
    }
  }
  file.close();

  success_msg_writer() << tr("CSV exported to ") << filename;

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::unspent_outputs(const std::vector<std::string> &args_)
{
  if(args_.size() > 3)
  {
    PRINT_USAGE(USAGE_UNSPENT_OUTPUTS);
    return true;
  }
  auto local_args = args_;

  std::set<uint32_t> subaddr_indices;
  if (local_args.size() > 0 && local_args[0].substr(0, 6) == "index=")
  {
    std::string parse_subaddr_err;
    if (!tools::parse_subaddress_indices(local_args[0], subaddr_indices, &parse_subaddr_err))
    {
      fail_msg_writer() << parse_subaddr_err;
      return true;
    }
    local_args.erase(local_args.begin());
  }

  uint64_t min_amount = 0;
  uint64_t max_amount = std::numeric_limits<uint64_t>::max();
  if (local_args.size() > 0)
  {
    if (!cryptonote::parse_amount(min_amount, local_args[0]))
    {
      fail_msg_writer() << tr("amount is wrong: ") << local_args[0];
      return true;
    }
    local_args.erase(local_args.begin());
    if (local_args.size() > 0)
    {
      if (!cryptonote::parse_amount(max_amount, local_args[0]))
      {
        fail_msg_writer() << tr("amount is wrong: ") << local_args[0];
        return true;
      }
      local_args.erase(local_args.begin());
    }
    if (min_amount > max_amount)
    {
      fail_msg_writer() << tr("<min_amount> should be smaller than <max_amount>");
      return true;
    }
  }
  tools::wallet2::transfer_container transfers;
  m_wallet->get_transfers(transfers);
  std::map<uint64_t, tools::wallet2::transfer_container> amount_to_tds;
  uint64_t min_height = std::numeric_limits<uint64_t>::max();
  uint64_t max_height = 0;
  uint64_t found_min_amount = std::numeric_limits<uint64_t>::max();
  uint64_t found_max_amount = 0;
  uint64_t count = 0;
  for (const auto& td : transfers)
  {
    uint64_t amount = td.amount();
    if (td.m_spent || amount < min_amount || amount > max_amount || td.m_subaddr_index.major != m_current_subaddress_account || (subaddr_indices.count(td.m_subaddr_index.minor) == 0 && !subaddr_indices.empty()))
      continue;
    amount_to_tds[amount].push_back(td);
    if (min_height > td.m_block_height) min_height = td.m_block_height;
    if (max_height < td.m_block_height) max_height = td.m_block_height;
    if (found_min_amount > amount) found_min_amount = amount;
    if (found_max_amount < amount) found_max_amount = amount;
    ++count;
  }
  if (amount_to_tds.empty())
  {
    success_msg_writer() << tr("There is no unspent output in the specified address");
    return true;
  }
  for (const auto& amount_tds : amount_to_tds)
  {
    auto& tds = amount_tds.second;
    success_msg_writer() << tr("\nAmount: ") << print_money(amount_tds.first) << tr(", number of keys: ") << tds.size();
    for (size_t i = 0; i < tds.size(); )
    {
      std::ostringstream oss;
      for (size_t j = 0; j < 8 && i < tds.size(); ++i, ++j)
        oss << tds[i].m_block_height << tr(" ");
      success_msg_writer() << oss.str();
    }
  }
  success_msg_writer()
    << tr("\nMin block height: ") << min_height
    << tr("\nMax block height: ") << max_height
    << tr("\nMin amount found: ") << print_money(found_min_amount)
    << tr("\nMax amount found: ") << print_money(found_max_amount)
    << tr("\nTotal count: ") << count;
  const size_t histogram_height = 10;
  const size_t histogram_width  = 50;
  double bin_size = (max_height - min_height + 1.0) / histogram_width;
  size_t max_bin_count = 0;
  std::vector<size_t> histogram(histogram_width, 0);
  for (const auto& amount_tds : amount_to_tds)
  {
    for (auto& td : amount_tds.second)
    {
      uint64_t bin_index = (td.m_block_height - min_height + 1) / bin_size;
      if (bin_index >= histogram_width)
        bin_index = histogram_width - 1;
      histogram[bin_index]++;
      if (max_bin_count < histogram[bin_index])
        max_bin_count = histogram[bin_index];
    }
  }
  for (size_t x = 0; x < histogram_width; ++x)
  {
    double bin_count = histogram[x];
    if (max_bin_count > histogram_height)
      bin_count *= histogram_height / (double)max_bin_count;
    if (histogram[x] > 0 && bin_count < 1.0)
      bin_count = 1.0;
    histogram[x] = bin_count;
  }
  std::vector<std::string> histogram_line(histogram_height, std::string(histogram_width, ' '));
  for (size_t y = 0; y < histogram_height; ++y)
  {
    for (size_t x = 0; x < histogram_width; ++x)
    {
      if (y < histogram[x])
        histogram_line[y][x] = '*';
    }
  }
  double count_per_star = max_bin_count / (double)histogram_height;
  if (count_per_star < 1)
    count_per_star = 1;
  success_msg_writer()
    << tr("\nBin size: ") << bin_size
    << tr("\nOutputs per *: ") << count_per_star;
  ostringstream histogram_str;
  histogram_str << tr("count\n  ^\n");
  for (size_t y = histogram_height; y > 0; --y)
    histogram_str << tr("  |") << histogram_line[y - 1] << tr("|\n");
  histogram_str
    << tr("  +") << std::string(histogram_width, '-') << tr("+--> block height\n")
    << tr("   ^") << std::string(histogram_width - 2, ' ') << tr("^\n")
    << tr("  ") << min_height << std::string(histogram_width - 8, ' ') << max_height;
  success_msg_writer() << histogram_str.str();
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::rescan_blockchain(const std::vector<std::string> &args_)
{
  bool hard = false;
  if (!args_.empty())
  {
    if (args_[0] != "hard")
    {
      PRINT_USAGE(USAGE_RESCAN_BC);
      return true;
    }
    hard = true;
  }

  if (hard)
  {
    message_writer() << tr("Warning: this will lose any information which can not be recovered from the blockchain.");
    message_writer() << tr("This includes destination addresses, tx secret keys, tx notes, etc");
    std::string confirm = input_line(tr("Rescan anyway?"), true);
    if(!std::cin.eof())
    {
      if (!command_line::is_yes(confirm))
        return true;
    }
  }
  return refresh_main(0, hard ? ResetHard : ResetSoft, true);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::check_for_messages()
{
  try
  {
    std::vector<mms::message> new_messages;
    bool new_message = get_message_store().check_for_messages(get_multisig_wallet_state(), new_messages);
    if (new_message)
    {
      message_writer(console_color_magenta, true) << tr("MMS received new message");
      list_mms_messages(new_messages);
      m_cmd_binder.print_prompt();
    }
  }
  catch(...) {}
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::wallet_idle_thread()
{
  while (true)
  {
    boost::unique_lock<boost::mutex> lock(m_idle_mutex);
    if (!m_idle_run.load(std::memory_order_relaxed))
      break;

    // auto refresh
    if (m_auto_refresh_enabled)
    {
      m_auto_refresh_refreshing = true;
      try
      {
        uint64_t fetched_blocks;
        bool received_money;
        if (try_connect_to_daemon(true))
          m_wallet->refresh(m_wallet->is_trusted_daemon(), 0, fetched_blocks, received_money, false); // don't check the pool in background mode
      }
      catch(...) {}
      m_auto_refresh_refreshing = false;
    }

    // Check for new MMS messages;
    // For simplicity auto message check is ALSO controlled by "m_auto_refresh_enabled" and has no
    // separate thread either; thread syncing is tricky enough with only this one idle thread here
    if (m_auto_refresh_enabled && get_message_store().get_active())
    {
      check_for_messages();
    }

    if (!m_idle_run.load(std::memory_order_relaxed))
      break;
    m_idle_cond.wait_for(lock, boost::chrono::seconds(90));
  }
}
//----------------------------------------------------------------------------------------------------
std::string simple_wallet::get_prompt() const
{
  std::string addr_start = m_wallet->get_subaddress_as_str({m_current_subaddress_account, 0}).substr(0, 6);
  std::string prompt = std::string("[") + tr("wallet") + " " + addr_start;
  if (!m_wallet->check_connection(NULL))
    prompt += tr(" (no daemon)");
  else
  {
    if (m_wallet->is_synced())
    {
      if (m_has_locked_key_images)
      {
        prompt += tr(" (has locked stakes)");
      }
    }
    else
    {
      prompt += tr(" (out of sync)");
    }
  }
  prompt += "]: ";

  return prompt;
}
//----------------------------------------------------------------------------------------------------

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
#include <thread>
#endif

bool simple_wallet::run()
{
  // check and display warning, but go on anyway
  try_connect_to_daemon();

  refresh_main(0, ResetNone, true);

  m_auto_refresh_enabled = m_wallet->auto_refresh();
  m_idle_thread = boost::thread([&]{wallet_idle_thread();});

  message_writer(console_color_green, false) << "Background refresh thread started";

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
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

    this->process_command(args);
    if (args.size() == 1 && args[0] == "exit")
    {
      antd::deinit_integration_test_context();
      return true;
    }

    antd::write_redirected_stdout_to_shared_mem();
  }
#endif

  return m_cmd_binder.run_handling([this]() {return get_prompt(); }, "");
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::stop()
{
  m_cmd_binder.stop_handling();
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::account(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  // Usage:
  //   account
  //   account new <label text with white spaces allowed>
  //   account switch <index>
  //   account label <index> <label text with white spaces allowed>
  //   account tag <tag_name> <account_index_1> [<account_index_2> ...]
  //   account untag <account_index_1> [<account_index_2> ...]
  //   account tag_description <tag_name> <description>

  if (args.empty())
  {
    // print all the existing accounts
    LOCK_IDLE_SCOPE();
    print_accounts();
    return true;
  }

  std::vector<std::string> local_args = args;
  std::string command = local_args[0];
  local_args.erase(local_args.begin());
  if (command == "new")
  {
    // create a new account and switch to it
    std::string label = boost::join(local_args, " ");
    if (label.empty())
      label = tr("(Untitled account)");
    m_wallet->add_subaddress_account(label);
    m_current_subaddress_account = m_wallet->get_num_subaddress_accounts() - 1;
    // update_prompt();
    LOCK_IDLE_SCOPE();
    print_accounts();
  }
  else if (command == "switch" && local_args.size() == 1)
  {
    // switch to the specified account
    uint32_t index_major;
    if (!epee::string_tools::get_xtype_from_string(index_major, local_args[0]))
    {
      fail_msg_writer() << tr("failed to parse index: ") << local_args[0];
      return true;
    }
    if (index_major >= m_wallet->get_num_subaddress_accounts())
    {
      fail_msg_writer() << tr("specify an index between 0 and ") << (m_wallet->get_num_subaddress_accounts() - 1);
      return true;
    }
    m_current_subaddress_account = index_major;
    // update_prompt();
    show_balance();
  }
  else if (command == "label" && local_args.size() >= 1)
  {
    // set label of the specified account
    uint32_t index_major;
    if (!epee::string_tools::get_xtype_from_string(index_major, local_args[0]))
    {
      fail_msg_writer() << tr("failed to parse index: ") << local_args[0];
      return true;
    }
    local_args.erase(local_args.begin());
    std::string label = boost::join(local_args, " ");
    try
    {
      m_wallet->set_subaddress_label({index_major, 0}, label);
      LOCK_IDLE_SCOPE();
      print_accounts();
    }
    catch (const std::exception& e)
    {
      fail_msg_writer() << e.what();
    }
  }
  else if (command == "tag" && local_args.size() >= 2)
  {
    const std::string tag = local_args[0];
    std::set<uint32_t> account_indices;
    for (size_t i = 1; i < local_args.size(); ++i)
    {
      uint32_t account_index;
      if (!epee::string_tools::get_xtype_from_string(account_index, local_args[i]))
      {
        fail_msg_writer() << tr("failed to parse index: ") << local_args[i];
        return true;
      }
      account_indices.insert(account_index);
    }
    try
    {
      m_wallet->set_account_tag(account_indices, tag);
      print_accounts(tag);
    }
    catch (const std::exception& e)
    {
      fail_msg_writer() << e.what();
    }
  }
  else if (command == "untag" && local_args.size() >= 1)
  {
    std::set<uint32_t> account_indices;
    for (size_t i = 0; i < local_args.size(); ++i)
    {
      uint32_t account_index;
      if (!epee::string_tools::get_xtype_from_string(account_index, local_args[i]))
      {
        fail_msg_writer() << tr("failed to parse index: ") << local_args[i];
        return true;
      }
      account_indices.insert(account_index);
    }
    try
    {
      m_wallet->set_account_tag(account_indices, "");
      print_accounts();
    }
    catch (const std::exception& e)
    {
      fail_msg_writer() << e.what();
    }
  }
  else if (command == "tag_description" && local_args.size() >= 1)
  {
    const std::string tag = local_args[0];
    std::string description;
    if (local_args.size() > 1)
    {
      local_args.erase(local_args.begin());
      description = boost::join(local_args, " ");
    }
    try
    {
      m_wallet->set_account_tag_description(tag, description);
      print_accounts(tag);
    }
    catch (const std::exception& e)
    {
      fail_msg_writer() << e.what();
    }
  }
  else
  {
    PRINT_USAGE(USAGE_ACCOUNT);
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::print_accounts()
{
  const std::pair<std::map<std::string, std::string>, std::vector<std::string>>& account_tags = m_wallet->get_account_tags();
  size_t num_untagged_accounts = m_wallet->get_num_subaddress_accounts();
  for (const std::pair<std::string, std::string>& p : account_tags.first)
  {
    const std::string& tag = p.first;
    print_accounts(tag);
    num_untagged_accounts -= std::count(account_tags.second.begin(), account_tags.second.end(), tag);
    success_msg_writer() << "";
  }

  if (num_untagged_accounts > 0)
    print_accounts("");

  if (num_untagged_accounts < m_wallet->get_num_subaddress_accounts())
    success_msg_writer() << tr("\nGrand total:\n  Balance: ") << print_money(m_wallet->balance_all()) << tr(", unlocked balance: ") << print_money(m_wallet->unlocked_balance_all());
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::print_accounts(const std::string& tag)
{
  const std::pair<std::map<std::string, std::string>, std::vector<std::string>>& account_tags = m_wallet->get_account_tags();
  if (tag.empty())
  {
    success_msg_writer() << tr("Untagged accounts:");
  }
  else
  {
    if (account_tags.first.count(tag) == 0)
    {
      fail_msg_writer() << boost::format(tr("Tag %s is unregistered.")) % tag;
      return;
    }
    success_msg_writer() << tr("Accounts with tag: ") << tag;
    success_msg_writer() << tr("Tag's description: ") << account_tags.first.find(tag)->second;
  }
  success_msg_writer() << boost::format("  %15s %21s %21s %21s") % tr("Account") % tr("Balance") % tr("Unlocked balance") % tr("Label");
  uint64_t total_balance = 0, total_unlocked_balance = 0;
  for (uint32_t account_index = 0; account_index < m_wallet->get_num_subaddress_accounts(); ++account_index)
  {
    if (account_tags.second[account_index] != tag)
      continue;
    success_msg_writer() << boost::format(tr(" %c%8u %6s %21s %21s %21s"))
      % (m_current_subaddress_account == account_index ? '*' : ' ')
      % account_index
      % m_wallet->get_subaddress_as_str({account_index, 0}).substr(0, 6)
      % print_money(m_wallet->balance(account_index))
      % print_money(m_wallet->unlocked_balance(account_index))
      % m_wallet->get_subaddress_label({account_index, 0});
    total_balance += m_wallet->balance(account_index);
    total_unlocked_balance += m_wallet->unlocked_balance(account_index);
  }
  success_msg_writer() << tr("----------------------------------------------------------------------------------");
  success_msg_writer() << boost::format(tr("%15s %21s %21s")) % "Total" % print_money(total_balance) % print_money(total_unlocked_balance);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_address(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  // Usage:
  //  address
  //  address new <label text with white spaces allowed>
  //  address all
  //  address <index_min> [<index_max>]
  //  address label <index> <label text with white spaces allowed>

  std::vector<std::string> local_args = args;
  tools::wallet2::transfer_container transfers;
  m_wallet->get_transfers(transfers);

  auto print_address_sub = [this, &transfers](uint32_t index)
  {
    bool used = std::find_if(
      transfers.begin(), transfers.end(),
      [this, &index](const tools::wallet2::transfer_details& td) {
        return td.m_subaddr_index == cryptonote::subaddress_index{ m_current_subaddress_account, index };
      }) != transfers.end();
    success_msg_writer() << index << "  " << m_wallet->get_subaddress_as_str({m_current_subaddress_account, index}) << "  " << (index == 0 ? tr("Primary address") : m_wallet->get_subaddress_label({m_current_subaddress_account, index})) << " " << (used ? tr("(used)") : "");
  };

  uint32_t index = 0;
  if (local_args.empty())
  {
    print_address_sub(index);
  }
  else if (local_args.size() == 1 && local_args[0] == "all")
  {
    local_args.erase(local_args.begin());
    for (; index < m_wallet->get_num_subaddresses(m_current_subaddress_account); ++index)
      print_address_sub(index);
  }
  else if (local_args[0] == "new")
  {
    local_args.erase(local_args.begin());
    std::string label;
    if (local_args.size() > 0)
      label = boost::join(local_args, " ");
    if (label.empty())
      label = tr("(Untitled address)");
    m_wallet->add_subaddress(m_current_subaddress_account, label);
    print_address_sub(m_wallet->get_num_subaddresses(m_current_subaddress_account) - 1);
  }
  else if (local_args.size() >= 2 && local_args[0] == "label")
  {
    if (!epee::string_tools::get_xtype_from_string(index, local_args[1]))
    {
      fail_msg_writer() << tr("failed to parse index: ") << local_args[1];
      return true;
    }
    if (index >= m_wallet->get_num_subaddresses(m_current_subaddress_account))
    {
      fail_msg_writer() << tr("specify an index between 0 and ") << (m_wallet->get_num_subaddresses(m_current_subaddress_account) - 1);
      return true;
    }
    local_args.erase(local_args.begin());
    local_args.erase(local_args.begin());
    std::string label = boost::join(local_args, " ");
    m_wallet->set_subaddress_label({m_current_subaddress_account, index}, label);
    print_address_sub(index);
  }
  else if (local_args.size() <= 2 && epee::string_tools::get_xtype_from_string(index, local_args[0]))
  {
    local_args.erase(local_args.begin());
    uint32_t index_min = index;
    uint32_t index_max = index_min;
    if (local_args.size() > 0)
    {
      if (!epee::string_tools::get_xtype_from_string(index_max, local_args[0]))
      {
        fail_msg_writer() << tr("failed to parse index: ") << local_args[0];
        return true;
      }
      local_args.erase(local_args.begin());
    }
    if (index_max < index_min)
      std::swap(index_min, index_max);
    if (index_min >= m_wallet->get_num_subaddresses(m_current_subaddress_account))
    {
      fail_msg_writer() << tr("<index_min> is already out of bound");
      return true;
    }
    if (index_max >= m_wallet->get_num_subaddresses(m_current_subaddress_account))
    {
      message_writer() << tr("<index_max> exceeds the bound");
      index_max = m_wallet->get_num_subaddresses(m_current_subaddress_account) - 1;
    }
    for (index = index_min; index <= index_max; ++index)
      print_address_sub(index);
  }
  else
  {
    PRINT_USAGE(USAGE_ADDRESS);
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_integrated_address(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  crypto::hash8 payment_id;
  if (args.size() > 1)
  {
    PRINT_USAGE(USAGE_INTEGRATED_ADDRESS);
    return true;
  }
  if (args.size() == 0)
  {
    if (m_current_subaddress_account != 0)
    {
      fail_msg_writer() << tr("Integrated addresses can only be created for account 0");
      return true;
    }
    payment_id = crypto::rand<crypto::hash8>();
    success_msg_writer() << tr("Random payment ID: ") << payment_id;
    success_msg_writer() << tr("Matching integrated address: ") << m_wallet->get_account().get_public_integrated_address_str(payment_id, m_wallet->nettype());
    return true;
  }
  if(tools::wallet2::parse_short_payment_id(args.back(), payment_id))
  {
    if (m_current_subaddress_account != 0)
    {
      fail_msg_writer() << tr("Integrated addresses can only be created for account 0");
      return true;
    }
    success_msg_writer() << m_wallet->get_account().get_public_integrated_address_str(payment_id, m_wallet->nettype());
    return true;
  }
  else {
    address_parse_info info;
    if(get_account_address_from_str(info, m_wallet->nettype(), args.back()))
    {
      if (info.has_payment_id)
      {
        success_msg_writer() << boost::format(tr("Integrated address: %s, payment ID: %s")) %
          get_account_address_as_str(m_wallet->nettype(), false, info.address) % epee::string_tools::pod_to_hex(info.payment_id);
      }
      else
      {
        success_msg_writer() << (info.is_subaddress ? tr("Subaddress: ") : tr("Standard address: ")) << get_account_address_as_str(m_wallet->nettype(), info.is_subaddress, info.address);
      }
      return true;
    }
  }
  fail_msg_writer() << tr("failed to parse payment ID or address");
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::address_book(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  if (args.size() == 0)
  {
  }
  else if (args.size() == 1 || (args[0] != "add" && args[0] != "delete"))
  {
    PRINT_USAGE(USAGE_ADDRESS_BOOK);
    return true;
  }
  else if (args[0] == "add")
  {
    cryptonote::address_parse_info info;
    if(!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), args[1], oa_prompter))
    {
      fail_msg_writer() << tr("failed to parse address");
      return true;
    }
    crypto::hash payment_id = crypto::null_hash;
    size_t description_start = 2;
    if (info.has_payment_id)
    {
      memcpy(payment_id.data, info.payment_id.data, 8);
    }
    else if (!info.has_payment_id && args.size() >= 4 && args[2] == "pid")
    {
      if (tools::wallet2::parse_long_payment_id(args[3], payment_id))
      {
        LONG_PAYMENT_ID_SUPPORT_CHECK();
        description_start += 2;
      }
      else if (tools::wallet2::parse_short_payment_id(args[3], info.payment_id))
      {
        fail_msg_writer() << tr("Short payment IDs are to be used within an integrated address only");
        return true;
      }
      else
      {
        fail_msg_writer() << tr("failed to parse payment ID");
        return true;
      }
    }
    std::string description;
    for (size_t i = description_start; i < args.size(); ++i)
    {
      if (i > description_start)
        description += " ";
      description += args[i];
    }
    m_wallet->add_address_book_row(info.address, payment_id, description, info.is_subaddress);
  }
  else
  {
    size_t row_id;
    if(!epee::string_tools::get_xtype_from_string(row_id, args[1]))
    {
      fail_msg_writer() << tr("failed to parse index");
      return true;
    }
    m_wallet->delete_address_book_row(row_id);
  }
  auto address_book = m_wallet->get_address_book();
  if (address_book.empty())
  {
    success_msg_writer() << tr("Address book is empty.");
  }
  else
  {
    for (size_t i = 0; i < address_book.size(); ++i) {
      auto& row = address_book[i];
      success_msg_writer() << tr("Index: ") << i;
      success_msg_writer() << tr("Address: ") << get_account_address_as_str(m_wallet->nettype(), row.m_is_subaddress, row.m_address);
      success_msg_writer() << tr("Payment ID: ") << row.m_payment_id << " (OBSOLETE)";
      success_msg_writer() << tr("Description: ") << row.m_description << "\n";
    }
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_tx_note(const std::vector<std::string> &args)
{
  if (args.size() == 0)
  {
    PRINT_USAGE(USAGE_SET_TX_NOTE);
    return true;
  }

  cryptonote::blobdata txid_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(args.front(), txid_data) || txid_data.size() != sizeof(crypto::hash))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }
  crypto::hash txid = *reinterpret_cast<const crypto::hash*>(txid_data.data());

  std::string note = "";
  for (size_t n = 1; n < args.size(); ++n)
  {
    if (n > 1)
      note += " ";
    note += args[n];
  }
  m_wallet->set_tx_note(txid, note);

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_tx_note(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_GET_TX_NOTE);
    return true;
  }

  cryptonote::blobdata txid_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(args.front(), txid_data) || txid_data.size() != sizeof(crypto::hash))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }
  crypto::hash txid = *reinterpret_cast<const crypto::hash*>(txid_data.data());

  std::string note = m_wallet->get_tx_note(txid);
  if (note.empty())
    success_msg_writer() << "no note found";
  else
    success_msg_writer() << "note found: " << note;

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_description(const std::vector<std::string> &args)
{
  // 0 arguments allowed, for setting the description to empty string

  std::string description = "";
  for (size_t n = 0; n < args.size(); ++n)
  {
    if (n > 0)
      description += " ";
    description += args[n];
  }
  m_wallet->set_description(description);

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::get_description(const std::vector<std::string> &args)
{
  if (args.size() != 0)
  {
    PRINT_USAGE(USAGE_GET_DESCRIPTION);
    return true;
  }

  std::string description = m_wallet->get_description();
  if (description.empty())
    success_msg_writer() << tr("no description found");
  else
    success_msg_writer() << tr("description found: ") << description;

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::status(const std::vector<std::string> &args)
{
  uint64_t local_height = m_wallet->get_blockchain_current_height();
  uint32_t version = 0;
  if (!m_wallet->check_connection(&version))
  {
    success_msg_writer() << "Refreshed " << local_height << "/?, no daemon connected";
    return true;
  }

  std::string err;
  uint64_t bc_height = get_daemon_blockchain_height(err);
  if (err.empty())
  {
    bool synced = local_height == bc_height;
    success_msg_writer() << "Refreshed " << local_height << "/" << bc_height << ", " << (synced ? "synced" : "syncing")
        << ", daemon RPC v" << get_version_string(version);
  }
  else
  {
    fail_msg_writer() << "Refreshed " << local_height << "/?, daemon connection error";
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::wallet_info(const std::vector<std::string> &args)
{
  bool ready;
  uint32_t threshold, total;
  std::string description = m_wallet->get_description();
  if (description.empty())
  {
    description = "<Not set>"; 
  }
  message_writer() << tr("Filename: ") << m_wallet->get_wallet_file();
  message_writer() << tr("Description: ") << description;
  message_writer() << tr("Address: ") << m_wallet->get_account().get_public_address_str(m_wallet->nettype());
  std::string type;
  if (m_wallet->watch_only())
    type = tr("Watch only");
  else if (m_wallet->multisig(&ready, &threshold, &total))
    type = (boost::format(tr("%u/%u multisig%s")) % threshold % total % (ready ? "" : " (not yet finalized)")).str();
  else
    type = tr("Normal");
  message_writer() << tr("Type: ") << type;
  message_writer() << tr("Network type: ") << (
    m_wallet->nettype() == cryptonote::TESTNET ? tr("Testnet") :
    m_wallet->nettype() == cryptonote::STAGENET ? tr("Stagenet") : tr("Mainnet"));
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::sign(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_SIGN);
    return true;
  }
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and cannot sign");
    return true;
  }
  if (m_wallet->multisig())
  {
    fail_msg_writer() << tr("This wallet is multisig and cannot sign");
    return true;
  }

  std::string filename = args[0];
  std::string data;
  bool r = epee::file_io_utils::load_file_to_string(filename, data);
  if (!r)
  {
    fail_msg_writer() << tr("failed to read file ") << filename;
    return true;
  }

  SCOPED_WALLET_UNLOCK();

  std::string signature = m_wallet->sign(data);
  success_msg_writer() << signature;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::verify(const std::vector<std::string> &args)
{
  if (args.size() != 3)
  {
    PRINT_USAGE(USAGE_VERIFY);
    return true;
  }
  std::string filename = args[0];
  std::string address_string = args[1];
  std::string signature= args[2];

  std::string data;
  bool r = epee::file_io_utils::load_file_to_string(filename, data);
  if (!r)
  {
    fail_msg_writer() << tr("failed to read file ") << filename;
    return true;
  }

  cryptonote::address_parse_info info;
  if(!cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), address_string, oa_prompter))
  {
    fail_msg_writer() << tr("failed to parse address");
    return true;
  }

  r = m_wallet->verify(data, info.address, signature);
  if (!r)
  {
    fail_msg_writer() << tr("Bad signature from ") << address_string;
  }
  else
  {
    success_msg_writer() << tr("Good signature from ") << address_string;
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::export_key_images(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (args.size() != 1 && args.size() != 2)
  {
    PRINT_USAGE(USAGE_EXPORT_KEY_IMAGES);
    return true;
  }
  if (m_wallet->watch_only())
  {
    fail_msg_writer() << tr("wallet is watch-only and cannot export key images");
    return true;
  }

  std::string filename = args[0];
  if (m_wallet->confirm_export_overwrite() && !check_file_overwrite(filename))
    return true;

  SCOPED_WALLET_UNLOCK();

  try
  {
    /// whether to export requested key images only
    bool requested_only = (args.size() == 2 && args[1] == "requested-only");
    if (!m_wallet->export_key_images(filename, requested_only))
    {
      fail_msg_writer() << tr("failed to save file ") << filename;
      return true;
    }
  }
  catch (const std::exception &e)
  {
    LOG_ERROR("Error exporting key images: " << e.what());
    fail_msg_writer() << "Error exporting key images: " << e.what();
    return true;
  }

  success_msg_writer() << tr("Signed key images exported to ") << filename;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::import_key_images(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (!m_wallet->is_trusted_daemon())
  {
    fail_msg_writer() << tr("this command requires a trusted daemon. Enable with --trusted-daemon");
    return true;
  }

  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_IMPORT_KEY_IMAGES);
    return true;
  }
  std::string filename = args[0];

  LOCK_IDLE_SCOPE();
  try
  {
    uint64_t spent = 0, unspent = 0;
    uint64_t height = m_wallet->import_key_images(filename, spent, unspent);
    if (height > 0)
    {
      success_msg_writer() << "Signed key images imported to height " << height << ", "
          << print_money(spent) << " spent, " << print_money(unspent) << " unspent"; 
    } else {
      fail_msg_writer() << "Failed to import key images";
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << "Failed to import key images: " << e.what();
    return true;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::hw_key_images_sync(const std::vector<std::string> &args)
{
  if (!m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command only supported by HW wallet");
    return true;
  }
  if (!m_wallet->get_account().get_device().has_ki_cold_sync())
  {
    fail_msg_writer() << tr("hw wallet does not support cold KI sync");
    return true;
  }

  LOCK_IDLE_SCOPE();
  key_images_sync_intern();
  return true;
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::key_images_sync_intern(){
  try
  {
    message_writer(console_color_white, false) << tr("Please confirm the key image sync on the device");

    uint64_t spent = 0, unspent = 0;
    uint64_t height = m_wallet->cold_key_image_sync(spent, unspent);
    if (height > 0)
    {
      success_msg_writer() << tr("Key images synchronized to height ") << height;
      if (!m_wallet->is_trusted_daemon())
      {
        message_writer() << tr("Running untrusted daemon, cannot determine which transaction output is spent. Use a trusted daemon with --trusted-daemon and run rescan_spent");
      } else
      {
        success_msg_writer() << print_money(spent) << tr(" spent, ") << print_money(unspent) << tr(" unspent");
      }
    }
    else {
      fail_msg_writer() << tr("Failed to import key images");
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to import key images: ") << e.what();
  }
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::hw_reconnect(const std::vector<std::string> &args)
{
  if (!m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command only supported by HW wallet");
    return true;
  }

  LOCK_IDLE_SCOPE();
  try
  {
    bool r = m_wallet->reconnect_device();
    if (!r){
      fail_msg_writer() << tr("Failed to reconnect device");
    }
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Failed to reconnect device: ") << tr(e.what());
    return true;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::export_outputs(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_EXPORT_OUTPUTS);
    return true;
  }

  std::string filename = args[0];
  if (m_wallet->confirm_export_overwrite() && !check_file_overwrite(filename))
    return true;

  SCOPED_WALLET_UNLOCK();

  try
  {
    std::string data = m_wallet->export_outputs_to_str();
    bool r = epee::file_io_utils::save_string_to_file(filename, data);
    if (!r)
    {
      fail_msg_writer() << tr("failed to save file ") << filename;
      return true;
    }
  }
  catch (const std::exception &e)
  {
    LOG_ERROR("Error exporting outputs: " << e.what());
    fail_msg_writer() << "Error exporting outputs: " << e.what();
    return true;
  }

  success_msg_writer() << tr("Outputs exported to ") << filename;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::import_outputs(const std::vector<std::string> &args)
{
  if (m_wallet->key_on_device())
  {
    fail_msg_writer() << tr("command not supported by HW wallet");
    return true;
  }
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_IMPORT_OUTPUTS);
    return true;
  }
  std::string filename = args[0];

  std::string data;
  bool r = epee::file_io_utils::load_file_to_string(filename, data);
  if (!r)
  {
    fail_msg_writer() << tr("failed to read file ") << filename;
    return true;
  }

  try
  {
    SCOPED_WALLET_UNLOCK();
    size_t n_outputs = m_wallet->import_outputs_from_str(data);
    success_msg_writer() << boost::lexical_cast<std::string>(n_outputs) << " outputs imported";
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << "Failed to import outputs " << filename << ": " << e.what();
    return true;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_transfer(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    PRINT_USAGE(USAGE_SHOW_TRANSFER);
    return true;
  }

  cryptonote::blobdata txid_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(args.front(), txid_data) || txid_data.size() != sizeof(crypto::hash))
  {
    fail_msg_writer() << tr("failed to parse txid");
    return true;
  }
  crypto::hash txid = *reinterpret_cast<const crypto::hash*>(txid_data.data());

  const uint64_t last_block_height = m_wallet->get_blockchain_current_height();

  std::list<std::pair<crypto::hash, tools::wallet2::payment_details>> payments;
  m_wallet->get_payments(payments, 0, (uint64_t)-1, m_current_subaddress_account);
  for (std::list<std::pair<crypto::hash, tools::wallet2::payment_details>>::const_iterator i = payments.begin(); i != payments.end(); ++i) {
    const tools::wallet2::payment_details &pd = i->second;
    if (pd.m_tx_hash == txid) {
      std::string payment_id = string_tools::pod_to_hex(i->first);
      if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
        payment_id = payment_id.substr(0,16);
      success_msg_writer() << "Incoming transaction found";
      success_msg_writer() << "txid: " << txid;
      success_msg_writer() << "Height: " << pd.m_block_height;
      success_msg_writer() << "Timestamp: " << tools::get_human_readable_timestamp(pd.m_timestamp);
      success_msg_writer() << "Amount: " << print_money(pd.m_amount);
      success_msg_writer() << "Payment ID: " << payment_id;
      if (pd.m_unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER)
      {
        uint64_t bh = std::max(pd.m_unlock_time, pd.m_block_height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE);
        uint64_t last_block_reward = m_wallet->get_last_block_reward();
        uint64_t suggested_threshold = last_block_reward ? (pd.m_amount + last_block_reward - 1) / last_block_reward : 0;
        if (bh >= last_block_height)
          success_msg_writer() << "Locked: " << (bh - last_block_height) << " blocks to unlock";
        else if (suggested_threshold > 0)
          success_msg_writer() << std::to_string(last_block_height - bh) << " confirmations (" << suggested_threshold << " suggested threshold)";
        else
          success_msg_writer() << std::to_string(last_block_height - bh) << " confirmations";
      }
      else
      {
        uint64_t current_time = static_cast<uint64_t>(time(NULL));
        uint64_t threshold = current_time + CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_SECONDS_V2;
        if (threshold >= pd.m_unlock_time)
          success_msg_writer() << "unlocked for " << tools::get_human_readable_timespan(std::chrono::seconds(threshold - pd.m_unlock_time));
        else
          success_msg_writer() << "locked for " << tools::get_human_readable_timespan(std::chrono::seconds(pd.m_unlock_time - threshold));
      }
      success_msg_writer() << "Address index: " << pd.m_subaddr_index.minor;
      success_msg_writer() << "Note: " << m_wallet->get_tx_note(txid);
      return true;
    }
  }

  std::list<std::pair<crypto::hash, tools::wallet2::confirmed_transfer_details>> payments_out;
  m_wallet->get_payments_out(payments_out, 0, (uint64_t)-1, m_current_subaddress_account);
  for (std::list<std::pair<crypto::hash, tools::wallet2::confirmed_transfer_details>>::const_iterator i = payments_out.begin(); i != payments_out.end(); ++i) {
    if (i->first == txid)
    {
      const tools::wallet2::confirmed_transfer_details &pd = i->second;
      uint64_t change = pd.m_change == (uint64_t)-1 ? 0 : pd.m_change; // change may not be known
      uint64_t fee = pd.m_amount_in - pd.m_amount_out;
      std::string dests;
      for (const auto &d: pd.m_dests) {
        if (!dests.empty())
          dests += ", ";
        dests +=  get_account_address_as_str(m_wallet->nettype(), d.is_subaddress, d.addr) + ": " + print_money(d.amount);
      }
      std::string payment_id = string_tools::pod_to_hex(i->second.m_payment_id);
      if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
        payment_id = payment_id.substr(0,16);
      success_msg_writer() << "Outgoing transaction found";
      success_msg_writer() << "txid: " << txid;
      success_msg_writer() << "Height: " << pd.m_block_height;
      success_msg_writer() << "Timestamp: " << tools::get_human_readable_timestamp(pd.m_timestamp);
      success_msg_writer() << "Amount: " << print_money(pd.m_amount_in - change - fee);
      success_msg_writer() << "Payment ID: " << payment_id;
      success_msg_writer() << "Change: " << print_money(change);
      success_msg_writer() << "Fee: " << print_money(fee);
      success_msg_writer() << "Destinations: " << dests;
      if (pd.m_unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER)
      {
        uint64_t bh = std::max(pd.m_unlock_time, pd.m_block_height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE);
        if (bh >= last_block_height)
          success_msg_writer() << "Locked: " << (bh - last_block_height) << " blocks to unlock";
        else
          success_msg_writer() << std::to_string(last_block_height - bh) << " confirmations";
      }
      else
      {
        uint64_t current_time = static_cast<uint64_t>(time(NULL));
        uint64_t threshold = current_time + CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_SECONDS_V2;
        if (threshold >= pd.m_unlock_time)
          success_msg_writer() << "unlocked for " << tools::get_human_readable_timespan(std::chrono::seconds(threshold - pd.m_unlock_time));
        else
          success_msg_writer() << "locked for " << tools::get_human_readable_timespan(std::chrono::seconds(pd.m_unlock_time - threshold));
      }
      success_msg_writer() << "Note: " << m_wallet->get_tx_note(txid);
      return true;
    }
  }

  try
  {
    m_wallet->update_pool_state();
    std::list<std::pair<crypto::hash, tools::wallet2::pool_payment_details>> pool_payments;
    m_wallet->get_unconfirmed_payments(pool_payments, m_current_subaddress_account);
    for (std::list<std::pair<crypto::hash, tools::wallet2::pool_payment_details>>::const_iterator i = pool_payments.begin(); i != pool_payments.end(); ++i) {
      const tools::wallet2::payment_details &pd = i->second.m_pd;
      if (pd.m_tx_hash == txid)
      {
        std::string payment_id = string_tools::pod_to_hex(i->first);
        if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
          payment_id = payment_id.substr(0,16);
        success_msg_writer() << "Unconfirmed incoming transaction found in the txpool";
        success_msg_writer() << "txid: " << txid;
        success_msg_writer() << "Timestamp: " << tools::get_human_readable_timestamp(pd.m_timestamp);
        success_msg_writer() << "Amount: " << print_money(pd.m_amount);
        success_msg_writer() << "Payment ID: " << payment_id;
        success_msg_writer() << "Address index: " << pd.m_subaddr_index.minor;
        success_msg_writer() << "Note: " << m_wallet->get_tx_note(txid);
        if (i->second.m_double_spend_seen)
          success_msg_writer() << tr("Double spend seen on the network: this transaction may or may not end up being mined");
        return true;
      }
    }
  }
  catch (...)
  {
    fail_msg_writer() << "Failed to get pool state";
  }

  std::list<std::pair<crypto::hash, tools::wallet2::unconfirmed_transfer_details>> upayments;
  m_wallet->get_unconfirmed_payments_out(upayments, m_current_subaddress_account);
  for (std::list<std::pair<crypto::hash, tools::wallet2::unconfirmed_transfer_details>>::const_iterator i = upayments.begin(); i != upayments.end(); ++i) {
    if (i->first == txid)
    {
      const tools::wallet2::unconfirmed_transfer_details &pd = i->second;
      uint64_t amount = pd.m_amount_in;
      uint64_t fee = amount - pd.m_amount_out;
      std::string payment_id = string_tools::pod_to_hex(i->second.m_payment_id);
      if (payment_id.substr(16).find_first_not_of('0') == std::string::npos)
        payment_id = payment_id.substr(0,16);
      bool is_failed = pd.m_state == tools::wallet2::unconfirmed_transfer_details::failed;

      success_msg_writer() << (is_failed ? "Failed" : "Pending") << " outgoing transaction found";
      success_msg_writer() << "txid: " << txid;
      success_msg_writer() << "Timestamp: " << tools::get_human_readable_timestamp(pd.m_timestamp);
      success_msg_writer() << "Amount: " << print_money(amount - pd.m_change - fee);
      success_msg_writer() << "Payment ID: " << payment_id;
      success_msg_writer() << "Change: " << print_money(pd.m_change);
      success_msg_writer() << "Fee: " << print_money(fee);
      success_msg_writer() << "Note: " << m_wallet->get_tx_note(txid);
      return true;
    }
  }

  fail_msg_writer() << tr("Transaction ID not found");
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::process_command(const std::vector<std::string> &args)
{
  return m_cmd_binder.process_command_vec(args);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::interrupt()
{
  if (m_in_manual_refresh.load(std::memory_order_relaxed))
  {
    m_wallet->stop();
  }
  else
  {
    stop();
  }
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::commit_or_save(std::vector<tools::wallet2::pending_tx>& ptx_vector, bool do_not_relay)
{
  size_t i = 0;
  std::string msg_buf; // NOTE(antd): Buffer output so integration tests read the entire output
  msg_buf.reserve(128);

  while (!ptx_vector.empty())
  {
    msg_buf.clear();
    auto & ptx = ptx_vector.back();
    const crypto::hash txid = get_transaction_hash(ptx.tx);
    if (do_not_relay)
    {
      cryptonote::blobdata blob;
      tx_to_blob(ptx.tx, blob);
      const std::string blob_hex = epee::string_tools::buff_to_hex_nodelimer(blob);
      const std::string filename = "raw_antd_tx" + (ptx_vector.size() == 1 ? "" : ("_" + std::to_string(i++)));
      bool success = epee::file_io_utils::save_string_to_file(filename, blob_hex);

      if (success) msg_buf += tr("Transaction successfully saved to ");
      else         msg_buf += tr("Failed to save transaction to ");

      msg_buf += filename;
      msg_buf += tr(", txid <");
      msg_buf += epee::string_tools::pod_to_hex(txid);
      msg_buf += ">";

      if (success) success_msg_writer(true) << msg_buf;
      else         fail_msg_writer()        << msg_buf;
    }
    else
    {
      m_wallet->commit_tx(ptx);
      msg_buf += tr("Transaction successfully submitted, transaction <");
      msg_buf += epee::string_tools::pod_to_hex(txid);
      msg_buf += ">\n";
      msg_buf += tr("You can check its status by using the `show_transfers` command.");
      success_msg_writer(true) << msg_buf;
    }
    // if no exception, remove element from vector
    ptx_vector.pop_back();
  }
}
//----------------------------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
  TRY_ENTRY();

#ifdef WIN32
  // Activate UTF-8 support for Boost filesystem classes on Windows
  std::locale::global(boost::locale::generator().generate(""));
  boost::filesystem::path::imbue(std::locale());
#endif

  po::options_description desc_params(wallet_args::tr("Wallet options"));
  tools::wallet2::init_options(desc_params);
  command_line::add_arg(desc_params, arg_wallet_file);
  command_line::add_arg(desc_params, arg_generate_new_wallet);
  command_line::add_arg(desc_params, arg_generate_from_device);
  command_line::add_arg(desc_params, arg_generate_from_view_key);
  command_line::add_arg(desc_params, arg_generate_from_spend_key);
  command_line::add_arg(desc_params, arg_generate_from_keys);
  command_line::add_arg(desc_params, arg_generate_from_multisig_keys);
  command_line::add_arg(desc_params, arg_generate_from_json);
  command_line::add_arg(desc_params, arg_mnemonic_language);
  command_line::add_arg(desc_params, arg_command);

  command_line::add_arg(desc_params, arg_restore_deterministic_wallet );
  command_line::add_arg(desc_params, arg_restore_multisig_wallet );
  command_line::add_arg(desc_params, arg_non_deterministic );
  command_line::add_arg(desc_params, arg_electrum_seed );
  command_line::add_arg(desc_params, arg_allow_mismatched_daemon_version);
  command_line::add_arg(desc_params, arg_restore_height);
  command_line::add_arg(desc_params, arg_restore_date);
  command_line::add_arg(desc_params, arg_do_not_relay);
  command_line::add_arg(desc_params, arg_create_address_file);
  command_line::add_arg(desc_params, arg_subaddress_lookahead);
  command_line::add_arg(desc_params, arg_use_english_language_names);
  command_line::add_arg(desc_params, arg_long_payment_id_support);

  po::positional_options_description positional_options;
  positional_options.add(arg_command.name, -1);

  boost::optional<po::variables_map> vm;
  bool should_terminate = false;
  std::tie(vm, should_terminate) = wallet_args::main(
   argc, argv,
   "antd-wallet-cli [--wallet-file=<filename>|--generate-new-wallet=<filename>] [<COMMAND>]",
    sw::tr("This is the command line Antd wallet. It needs to connect to a Antd\ndaemon to work correctly.\n\nWARNING: Do not reuse your Antd keys on a contentious fork, doing so will harm your privacy.\n Only consider reusing your key on a contentious fork if the fork has key reuse mitigations built in."),
    desc_params,
    positional_options,
    [](const std::string &s, bool emphasis){ tools::scoped_message_writer(emphasis ? epee::console_color_white : epee::console_color_default, true) << s; },
    "antd-wallet-cli.log"
  );

  if (!vm)
  {
    return 1;
  }

  if (should_terminate)
  {
    return 0;
  }

  cryptonote::simple_wallet w;
  const bool r = w.init(*vm);
  CHECK_AND_ASSERT_MES(r, 1, sw::tr("Failed to initialize wallet"));

  std::vector<std::string> command = command_line::get_arg(*vm, arg_command);
  if (!command.empty())
  {
    if (!w.process_command(command))
      fail_msg_writer() << sw::tr("Unknown command: ") << command.front();
    w.stop();
    w.deinit();
  }
  else
  {
    tools::signal_handler::install([&w](int type) {
      if (tools::password_container::is_prompting.load())
      {
        // must be prompting for password so return and let the signal stop prompt
        return;
      }
#ifdef WIN32
      if (type == CTRL_C_EVENT)
#else
      if (type == SIGINT)
#endif
      {
        // if we're pressing ^C when refreshing, just stop refreshing
        w.interrupt();
      }
      else
      {
        w.stop();
      }
    });
    w.run();

    w.deinit();
  }
  return 0;
  CATCH_ENTRY_L0("main", 1);
}

// MMS ---------------------------------------------------------------------------------------------------

// Access to the message store, or more exactly to the list of the messages that can be changed
// by the idle thread, is guarded by the same mutex-based mechanism as access to the wallet
// as a whole and thus e.g. uses the "LOCK_IDLE_SCOPE" macro. This is a little over-cautious, but
// simple and safe. Care has to be taken however where MMS methods call other simplewallet methods
// that use "LOCK_IDLE_SCOPE" as this cannot be nested!

// Methods for commands like "export_multisig_info" usually read data from file(s) or write data
// to files. The MMS calls now those methods as well, to produce data for messages and to process data
// from messages. As it would be quite inconvenient for the MMS to write data for such methods to files
// first or get data out of result files after the call, those methods detect a call from the MMS and
// expect data as arguments instead of files and give back data by calling 'process_wallet_created_data'.

bool simple_wallet::user_confirms(const std::string &question)
{
   std::string answer = input_line(question + tr(" (Y/Yes/N/No): "));
   return !std::cin.eof() && command_line::is_yes(answer);
}

bool simple_wallet::get_number_from_arg(const std::string &arg, uint32_t &number, const uint32_t lower_bound, const uint32_t upper_bound)
{
  bool valid = false;
  try
  {
    number = boost::lexical_cast<uint32_t>(arg);
    valid = (number >= lower_bound) && (number <= upper_bound);
  }
  catch(const boost::bad_lexical_cast &)
  {
  }
  return valid;
}

bool simple_wallet::choose_mms_processing(const std::vector<mms::processing_data> &data_list, uint32_t &choice)
{
  size_t choices = data_list.size();
  if (choices == 1)
  {
    choice = 0;
    return true;
  }
  mms::message_store& ms = m_wallet->get_message_store();
  message_writer() << tr("Choose processing:");
  std::string text;
  for (size_t i = 0; i < choices; ++i)
  {
    const mms::processing_data &data = data_list[i];
    text = std::to_string(i+1) + ": ";
    switch (data.processing)
    {
    case mms::message_processing::sign_tx:
      text += tr("Sign tx");
      break;
    case mms::message_processing::send_tx:
    {
      mms::message m;
      ms.get_message_by_id(data.message_ids[0], m);
      if (m.type == mms::message_type::fully_signed_tx)
      {
        text += tr("Send the tx for submission to ");
      }
      else
      {
        text += tr("Send the tx for signing to ");
      }
      mms::authorized_signer signer = ms.get_signer(data.receiving_signer_index);
      text += ms.signer_to_string(signer, 50);
      break;
    }
    case mms::message_processing::submit_tx:
      text += tr("Submit tx");
      break;
    default:
      text += tr("unknown");
      break;
    }
    message_writer() << text;
  }

  std::string line = input_line(tr("Choice: "));
  if (std::cin.eof() || line.empty())
  {
    return false;
  }
  bool choice_ok = get_number_from_arg(line, choice, 1, choices);
  if (choice_ok)
  {
    choice--;
  }
  else
  {
    fail_msg_writer() << tr("Wrong choice");
  }
  return choice_ok;
}

void simple_wallet::list_mms_messages(const std::vector<mms::message> &messages)
{
  message_writer() << boost::format("%4s %-4s %-30s %-21s %7s %3s %-15s %-40s") % tr("Id") % tr("I/O") % tr("Authorized Signer")
          % tr("Message Type") % tr("Height") % tr("R") % tr("Message State") % tr("Since");
  mms::message_store& ms = m_wallet->get_message_store();
  uint64_t now = (uint64_t)time(NULL);
  for (size_t i = 0; i < messages.size(); ++i)
  {
    const mms::message &m = messages[i];
    const mms::authorized_signer &signer = ms.get_signer(m.signer_index);
    bool highlight = (m.state == mms::message_state::ready_to_send) || (m.state == mms::message_state::waiting);
    message_writer(m.direction == mms::message_direction::out ? console_color_green : console_color_magenta, highlight) <<
            boost::format("%4s %-4s %-30s %-21s %7s %3s %-15s %-40s") %
            m.id %
            ms.message_direction_to_string(m.direction) %
            ms.signer_to_string(signer, 30) %
            ms.message_type_to_string(m.type) %
            m.wallet_height %
            m.round %
            ms.message_state_to_string(m.state) %
            (tools::get_human_readable_timestamp(m.modified) + ", " + tools::get_human_readable_timespan(std::chrono::seconds(now - m.modified)) + tr(" ago"));
  }
}

void simple_wallet::list_signers(const std::vector<mms::authorized_signer> &signers)
{
  message_writer() << boost::format("%2s %-20s %-s") % tr("#") % tr("Label") % tr("Transport Address");
  message_writer() << boost::format("%2s %-20s %-s") % "" % tr("Auto-Config Token") % tr("Antd Address");
  for (size_t i = 0; i < signers.size(); ++i)
  {
    const mms::authorized_signer &signer = signers[i];
    std::string label = signer.label.empty() ? tr("<not set>") : signer.label;
    std::string monero_address;
    if (signer.monero_address_known)
    {
      monero_address = get_account_address_as_str(m_wallet->nettype(), false, signer.monero_address);
    }
    else
    {
      monero_address = tr("<not set>");
    }
    std::string transport_address = signer.transport_address.empty() ? tr("<not set>") : signer.transport_address;
    message_writer() << boost::format("%2s %-20s %-s") % (i + 1) % label % transport_address;
    message_writer() << boost::format("%2s %-20s %-s") % "" % signer.auto_config_token % monero_address;
    message_writer() << "";
  }
}

void simple_wallet::add_signer_config_messages()
{
  mms::message_store& ms = m_wallet->get_message_store();
  std::string signer_config;
  ms.get_signer_config(signer_config);

  const std::vector<mms::authorized_signer> signers = ms.get_all_signers();
  mms::multisig_wallet_state state = get_multisig_wallet_state();
  uint32_t num_authorized_signers = ms.get_num_authorized_signers();
  for (uint32_t i = 1 /* without me */; i < num_authorized_signers; ++i)
  {
    ms.add_message(state, i, mms::message_type::signer_config, mms::message_direction::out, signer_config);
  }
}

void simple_wallet::show_message(const mms::message &m)
{
  mms::message_store& ms = m_wallet->get_message_store();
  const mms::authorized_signer &signer = ms.get_signer(m.signer_index);
  bool display_content;
  std::string sanitized_text;
  switch (m.type)
  {
  case mms::message_type::key_set:
  case mms::message_type::additional_key_set:
  case mms::message_type::note:
    display_content = true;
    ms.get_sanitized_message_text(m, sanitized_text);
    break;
  default:
    display_content = false;
  }
  uint64_t now = (uint64_t)time(NULL);
  message_writer() << "";
  message_writer() << tr("Message ") << m.id;
  message_writer() << tr("In/out: ") << ms.message_direction_to_string(m.direction);
  message_writer() << tr("Type: ") << ms.message_type_to_string(m.type);
  message_writer() << tr("State: ") << boost::format(tr("%s since %s, %s ago")) %
          ms.message_state_to_string(m.state) % tools::get_human_readable_timestamp(m.modified) % tools::get_human_readable_timespan(std::chrono::seconds(now - m.modified));
  if (m.sent == 0)
  {
    message_writer() << tr("Sent: Never");
  }
  else
  {
    message_writer() << boost::format(tr("Sent: %s, %s ago")) %
            tools::get_human_readable_timestamp(m.sent) % tools::get_human_readable_timespan(std::chrono::seconds(now - m.sent));
  }
  message_writer() << tr("Authorized signer: ") << ms.signer_to_string(signer, 100);
  message_writer() << tr("Content size: ") << m.content.length() << tr(" bytes");
  message_writer() << tr("Content: ") << (display_content ? sanitized_text : tr("(binary data)"));

  if (m.type == mms::message_type::note)
  {
    // Showing a note and read its text is "processing" it: Set the state accordingly
    // which will also delete it from Bitmessage as a side effect
    // (Without this little "twist" it would never change the state, and never get deleted)
    ms.set_message_processed_or_sent(m.id);
  }
}

void simple_wallet::ask_send_all_ready_messages()
{
  mms::message_store& ms = m_wallet->get_message_store();
  std::vector<mms::message> ready_messages;
  const std::vector<mms::message> &messages = ms.get_all_messages();
  for (size_t i = 0; i < messages.size(); ++i)
  {
    const mms::message &m = messages[i];
    if (m.state == mms::message_state::ready_to_send)
    {
      ready_messages.push_back(m);
    }
  }
  if (ready_messages.size() != 0)
  {
    list_mms_messages(ready_messages);
    bool send = ms.get_auto_send();
    if (!send)
    {
      send = user_confirms(tr("Send these messages now?"));
    }
    if (send)
    {
      mms::multisig_wallet_state state = get_multisig_wallet_state();
      for (size_t i = 0; i < ready_messages.size(); ++i)
      {
        ms.send_message(state, ready_messages[i].id);
        ms.set_message_processed_or_sent(ready_messages[i].id);
      }
      success_msg_writer() << tr("Queued for sending.");
    }
  }
}

bool simple_wallet::get_message_from_arg(const std::string &arg, mms::message &m)
{
  mms::message_store& ms = m_wallet->get_message_store();
  bool valid_id = false;
  uint32_t id;
  try
  {
    id = (uint32_t)boost::lexical_cast<uint32_t>(arg);
    valid_id = ms.get_message_by_id(id, m);
  }
  catch (const boost::bad_lexical_cast &)
  {
  }
  if (!valid_id)
  {
    fail_msg_writer() << tr("Invalid message id");
  }
  return valid_id;
}

void simple_wallet::mms_init(const std::vector<std::string> &args)
{
  if (args.size() != 3)
  {
    fail_msg_writer() << tr("usage: mms init <required_signers>/<authorized_signers> <own_label> <own_transport_address>");
    return;
  }
  mms::message_store& ms = m_wallet->get_message_store();
  if (ms.get_active())
  {
    if (!user_confirms(tr("The MMS is already initialized. Re-initialize by deleting all signer info and messages?")))
    {
      return;
    }
  }
  uint32_t num_required_signers;
  uint32_t num_authorized_signers;
  const std::string &mn = args[0];
  std::vector<std::string> numbers;
  boost::split(numbers, mn, boost::is_any_of("/"));
  bool mn_ok = (numbers.size() == 2)
               && get_number_from_arg(numbers[1], num_authorized_signers, 2, 100)
               && get_number_from_arg(numbers[0], num_required_signers, 2, num_authorized_signers);
  if (!mn_ok)
  {
    fail_msg_writer() << tr("Error in the number of required signers and/or authorized signers");
    return;
  }
  LOCK_IDLE_SCOPE();
  ms.init(get_multisig_wallet_state(), args[1], args[2], num_authorized_signers, num_required_signers);
}

void simple_wallet::mms_info(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  if (ms.get_active())
  {
    message_writer() << boost::format("The MMS is active for %s/%s multisig.")
            % ms.get_num_required_signers() % ms.get_num_authorized_signers();
  }
  else
  {
    message_writer() << tr("The MMS is not active.");
  }
}

void simple_wallet::mms_signer(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  const std::vector<mms::authorized_signer> &signers = ms.get_all_signers();
  if (args.size() == 0)
  {
    // Without further parameters list all defined signers
    list_signers(signers);
    return;
  }

  uint32_t index;
  bool index_valid = get_number_from_arg(args[0], index, 1, ms.get_num_authorized_signers());
  if (index_valid)
  {
    index--;
  }
  else
  {
    fail_msg_writer() << tr("Invalid signer number ") + args[0];
    return;
  }
  if ((args.size() < 2) || (args.size() > 4))
  {
    fail_msg_writer() << tr("mms signer [<number> <label> [<transport_address> [<antd_address>]]]");
    return;
  }

  boost::optional<string> label = args[1];
  boost::optional<string> transport_address;
  if (args.size() >= 3)
  {
    transport_address = args[2];
  }
  boost::optional<cryptonote::account_public_address> monero_address;
  LOCK_IDLE_SCOPE();
  mms::multisig_wallet_state state = get_multisig_wallet_state();
  if (args.size() == 4)
  {
    cryptonote::address_parse_info info;
    bool ok = cryptonote::get_account_address_from_str_or_url(info, m_wallet->nettype(), args[3], oa_prompter);
    if (!ok)
    {
      fail_msg_writer() << tr("Invalid Antd address");
      return;
    }
    monero_address = info.address;
    const std::vector<mms::message> &messages = ms.get_all_messages();
    if ((messages.size() > 0) || state.multisig)
    {
      fail_msg_writer() << tr("Wallet state does not allow changing Antd addresses anymore");
      return;
    }
  }
  ms.set_signer(state, index, label, transport_address, monero_address);
}

void simple_wallet::mms_list(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  if (args.size() != 0)
  {
    fail_msg_writer() << tr("Usage: mms list");
    return;
  }
  LOCK_IDLE_SCOPE();
  const std::vector<mms::message> &messages = ms.get_all_messages();
  list_mms_messages(messages);
}

void simple_wallet::mms_next(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  if ((args.size() > 1) || ((args.size() == 1) && (args[0] != "sync")))
  {
    fail_msg_writer() << tr("Usage: mms next [sync]");
    return;
  }
  bool avail = false;
  std::vector<mms::processing_data> data_list;
  bool force_sync = false;
  uint32_t choice = 0;
  {
    LOCK_IDLE_SCOPE();
    if ((args.size() == 1) && (args[0] == "sync"))
    {
      // Force the MMS to process any waiting sync info although on its own it would just ignore
      // those messages because no need to process them can be seen
      force_sync = true;
    }
    string wait_reason;
    {
      avail = ms.get_processable_messages(get_multisig_wallet_state(), force_sync, data_list, wait_reason);
    }
    if (avail)
    {
      avail = choose_mms_processing(data_list, choice);
    }
    else if (!wait_reason.empty())
    {
      message_writer() << tr("No next step: ") << wait_reason;
    }
  }
  if (avail)
  {
    mms::processing_data data = data_list[choice];
    bool command_successful = false;
    switch(data.processing)
    {
    case mms::message_processing::prepare_multisig:
      message_writer() << tr("prepare_multisig");
      command_successful = prepare_multisig_main(std::vector<std::string>(), true);
      break;

    case mms::message_processing::make_multisig:
    {
      message_writer() << tr("make_multisig");
      size_t number_of_key_sets = data.message_ids.size();
      std::vector<std::string> sig_args(number_of_key_sets + 1);
      sig_args[0] = std::to_string(ms.get_num_required_signers());
      for (size_t i = 0; i < number_of_key_sets; ++i)
      {
        mms::message m = ms.get_message_by_id(data.message_ids[i]);
        sig_args[i+1] = m.content;
      }
      command_successful = make_multisig_main(sig_args, true);
      break;
    }

    case mms::message_processing::exchange_multisig_keys:
    {
      message_writer() << tr("exchange_multisig_keys");
      size_t number_of_key_sets = data.message_ids.size();
      // Other than "make_multisig" only the key sets as parameters, no num_required_signers
      std::vector<std::string> sig_args(number_of_key_sets);
      for (size_t i = 0; i < number_of_key_sets; ++i)
      {
        mms::message m = ms.get_message_by_id(data.message_ids[i]);
        sig_args[i] = m.content;
      }
      command_successful = exchange_multisig_keys_main(sig_args, true);
      break;
    }

    case mms::message_processing::create_sync_data:
    {
      message_writer() << tr("export_multisig_info");
      std::vector<std::string> export_args;
      export_args.push_back("MMS");  // dummy filename
      command_successful = export_multisig_main(export_args, true);
      break;
    }

    case mms::message_processing::process_sync_data:
    {
      message_writer() << tr("import_multisig_info");
      std::vector<std::string> import_args;
      for (size_t i = 0; i < data.message_ids.size(); ++i)
      {
        mms::message m = ms.get_message_by_id(data.message_ids[i]);
        import_args.push_back(m.content);
      }
      command_successful = import_multisig_main(import_args, true);
      break;
    }

    case mms::message_processing::sign_tx:
    {
      message_writer() << tr("sign_multisig");
      std::vector<std::string> sign_args;
      mms::message m = ms.get_message_by_id(data.message_ids[0]);
      sign_args.push_back(m.content);
      command_successful = sign_multisig_main(sign_args, true);
      break;
    }

    case mms::message_processing::submit_tx:
    {
      message_writer() << tr("submit_multisig");
      std::vector<std::string> submit_args;
      mms::message m = ms.get_message_by_id(data.message_ids[0]);
      submit_args.push_back(m.content);
      command_successful = submit_multisig_main(submit_args, true);
      break;
    }

    case mms::message_processing::send_tx:
    {
      message_writer() << tr("Send tx");
      mms::message m = ms.get_message_by_id(data.message_ids[0]);
      LOCK_IDLE_SCOPE();
      ms.add_message(get_multisig_wallet_state(), data.receiving_signer_index, m.type, mms::message_direction::out,
                     m.content);
      command_successful = true;
      break;
    }

    case mms::message_processing::process_signer_config:
    {
      message_writer() << tr("Process signer config");
      LOCK_IDLE_SCOPE();
      mms::message m = ms.get_message_by_id(data.message_ids[0]);
      mms::authorized_signer me = ms.get_signer(0);
      mms::multisig_wallet_state state = get_multisig_wallet_state();
      if (!me.auto_config_running)
      {
        // If no auto-config is running, the config sent may be unsolicited or problematic
        // so show what arrived and ask for confirmation before taking it in
        std::vector<mms::authorized_signer> signers;
        ms.unpack_signer_config(state, m.content, signers);
        list_signers(signers);
        if (!user_confirms(tr("Replace current signer config with the one displayed above?")))
        {
          break;
        }
      }
      ms.process_signer_config(state, m.content);
      ms.stop_auto_config();
      list_signers(ms.get_all_signers());
      command_successful = true;
      break;
    }

    case mms::message_processing::process_auto_config_data:
    {
      message_writer() << tr("Process auto config data");
      LOCK_IDLE_SCOPE();
      for (size_t i = 0; i < data.message_ids.size(); ++i)
      {
        ms.process_auto_config_data_message(data.message_ids[i]);
      }
      ms.stop_auto_config();
      list_signers(ms.get_all_signers());
      add_signer_config_messages();
      command_successful = true;
      break;
    }

    default:
      message_writer() << tr("Nothing ready to process");
      break;
    }

    if (command_successful)
    {
      {
        LOCK_IDLE_SCOPE();
        ms.set_messages_processed(data);
        ask_send_all_ready_messages();
      }
    }
  }
}

void simple_wallet::mms_sync(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  if (args.size() != 0)
  {
    fail_msg_writer() << tr("Usage: mms sync");
    return;
  }
  // Force the start of a new sync round, for exceptional cases where something went wrong
  // Can e.g. solve the problem "This signature was made with stale data" after trying to
  // create 2 transactions in a row somehow
  // Code is identical to the code for 'message_processing::create_sync_data'
  message_writer() << tr("export_multisig_info");
  std::vector<std::string> export_args;
  export_args.push_back("MMS");  // dummy filename
  export_multisig_main(export_args, true);
  ask_send_all_ready_messages();
}

void simple_wallet::mms_transfer(const std::vector<std::string> &args)
{
  // It's too complicated to check any arguments here, just let 'transfer_main' do the whole job
  transfer_main(Transfer, args, true);
}

void simple_wallet::mms_delete(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    fail_msg_writer() << tr("Usage: mms delete (<message_id> | all)");
    return;
  }
  LOCK_IDLE_SCOPE();
  mms::message_store& ms = m_wallet->get_message_store();
  if (args[0] == "all")
  {
    if (user_confirms(tr("Delete all messages?")))
    {
      ms.delete_all_messages();
    }
  }
  else
  {
    mms::message m;
    bool valid_id = get_message_from_arg(args[0], m);
    if (valid_id)
    {
      // If only a single message and not all delete even if unsent / unprocessed
      ms.delete_message(m.id);
    }
  }
}

void simple_wallet::mms_send(const std::vector<std::string> &args)
{
  if (args.size() == 0)
  {
    ask_send_all_ready_messages();
    return;
  }
  else if (args.size() != 1)
  {
    fail_msg_writer() << tr("Usage: mms send [<message_id>]");
    return;
  }
  LOCK_IDLE_SCOPE();
  mms::message_store& ms = m_wallet->get_message_store();
  mms::message m;
  bool valid_id = get_message_from_arg(args[0], m);
  if (valid_id)
  {
    ms.send_message(get_multisig_wallet_state(), m.id);
  }
}

void simple_wallet::mms_receive(const std::vector<std::string> &args)
{
  if (args.size() != 0)
  {
    fail_msg_writer() << tr("Usage: mms receive");
    return;
  }
  std::vector<mms::message> new_messages;
  LOCK_IDLE_SCOPE();
  mms::message_store& ms = m_wallet->get_message_store();
  bool avail = ms.check_for_messages(get_multisig_wallet_state(), new_messages);
  if (avail)
  {
    list_mms_messages(new_messages);
  }
}

void simple_wallet::mms_export(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    fail_msg_writer() << tr("Usage: mms export <message_id>");
    return;
  }
  LOCK_IDLE_SCOPE();
  mms::message_store& ms = m_wallet->get_message_store();
  mms::message m;
  bool valid_id = get_message_from_arg(args[0], m);
  if (valid_id)
  {
    const std::string filename = "mms_message_content";
    if (epee::file_io_utils::save_string_to_file(filename, m.content))
    {
      success_msg_writer() << tr("Message content saved to: ") << filename;
    }
    else
    {
      fail_msg_writer() << tr("Failed to to save message content");
    }
  }
}

void simple_wallet::mms_note(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  if (args.size() == 0)
  {
    LOCK_IDLE_SCOPE();
    const std::vector<mms::message> &messages = ms.get_all_messages();
    for (size_t i = 0; i < messages.size(); ++i)
    {
      const mms::message &m = messages[i];
      if ((m.type == mms::message_type::note) && (m.state == mms::message_state::waiting))
      {
        show_message(m);
      }
    }
    return;
  }
  if (args.size() < 2)
  {
    fail_msg_writer() << tr("Usage: mms note [<label> <text>]");
    return;
  }
  uint32_t signer_index;
  bool found = ms.get_signer_index_by_label(args[0], signer_index);
  if (!found)
  {
    fail_msg_writer() << tr("No signer found with label ") << args[0];
    return;
  }
  std::string note = "";
  for (size_t n = 1; n < args.size(); ++n)
  {
    if (n > 1)
    {
      note += " ";
    }
    note += args[n];
  }
  LOCK_IDLE_SCOPE();
  ms.add_message(get_multisig_wallet_state(), signer_index, mms::message_type::note,
                 mms::message_direction::out, note);
  ask_send_all_ready_messages();
}

void simple_wallet::mms_show(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    fail_msg_writer() << tr("Usage: mms show <message_id>");
    return;
  }
  LOCK_IDLE_SCOPE();
  mms::message_store& ms = m_wallet->get_message_store();
  mms::message m;
  bool valid_id = get_message_from_arg(args[0], m);
  if (valid_id)
  {
    show_message(m);
  }
}

void simple_wallet::mms_set(const std::vector<std::string> &args)
{
  bool set = args.size() == 2;
  bool query = args.size() == 1;
  if (!set && !query)
  {
    fail_msg_writer() << tr("Usage: mms set <option_name> [<option_value>]");
    return;
  }
  mms::message_store& ms = m_wallet->get_message_store();
  LOCK_IDLE_SCOPE();
  if (args[0] == "auto-send")
  {
    if (set)
    {
      bool result;
      bool ok = parse_bool(args[1], result);
      if (ok)
      {
        ms.set_auto_send(result);
      }
      else
      {
        fail_msg_writer() << tr("Wrong option value");
      }
    }
    else
    {
      message_writer() << (ms.get_auto_send() ? tr("Auto-send is on") : tr("Auto-send is off"));
    }
  }
  else
  {
    fail_msg_writer() << tr("Unknown option");
  }
}

void simple_wallet::mms_help(const std::vector<std::string> &args)
{
  if (args.size() > 1)
  {
    fail_msg_writer() << tr("Usage: mms help [<subcommand>]");
    return;
  }
  std::vector<std::string> help_args;
  help_args.push_back("mms");
  if (args.size() == 1)
  {
    help_args.push_back(args[0]);
  }
  help(help_args);
}

void simple_wallet::mms_send_signer_config(const std::vector<std::string> &args)
{
  if (args.size() != 0)
  {
    fail_msg_writer() << tr("Usage: mms send_signer_config");
    return;
  }
  mms::message_store& ms = m_wallet->get_message_store();
  if (!ms.signer_config_complete())
  {
    fail_msg_writer() << tr("Signer config not yet complete");
    return;
  }
  LOCK_IDLE_SCOPE();
  add_signer_config_messages();
  ask_send_all_ready_messages();
}

void simple_wallet::mms_start_auto_config(const std::vector<std::string> &args)
{
  mms::message_store& ms = m_wallet->get_message_store();
  uint32_t other_signers = ms.get_num_authorized_signers() - 1;
  size_t args_size = args.size();
  if ((args_size != 0) && (args_size != other_signers))
  {
    fail_msg_writer() << tr("Usage: mms start_auto_config [<label> <label> ...]");
    return;
  }
  if ((args_size == 0) && !ms.signer_labels_complete())
  {
    fail_msg_writer() << tr("There are signers without a label set. Complete labels before auto-config or specify them as parameters here.");
    return;
  }
  mms::authorized_signer me = ms.get_signer(0);
  if (me.auto_config_running)
  {
    if (!user_confirms(tr("Auto-config is already running. Cancel and restart?")))
    {
      return;
    }
  }
  LOCK_IDLE_SCOPE();
  mms::multisig_wallet_state state = get_multisig_wallet_state();
  if (args_size != 0)
  {
    // Set (or overwrite) all the labels except "me" from the arguments
    for (uint32_t i = 1; i < (other_signers + 1); ++i)
    {
      ms.set_signer(state, i, args[i - 1], boost::none, boost::none);
    }
  }
  ms.start_auto_config(state);
  // List the signers to show the generated auto-config tokens
  list_signers(ms.get_all_signers());
}

void simple_wallet::mms_stop_auto_config(const std::vector<std::string> &args)
{
  if (args.size() != 0)
  {
    fail_msg_writer() << tr("Usage: mms stop_auto_config");
    return;
  }
  if (!user_confirms(tr("Delete any auto-config tokens and stop auto-config?")))
  {
    return;
  }
  mms::message_store& ms = m_wallet->get_message_store();
  LOCK_IDLE_SCOPE();
  ms.stop_auto_config();
}

void simple_wallet::mms_auto_config(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    fail_msg_writer() << tr("Usage: mms auto_config <auto_config_token>");
    return;
  }
  mms::message_store& ms = m_wallet->get_message_store();
  std::string adjusted_token;
  if (!ms.check_auto_config_token(args[0], adjusted_token))
  {
    fail_msg_writer() << tr("Invalid auto-config token");
    return;
  }
  mms::authorized_signer me = ms.get_signer(0);
  if (me.auto_config_running)
  {
    if (!user_confirms(tr("Auto-config already running. Cancel and restart?")))
    {
      return;
    }
  }
  LOCK_IDLE_SCOPE();
  ms.add_auto_config_data_message(get_multisig_wallet_state(), adjusted_token);
  ask_send_all_ready_messages();
}

bool simple_wallet::mms(const std::vector<std::string> &args)
{
  try
  {
    mms::message_store& ms = m_wallet->get_message_store();
    if (args.size() == 0)
    {
      mms_info(args);
      return true;
    }

    const std::string &sub_command = args[0];
    std::vector<std::string> mms_args = args;
    mms_args.erase(mms_args.begin());

    if (sub_command == "init")
    {
      mms_init(mms_args);
      return true;
    }
    if (!ms.get_active())
    {
      fail_msg_writer() << tr("The MMS is not active. Activate using the \"mms init\" command");
      return true;
    }
    else if (sub_command == "info")
    {
      mms_info(mms_args);
    }
    else if (sub_command == "signer")
    {
      mms_signer(mms_args);
    }
    else if (sub_command == "list")
    {
      mms_list(mms_args);
    }
    else if (sub_command == "next")
    {
      mms_next(mms_args);
    }
    else if (sub_command == "sync")
    {
      mms_sync(mms_args);
    }
    else if (sub_command == "transfer")
    {
      mms_transfer(mms_args);
    }
    else if (sub_command == "delete")
    {
      mms_delete(mms_args);
    }
    else if (sub_command == "send")
    {
      mms_send(mms_args);
    }
    else if (sub_command == "receive")
    {
      mms_receive(mms_args);
    }
    else if (sub_command == "export")
    {
      mms_export(mms_args);
    }
    else if (sub_command == "note")
    {
      mms_note(mms_args);
    }
    else if (sub_command == "show")
    {
      mms_show(mms_args);
    }
    else if (sub_command == "set")
    {
      mms_set(mms_args);
    }
    else if (sub_command == "help")
    {
      mms_help(mms_args);
    }
    else if (sub_command == "send_signer_config")
    {
      mms_send_signer_config(mms_args);
    }
    else if (sub_command == "start_auto_config")
    {
      mms_start_auto_config(mms_args);
    }
    else if (sub_command == "stop_auto_config")
    {
      mms_stop_auto_config(mms_args);
    }
    else if (sub_command == "auto_config")
    {
      mms_auto_config(mms_args);
    }
    else
    {
      fail_msg_writer() << tr("Invalid MMS subcommand");
    }
  }
  catch (const tools::error::no_connection_to_daemon &e)
  {
    fail_msg_writer() << tr("Error in MMS command: ") << e.what() << " " << e.request();
  }
  catch (const std::exception &e)
  {
    fail_msg_writer() << tr("Error in MMS command: ") << e.what();
    PRINT_USAGE(USAGE_MMS);
    return true;
  }
  return true;
}
// End MMS ------------------------------------------------------------------------------------------------
