// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_EDK_SYSTEM_PORTS_NODE_H_
#define MOJO_EDK_SYSTEM_PORTS_NODE_H_

#include <stddef.h>
#include <stdint.h>

#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "base/macros.h"
#include "mojo/edk/system/ports/event.h"
#include "mojo/edk/system/ports/hash_functions.h"
#include "mojo/edk/system/ports/message.h"
#include "mojo/edk/system/ports/name.h"
#include "mojo/edk/system/ports/port.h"
#include "mojo/edk/system/ports/port_ref.h"
#include "mojo/edk/system/ports/user_data.h"

#undef SendMessage  // Gah, windows

namespace mojo {
namespace edk {
namespace ports {

enum : int {
  OK = 0,
  ERROR_PORT_UNKNOWN = -10,
  ERROR_PORT_EXISTS = -11,
  ERROR_PORT_STATE_UNEXPECTED = -12,
  ERROR_PORT_CANNOT_SEND_SELF = -13,
  ERROR_PORT_PEER_CLOSED = -14,
  ERROR_PORT_CANNOT_SEND_PEER = -15,
  ERROR_NOT_IMPLEMENTED = -100,
};

struct PortStatus {
  bool has_messages;
  bool peer_closed;
};

class NodeDelegate;

class Node {
 public:
  // Does not take ownership of the delegate.
  Node(const NodeName& name, NodeDelegate* delegate);
  ~Node();

  // Lookup the named port.
  int GetPort(const PortName& port_name, PortRef* port_ref);

  // Creates a port on this node. Before the port can be used, it must be
  // initialized using InitializePort. This method is useful for bootstrapping
  // a connection between two nodes. Generally, ports are created using
  // CreatePortPair instead.
  int CreateUninitializedPort(PortRef* port_ref);

  // Initializes a newly created port.
  int InitializePort(const PortRef& port_ref,
                     const NodeName& peer_node_name,
                     const PortName& peer_port_name);

  // Generates a new connected pair of ports bound to this node. These ports
  // are initialized and ready to go.
  int CreatePortPair(PortRef* port0_ref, PortRef* port1_ref);

  // User data associated with the port.
  int SetUserData(const PortRef& port_ref,
                  std::shared_ptr<UserData> user_data);
  int GetUserData(const PortRef& port_ref,
                  std::shared_ptr<UserData>* user_data);

  // Prevents further messages from being sent from this port or delivered to
  // this port. The port is removed, and the port's peer is notified of the
  // closure after it has consumed all pending messages.
  int ClosePort(const PortRef& port_ref);

  // Returns the current status of the port.
  int GetStatus(const PortRef& port_ref, PortStatus* port_status);

  // Returns the next available message on the specified port or returns a null
  // message if there are none available. Returns ERROR_PORT_PEER_CLOSED to
  // indicate that this port's peer has closed. In such cases GetMessage may
  // be called until it yields a null message, indicating that no more messages
  // may be read from the port.
  int GetMessage(const PortRef& port_ref, ScopedMessage* message);

  // Like GetMessage, but the caller may optionally supply a selector function
  // that decides whether or not to return the message. If |selector| is a
  // nullptr, then GetMessageIf acts just like GetMessage. The |selector| may
  // not call any Node methods.
  int GetMessageIf(const PortRef& port_ref,
                   std::function<bool(const Message&)> selector,
                   ScopedMessage* message);

  // Allocate a message that can be passed to SendMessage. The caller may
  // mutate the payload and ports arrays before passing the message to
  // SendMessage. The header array should not be modified by the caller.
  int AllocMessage(size_t num_payload_bytes,
                   size_t num_ports,
                   ScopedMessage* message);

  // Sends a message from the specified port to its peer. Note that the message
  // notification may arrive synchronously (via PortStatusChanged() on the
  // delegate) if the peer is local to this Node.
  int SendMessage(const PortRef& port_ref, ScopedMessage message);

  // Corresponding to NodeDelegate::ForwardMessage.
  int AcceptMessage(ScopedMessage message);

  // Called to inform this node that communication with another node is lost
  // indefinitely. This triggers cleanup of ports bound to this node.
  int LostConnectionToNode(const NodeName& node_name);

 private:
  int OnUserMessage(ScopedMessage message);
  int OnPortAccepted(const PortName& port_name);
  int OnObserveProxy(const PortName& port_name,
                     const ObserveProxyEventData& event);
  int OnObserveProxyAck(const PortName& port_name, uint64_t last_sequence_num);
  int OnObserveClosure(const PortName& port_name, uint64_t last_sequence_num);

  int AddPortWithName(const PortName& port_name,
                      const std::shared_ptr<Port>& port);
  void ErasePort(const PortName& port_name);
  std::shared_ptr<Port> GetPort(const PortName& port_name);

  void WillSendPort_Locked(Port* port,
                           const NodeName& to_node_name,
                           PortName* port_name,
                           PortDescriptor* port_descriptor);
  int AcceptPort(const PortName& port_name,
                 const PortDescriptor& port_descriptor);

  int WillSendMessage_Locked(Port* port,
                             const PortName& port_name,
                             Message* message,
                             std::vector<std::shared_ptr<Port>>* ports_taken);
  int ForwardMessages_Locked(Port* port, const PortName& port_name);
  void InitiateProxyRemoval_Locked(Port* port, const PortName& port_name);
  void MaybeRemoveProxy_Locked(Port* port, const PortName& port_name);
  void FlushOutgoingMessages_Locked(Port* port);

  ScopedMessage NewInternalMessage_Helper(const PortName& port_name,
                                          const EventType& type,
                                          const void* data,
                                          size_t num_data_bytes);

  ScopedMessage NewInternalMessage(const PortName& port_name,
                                   const EventType& type) {
    return NewInternalMessage_Helper(port_name, type, nullptr, 0);
  }

  template <typename EventData>
  ScopedMessage NewInternalMessage(const PortName& port_name,
                                   const EventType& type,
                                   const EventData& data) {
    return NewInternalMessage_Helper(port_name, type, &data, sizeof(data));
  }

  NodeName name_;
  NodeDelegate* delegate_;

  // Guards |ports_|.
  std::mutex ports_lock_;
  std::unordered_map<PortName, std::shared_ptr<Port>> ports_;

  // Guards multiple threads from sending ports simultaneously.
  std::mutex send_with_ports_lock_;

  // Guards the fields below.
  std::mutex local_message_lock_;
  bool is_delivering_local_messages_ = false;
  std::queue<ScopedMessage> local_messages_;

  DISALLOW_COPY_AND_ASSIGN(Node);
};

}  // namespace ports
}  // namespace edk
}  // namespace mojo

#endif  // MOJO_EDK_SYSTEM_PORTS_NODE_H_
