// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PORTS_SRC_PORT_H_
#define PORTS_SRC_PORT_H_

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "mojo/edk/system/ports/message_queue.h"

namespace ports {

class Port {
 public:
  enum State {
    kReceiving,
    kBuffering,
    kProxying,
    kClosed
  };

  std::mutex lock;
  State state;
  NodeName peer_node_name;
  PortName peer_port_name;
  uint32_t next_sequence_num_to_send;
  uint32_t last_sequence_num_to_receive;
  MessageQueue message_queue;
  std::unique_ptr<std::pair<NodeName, ScopedMessage>> send_on_proxy_removal;
  std::shared_ptr<UserData> user_data;
  bool remove_proxy_on_last_message;
  bool peer_closed;

  Port(uint32_t next_sequence_num_to_send,
       uint32_t next_sequence_num_to_receive);
  ~Port();
};

}  // namespace ports

#endif  // PORTS_SRC_PORT_H_
