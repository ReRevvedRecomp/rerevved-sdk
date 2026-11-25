/**
 * @file        rex/online_host.h
 * @brief       What a title shows while this launch hosts an online game
 *
 * Safe from any host thread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>

#include <rex/port_mapping.h>

namespace rex::online_host {

struct Status {
  bool hosting = false;
  std::string error;
  port_mapping::Status router;
  std::string address;
  std::string friends_code;  // a credential: keep it out of logs
};

// Encodes the code on every call: not for every frame.
Status Current();

}  // namespace rex::online_host
