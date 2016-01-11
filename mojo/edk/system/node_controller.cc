// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/node_controller.h"

#include <algorithm>

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "crypto/random.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/system/core.h"
#include "mojo/edk/system/ports_message.h"

namespace mojo {
namespace edk {

namespace {

template <typename T>
void GenerateRandomName(T* out) { crypto::RandBytes(out, sizeof(T)); }

ports::NodeName GetRandomNodeName() {
  ports::NodeName name;
  GenerateRandomName(&name);
  return name;
}

}  // namespace

NodeController::~NodeController() {}

NodeController::NodeController(Core* core)
    : core_(core),
      name_(GetRandomNodeName()),
      node_(new ports::Node(name_, this)) {
  DVLOG(1) << "Initializing node " << name_;
}

void NodeController::SetIOTaskRunner(
    scoped_refptr<base::TaskRunner> task_runner) {
  io_task_runner_ = task_runner;
}

void NodeController::ConnectToChild(ScopedPlatformHandle platform_handle) {
  io_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&NodeController::ConnectToChildOnIOThread,
                 base::Unretained(this),
                 base::Passed(&platform_handle)));
}

void NodeController::ConnectToParent(ScopedPlatformHandle platform_handle) {
  io_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&NodeController::ConnectToParentOnIOThread,
                 base::Unretained(this),
                 base::Passed(&platform_handle)));
}

void NodeController::SetPortObserver(
    const ports::PortRef& port,
    std::shared_ptr<PortObserver> observer) {
  DCHECK(observer);
  node_->SetUserData(port, std::move(observer));
}

scoped_ptr<PortsMessage> NodeController::AllocMessage(size_t num_payload_bytes,
                                                      size_t num_ports) {
  ports::ScopedMessage m;
  int rv = node_->AllocMessage(num_payload_bytes, num_ports, &m);
  if (rv != ports::OK)
    return nullptr;
  DCHECK(m);

  return make_scoped_ptr(static_cast<PortsMessage*>(m.release()));
}

int NodeController::SendMessage(const ports::PortRef& port,
                                scoped_ptr<PortsMessage> message) {
  ports::ScopedMessage ports_message(message.release());
  return node_->SendMessage(port, std::move(ports_message));
}

void NodeController::InitializePortDeferred(
    const ports::PortRef& port_ref,
    const ports::NodeName& peer_node_name,
    const ports::PortName& peer_port_name) {
  {
    base::AutoLock lock(peers_lock_);
    if (peers_.find(peer_node_name) == peers_.end()) {
      DeferredPeerPort deferred_port;
      deferred_port.local_port = port_ref;
      deferred_port.remote_port = peer_port_name;
      pending_peer_ports_[peer_node_name].push_back(deferred_port);
      return;
    }
  }

  node_->InitializePort(port_ref, peer_node_name, peer_port_name);
}

void NodeController::ReservePortForToken(const ports::PortName& port_name,
                                         const std::string& token) {
  io_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&NodeController::ReservePortForTokenOnIOThread,
                 base::Unretained(this), port_name, token));
}

void NodeController::ConnectToParentPortByToken(
    const std::string& token,
    const ports::PortName& local_port) {
  io_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&NodeController::ConnectToParentPortByTokenOnIOThread,
                 base::Unretained(this), token, local_port));
}

void NodeController::ConnectToChildOnIOThread(
    ScopedPlatformHandle platform_handle) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  scoped_ptr<NodeChannel> channel(
      new NodeChannel(this, std::move(platform_handle), io_task_runner_));

  ports::NodeName token;
  GenerateRandomName(&token);

  channel->SetRemoteNodeName(token);
  channel->Start();
  channel->AcceptChild(name_, token);

  pending_children_.insert(std::make_pair(token, std::move(channel)));
}

void NodeController::ConnectToParentOnIOThread(
    ScopedPlatformHandle platform_handle) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  DCHECK(parent_name_ == ports::kInvalidNodeName);
  DCHECK(!bootstrap_channel_to_parent_);

  scoped_ptr<NodeChannel> channel(
      new NodeChannel(this, std::move(platform_handle), io_task_runner_));
  bootstrap_channel_to_parent_ = std::move(channel);
  bootstrap_channel_to_parent_->Start();
}

void NodeController::AddPeer(const ports::NodeName& name,
                             scoped_ptr<NodeChannel> channel,
                             bool start_channel) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  DCHECK(name != ports::kInvalidNodeName);
  DCHECK(channel);

  channel->SetRemoteNodeName(name);

  DeferredPeerPorts ports_to_initialize;
  {
    base::AutoLock lock(peers_lock_);
    if (peers_.find(name) != peers_.end()) {
      // This can happen normally if two nodes race to be introduced to each
      // other. The losing pipe will be silently closed and introduction should
      // not be affected.
      DVLOG(1) << "Ignoring duplicate peer name " << name;
      return;
    }

    DVLOG(1) << "Accepting new peer " << name << " on node " << name_;

    if (start_channel)
      channel->Start();

    OutgoingMessageQueue pending_messages;
    auto it = pending_peer_messages_.find(name);
    if (it != pending_peer_messages_.end()) {
      auto& message_queue = it->second;
      while (!message_queue.empty()) {
        ports::ScopedMessage message = std::move(message_queue.front());
        channel->PortsMessage(
            static_cast<PortsMessage*>(message.get())->TakeChannelMessage());
        message_queue.pop();
      }
      pending_peer_messages_.erase(it);
    }

    auto result = peers_.insert(std::make_pair(name, std::move(channel)));
    DCHECK(result.second);

    auto ports_iter = pending_peer_ports_.find(name);
    if (ports_iter != pending_peer_ports_.end()) {
      std::swap(ports_to_initialize, ports_iter->second);
      pending_peer_ports_.erase(ports_iter);
    }
  }

  for (const auto& port : ports_to_initialize)
    node_->InitializePort(port.local_port, name, port.remote_port);
}

void NodeController::DropPeer(const ports::NodeName& name) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  {
    base::AutoLock lock(peers_lock_);
    auto it = peers_.find(name);

    if (it != peers_.end()) {
      ports::NodeName peer = it->first;
      peers_.erase(it);
      DVLOG(1) << "Dropped peer " << peer;
    }

    pending_peer_messages_.erase(name);
    pending_children_.erase(name);
  }

  node_->LostConnectionToNode(name);
}

void NodeController::SendPeerMessage(const ports::NodeName& name,
                                     ports::ScopedMessage message) {
  base::AutoLock lock(peers_lock_);
  auto it = peers_.find(name);
  if (it != peers_.end()) {
    it->second->PortsMessage(
        static_cast<PortsMessage*>(message.get())->TakeChannelMessage());
    return;
  }

  if (parent_name_ == ports::kInvalidNodeName) {
    DVLOG(1) << "Dropping message for unknown peer: " << name;
    return;
  }

  // If we don't know who the peer is, queue the message for delivery and ask
  // our parent node to introduce us.
  auto& queue = pending_peer_messages_[name];
  if (queue.empty()) {
    auto it = peers_.find(parent_name_);
    DCHECK(it != peers_.end());
    it->second->RequestIntroduction(name);
  }
  queue.emplace(std::move(message));
}

void NodeController::ReservePortForTokenOnIOThread(
    const ports::PortName& port_name,
    const std::string& token) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  auto result = reserved_ports_.insert(std::make_pair(token, port_name));
  if (!result.second)
    DLOG(ERROR) << "Can't reserve port for duplicate token: " << token;
}

void NodeController::ConnectToParentPortByTokenOnIOThread(
    const std::string& token,
    const ports::PortName& local_port) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  if (parent_name_ != ports::kInvalidNodeName) {
    ConnectToParentPortByTokenNow(token, local_port);
  } else {
    auto result = pending_token_connections_.insert(
        std::make_pair(token, local_port));
    if (!result.second)
      DLOG(ERROR) << "Ignoring duplicate token connection request.";
  }
}

void NodeController::ConnectToParentPortByTokenNow(
    const std::string& token,
    const ports::PortName& local_port) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  DCHECK(parent_name_ != ports::kInvalidNodeName);

  base::AutoLock lock(peers_lock_);
  auto it = peers_.find(parent_name_);
  it->second->ConnectToPort(token, local_port);
}

void NodeController::AcceptIncomingMessages() {
  std::queue<ports::ScopedMessage> messages;
  {
    base::AutoLock lock(messages_lock_);
    std::swap(messages, incoming_messages_);
  }

  while (!messages.empty()) {
    node_->AcceptMessage(std::move(messages.front()));
    messages.pop();
  }
}

void NodeController::GenerateRandomPortName(ports::PortName* port_name) {
  GenerateRandomName(port_name);
}

void NodeController::AllocMessage(size_t num_header_bytes,
                                  size_t num_payload_bytes,
                                  size_t num_ports_bytes,
                                  ports::ScopedMessage* message) {
  message->reset(new PortsMessage(num_header_bytes, num_payload_bytes,
                                  num_ports_bytes, nullptr, 0, nullptr));
}

void NodeController::ForwardMessage(const ports::NodeName& node,
                                    ports::ScopedMessage message) {
  if (node == name_) {
    // NOTE: It isn't critical that we accept messages on the IO thread.
    // Rather, we just need to avoid re-entering the Node instance within
    // ForwardMessage.

    bool queue_was_empty = false;
    {
      base::AutoLock lock(messages_lock_);
      queue_was_empty = incoming_messages_.empty();
      incoming_messages_.emplace(std::move(message));
    }

    if (queue_was_empty) {
      io_task_runner_->PostTask(
          FROM_HERE,
          base::Bind(&NodeController::AcceptIncomingMessages,
                     base::Unretained(this)));
    }
  } else {
    SendPeerMessage(node, std::move(message));
  }
}

void NodeController::PortStatusChanged(const ports::PortRef& port) {
  std::shared_ptr<ports::UserData> user_data;
  node_->GetUserData(port, &user_data);

  PortObserver* observer = static_cast<PortObserver*>(user_data.get());
  if (observer)
    observer->OnPortStatusChanged();
}

void NodeController::OnAcceptChild(const ports::NodeName& from_node,
                                   const ports::NodeName& parent_name,
                                   const ports::NodeName& token) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  if (!bootstrap_channel_to_parent_) {
    DLOG(ERROR) << "Unexpected AcceptChild message from " << from_node;
    DropPeer(from_node);
    return;
  }

  parent_name_ = parent_name;
  bootstrap_channel_to_parent_->AcceptParent(token, name_);
  AddPeer(parent_name_, std::move(bootstrap_channel_to_parent_),
          false /* start_channel */);

  // Flush any pending token-based port connections.
  for (const auto& c : pending_token_connections_)
    ConnectToParentPortByTokenNow(c.first, c.second);

  DVLOG(1) << "Child " << name_ << " accepted parent " << parent_name;
}

void NodeController::OnAcceptParent(const ports::NodeName& from_node,
                                    const ports::NodeName& token,
                                    const ports::NodeName& child_name) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  scoped_ptr<NodeChannel> channel;

  auto it = pending_children_.find(from_node);
  if (it == pending_children_.end() || token != from_node) {
    DLOG(ERROR) << "Received unexpected AcceptParent message from "
                << from_node;
    DropPeer(from_node);
    return;
  }

  channel = std::move(it->second);
  pending_children_.erase(it);

  DCHECK(channel);

  AddPeer(child_name, std::move(channel), false /* start_channel */);
}

void NodeController::OnPortsMessage(
    const ports::NodeName& from_node,
    const void* bytes,
    size_t num_bytes,
    ScopedPlatformHandleVectorPtr platform_handles) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  size_t num_header_bytes, num_payload_bytes, num_ports_bytes;
  ports::Message::Parse(bytes,
                        num_bytes,
                        &num_header_bytes,
                        &num_payload_bytes,
                        &num_ports_bytes);

  ports::ScopedMessage message(
      new PortsMessage(num_header_bytes,
                       num_payload_bytes,
                       num_ports_bytes,
                       bytes,
                       num_bytes,
                       std::move(platform_handles)));
  node_->AcceptMessage(std::move(message));
}

void NodeController::OnConnectToPort(const ports::NodeName& from_node,
                                     const ports::PortName& connector_port_name,
                                     const std::string& token) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  // There are some fairly subtle issues here when establishing the new
  // connection.
  //
  // First, we can't hold |peers_lock_| when initializing the port, because it
  // causes the port to flush its outgoing message queue which may in turn
  // re-enter NodeController for e.g. SendPeerMessage.
  //
  // Second, the remote node must receive the ConnectToPortAck message *before*
  // our port flushes its queue, otherwise the remote node may be asked to
  // accept messages for an uninitialized port.
  //
  // So we send the ConnectToPortAck first, then initialize the local port. This
  // is safe because although the ConnectToPortAck may result in the peer
  // immediately sending messages to our uninitialized port, we'll receive them
  // on the I/O thread (this one) and they won't be processed until after this
  // function returns.

  ports::PortName parent_port_name;
  ports::PortRef parent_port;
  {
    base::AutoLock lock(peers_lock_);
    auto port_it = reserved_ports_.find(token);
    auto peer_it = peers_.find(from_node);
    if (port_it == reserved_ports_.end() || peer_it == peers_.end()) {
      base::AutoUnlock unlock(peers_lock_);
      DLOG(ERROR) << "Ignoring invalid ConnectToPort from node " << from_node
                  << " for token " << token;
      DropPeer(from_node);
      return;
    }

    parent_port_name = port_it->second;
    reserved_ports_.erase(port_it);

    CHECK_EQ(ports::OK, node_->GetPort(parent_port_name, &parent_port));

    peer_it->second->ConnectToPortAck(connector_port_name, parent_port_name);
  }

  CHECK_EQ(ports::OK, node_->InitializePort(parent_port, from_node,
                                            connector_port_name));
}

void NodeController::OnConnectToPortAck(
    const ports::NodeName& from_node,
    const ports::PortName& connector_port_name,
    const ports::PortName& connectee_port_name) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  if (from_node != parent_name_) {
    DLOG(ERROR) << "Ignoring ConnectToPortAck from non-parent node "
                << from_node;
    DropPeer(from_node);
    return;
  }

  ports::PortRef connector_port;
  CHECK_EQ(ports::OK, node_->GetPort(connector_port_name, &connector_port));

  CHECK_EQ(ports::OK, node_->InitializePort(connector_port, parent_name_,
                                            connectee_port_name));
}

void NodeController::OnRequestIntroduction(const ports::NodeName& from_node,
                                           const ports::NodeName& name) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  if (from_node == name || name == ports::kInvalidNodeName) {
    DLOG(ERROR) << "Rejecting invalid OnRequestIntroduction message from "
                << from_node;
    DropPeer(from_node);
    return;
  }

  base::AutoLock lock(peers_lock_);
  auto it = peers_.find(from_node);
  DCHECK(it != peers_.end());

  NodeChannel* requestor = it->second.get();
  it = peers_.find(name);
  if (it == peers_.end()) {
    requestor->Introduce(name, ScopedPlatformHandle());
  } else {
    PlatformChannelPair new_channel;
    requestor->Introduce(name, new_channel.PassServerHandle());
    it->second->Introduce(from_node, new_channel.PassClientHandle());
  }
}

void NodeController::OnIntroduce(const ports::NodeName& from_node,
                                 const ports::NodeName& name,
                                 ScopedPlatformHandle channel_handle) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  if (from_node != parent_name_) {
    DLOG(ERROR) << "Received unexpected Introduce message from node "
                << from_node;
    DropPeer(from_node);
    return;
  }

  if (!channel_handle.is_valid()) {
    DLOG(ERROR) << "Could not be introduced to peer " << name;
    base::AutoLock lock(peers_lock_);
    pending_peer_messages_.erase(name);
    return;
  }

  scoped_ptr<NodeChannel> channel(
      new NodeChannel(this, std::move(channel_handle), io_task_runner_));
  AddPeer(name, std::move(channel), true /* start_channel */);
}

void NodeController::OnChannelError(const ports::NodeName& from_node) {
  if (io_task_runner_->RunsTasksOnCurrentThread()) {
    DropPeer(from_node);
  } else {
    io_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&NodeController::DropPeer, base::Unretained(this), from_node));
  }
}

}  // namespace edk
}  // namespace mojo
