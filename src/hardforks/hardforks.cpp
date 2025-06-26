#include "hardforks.h"
#include "cryptonote_config.h"





const hardfork_t mainnet_hard_forks[] = {
  { cryptonote::network_version_7, 1, 0, 1744823806 },
  { cryptonote::network_version_8, 200, 0, 1744824295 },
  { cryptonote::network_version_9_full_nodes, 300, 0, 1744825855 },
  { cryptonote::network_version_10_bulletproofs, 400, 0, 1744826995 },
  { cryptonote::network_version_11_infinite_staking, 450, 0, 1749055891 }
};

const size_t num_mainnet_hard_forks = sizeof(mainnet_hard_forks) / sizeof(mainnet_hard_forks[0]);

const hardfork_t testnet_hard_forks[] = {
  { cryptonote::network_version_7, 1, 0, 1744823806 },
  { cryptonote::network_version_8, 200, 0, 1744824295 },
  { cryptonote::network_version_9_full_nodes, 300, 0, 1744825855 },
  { cryptonote::network_version_10_bulletproofs, 400, 0, 1744826995 },
  { cryptonote::network_version_11_infinite_staking, 450, 0, 1749055891 }
};

const size_t num_testnet_hard_forks = sizeof(testnet_hard_forks) / sizeof(testnet_hard_forks[0]);

const hardfork_t stagenet_hard_forks[] = {
  { cryptonote::network_version_7, 1, 0, 1744823806 },
  { cryptonote::network_version_8, 200, 0, 1744824295 },
  { cryptonote::network_version_9_full_nodes, 300, 0, 1744825855 },
  { cryptonote::network_version_10_bulletproofs, 400, 0, 1744826995 },
  { cryptonote::network_version_11_infinite_staking, 450, 0, 1749055891 }
};

const size_t num_stagenet_hard_forks = sizeof(stagenet_hard_forks) / sizeof(stagenet_hard_forks[0]);
