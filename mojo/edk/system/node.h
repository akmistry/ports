// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PORTS_MOJO_SYSTEM_NODE_H_
#define PORTS_MOJO_SYSTEM_NODE_H_

#include <queue>
#include <unordered_map>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "mojo/edk/embedder/platform_handle_vector.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "mojo/edk/system/node_channel.h"
#include "mojo/edk/system/ports/hash_functions.h"
#include "mojo/edk/system/ports/ports.h"

namespace mojo {
namespace edk {

class Core;
class PortsMessage;

// An implementation of ports::Node which facilitates core EDK implementation.
// All public interface methods are safe to call from any thread.
class Node : public ports::NodeDelegate, public NodeChannel::Delegate {
 public:
  class PortObserver : public ports::UserData {
   public:
    ~PortObserver() override {}
    virtual void OnMessagesAvailable() = 0;
  };

  // |core| owns and out-lives us.
  explicit Node(Core* core);
  ~Node() override;

  const ports::NodeName& name() const { return name_; }
  Core* core() const { return core_; }

  // Connects this node to a child node. This node will initiate a handshake.
  void ConnectToChild(ScopedPlatformHandle platform_handle);

  // Connects this node to a parent node. The parent node will initiate a
  // handshake.
  void ConnectToParent(ScopedPlatformHandle platform_handle);

  void GetPort(const ports::PortName& port_name, ports::PortRef* port);

  // Creates a single uninitialized port which is not ready for use.
  void CreateUninitializedPort(ports::PortRef* port);

  // Initializes a port with peer information.
  void InitializePort(const ports::PortRef& port,
                      const ports::NodeName& peer_node_name,
                      const ports::PortName& peer_port_name);

  // Creates a new pair of local ports on this node, returning their names.
  void CreatePortPair(ports::PortRef* port0, ports::PortRef* port1);

  // Sets a port's observer.
  void SetPortObserver(const ports::PortRef& port,
                       std::shared_ptr<PortObserver> observer);

  scoped_ptr<PortsMessage> AllocMessage(size_t num_payload_bytes,
                                        size_t num_ports);

  // Sends a message on a port to its peer.
  int SendMessage(const ports::PortRef& port_ref,
                  scoped_ptr<PortsMessage> message);

  // Enable use of lambda functions for selecting messages.
  template <typename Predicate>
  int GetMessageIf(const ports::PortRef& port_ref,
                   Predicate predicate,
                   ports::ScopedMessage* message) {
    class Adaptor : public ports::MessageSelector {
     public:
      explicit Adaptor(Predicate predicate) : predicate_(predicate) {}
      bool Select(const ports::Message& message) override {
        return predicate_(message);
      }
      Predicate predicate_;
    } adaptor(predicate);
    return node_->GetMessageIf(port_ref, &adaptor, message);
  }

  // Closes a port.
  void ClosePort(const ports::PortRef& port_ref);

  void ReservePortForToken(const ports::PortName& port_name,
                           const std::string& token,
                           const base::Closure& on_connect);

  void ConnectToParentPortByToken(const std::string& token,
                                  const ports::PortName& local_port,
                                  const base::Closure& on_connect);

 private:
  using NodeMap = std::unordered_map<ports::NodeName, scoped_ptr<NodeChannel>>;
  using OutgoingMessageQueue = std::queue<ports::ScopedMessage>;

  struct PendingTokenConnection {
    PendingTokenConnection();
    ~PendingTokenConnection();

    ports::PortName port;
    std::string token;
    base::Closure callback;
  };

  struct ReservedPort {
    ReservedPort();
    ~ReservedPort();

    ports::PortName local_port;
    base::Closure callback;
  };

  void ConnectToChildOnIOThread(ScopedPlatformHandle platform_handle);
  void ConnectToParentOnIOThread(ScopedPlatformHandle platform_handle);
  void AddPeer(const ports::NodeName& name,
               scoped_ptr<NodeChannel> channel,
               bool start_channel);
  void DropPeer(const ports::NodeName& name);
  void SendPeerMessage(const ports::NodeName& name,
                       ports::ScopedMessage message);
  void ReservePortForTokenOnIOThread(const ports::PortName& port_name,
                                     const std::string& token,
                                     const base::Closure& on_connect);
  void ConnectToParentPortByTokenOnIOThread(const std::string& token,
                                            const ports::PortName& local_port,
                                            const base::Closure& on_connect);
  void ConnectToParentPortByTokenNow(const std::string& token,
                                     const ports::PortName& local_port,
                                     const base::Closure& on_connect);
  void AcceptMessageOnIOThread(ports::ScopedMessage message);

  // ports::NodeDelegate:
  void GenerateRandomPortName(ports::PortName* port_name) override;
  void AllocMessage(size_t num_header_bytes,
                    size_t num_payload_bytes,
                    size_t num_ports,
                    ports::ScopedMessage* message) override;
  void ForwardMessage(const ports::NodeName& node,
                      ports::ScopedMessage message) override;
  void MessagesAvailable(const ports::PortRef& port) override;

  // NodeChannel::Delegate:
  void OnAcceptChild(const ports::NodeName& from_node,
                     const ports::NodeName& parent_name,
                     const ports::NodeName& token) override;
  void OnAcceptParent(const ports::NodeName& from_node,
                      const ports::NodeName& token,
                      const ports::NodeName& child_name) override;
  void OnPortsMessage(const ports::NodeName& from_node,
                      const void* payload,
                      size_t payload_size,
                      ScopedPlatformHandleVectorPtr platform_handles) override;
  void OnConnectToPort(const ports::NodeName& from_node,
                       const ports::PortName& connector_port,
                       const std::string& token) override;
  void OnConnectToPortAck(const ports::NodeName& from_node,
                          const ports::PortName& connector_port,
                          const ports::PortName& connectee_port) override;
  void OnRequestIntroduction(const ports::NodeName& from_node,
                             const ports::NodeName& name) override;
  void OnIntroduce(const ports::NodeName& from_node,
                   const ports::NodeName& name,
                   ScopedPlatformHandle channel_handle) override;
  void OnChannelError(const ports::NodeName& from_node) override;

  // These are safe to access from any thread as long as the Node is alive.
  Core* const core_;
  const ports::NodeName name_;
  const scoped_ptr<ports::Node> node_;

  // Guards peers_ and pending_peer_messages_.
  base::Lock peers_lock_;

  // Channels to known peers, including parent and children, if any.
  NodeMap peers_;

  // Outgoing message queues for peers we've heard of but can't yet talk to.
  std::unordered_map<ports::NodeName, OutgoingMessageQueue>
      pending_peer_messages_;

  // All other fields below must only be accessed on the I/O thread, i.e., the
  // thread on which core_->io_task_runner() runs tasks.

  // The name of our parent node, if any.
  ports::NodeName parent_name_;

  // A channel to our parent during handshake.
  scoped_ptr<NodeChannel> bootstrap_channel_to_parent_;

  // Channels to children during handshake.
  NodeMap pending_children_;

  // Named ports for establishing cross-node port pairs out-of-band. A port
  // can be reserved by name via ReservePortForToken(), and a peer can entangle
  // one of its owns ports to the reserved port by referencing the token in a
  // NodeChannel::ConnectToPort request.
  //
  // The embedder must provide a channel to communicate the token to each node.
  std::unordered_map<std::string, ReservedPort> reserved_ports_;

  // This tracks pending outgoing connection request for named ports.
  std::vector<PendingTokenConnection> pending_token_connections_;
  std::unordered_map<ports::PortName, base::Closure> pending_connection_acks_;

  DISALLOW_COPY_AND_ASSIGN(Node);
};

}  // namespace edk
}  // namespace mojo

#endif  // PORTS_MOJO_SYSTEM_NODE_H_