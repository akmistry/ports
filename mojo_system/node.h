// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PORTS_MOJO_SYSTEM_NODE_H_
#define PORTS_MOJO_SYSTEM_NODE_H_

#include <set>
#include <unordered_map>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task_runner.h"
#include "base/threading/thread.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "ports/include/ports.h"
#include "ports/mojo_system/node_channel.h"
#include "ports/mojo_system/node_controller.h"
#include "ports/src/hash_functions.h"

namespace mojo {
namespace edk {

class Core;

class Node : public ports::NodeDelegate, public NodeChannel::Delegate {
 public:
  class Observer {
   public:
    virtual ~Observer() {}

    // Notifies the observer that a new peer connection has been established.
    virtual void OnPeerAdded(const ports::NodeName& name) = 0;
  };

  class PortObserver {
   public:
    virtual ~PortObserver() {}

    // Notifies the observer that a message is available on a port.
    virtual void OnMessageAvailable(const ports::PortName& name,
                                    ports::ScopedMessage message) = 0;

    // Notifies the observer that a port's peer has been closed.
    virtual void OnPeerClosed(const ports::PortName& name) = 0;
  };

  // |core| owns and out-lives us.
  explicit Node(Core* core);
  ~Node() override;

  const ports::NodeName& name() const { return name_; }

  void set_controller(scoped_ptr<NodeController> controller) {
    controller_ = std::move(controller);
  }

  NodeController* controller() const { return controller_.get(); }

  // Adds or removes an observer on this Node. The observer must outlive the
  // Node or remove itself before dying.
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Connects this node to a via an OS pipe under |platform_handle|.
  // If |peer_name| is unknown, it should be set to |ports::kInvalidNodeName|.
  void ConnectToPeer(const ports::NodeName& peer_name,
                     ScopedPlatformHandle platform_handle,
                     const scoped_refptr<base::TaskRunner>& io_task_runner);

  // Indicates if a peer named |name| is already connected to this node.
  bool HasPeer(const ports::NodeName& name);

  // Registers a node named |name| with the given |channel|. |name| must be
  // a valid node name.
  void AddPeer(const ports::NodeName& name, scoped_ptr<NodeChannel> channel);

  // Drops the connection to peer named |name| if one exists.
  void DropPeer(const ports::NodeName& name);

  // Creates a single uninitialized port which is not ready for use.
  void CreateUninitializedPort(ports::PortName* port_name);

  // Initializes a previously uninitialized port with peer info.
  int InitializePort(const ports::PortName& port_name,
                     const ports::NodeName& peer_node_name,
                     const ports::PortName& peer_port_name);

  // Creates a new pair of local ports on this node, returning their names.
  void CreatePortPair(ports::PortName* port0, ports::PortName* port1);

  // Sets a port's observer.
  void SetPortObserver(const ports::PortName& port_name,
                       PortObserver* observer);

  // Sends a message on a port to its peer.
  int SendMessage(const ports::PortName& port_name,
                  ports::ScopedMessage message);

  // Closes a port.
  void ClosePort(const ports::PortName& port_name);

 private:
  // ports::NodeDelegate:
  void GenerateRandomPortName(ports::PortName* port_name) override;
  void SendEvent(const ports::NodeName& node, ports::Event event) override;
  void MessagesAvailable(const ports::PortName& port,
                         std::shared_ptr<ports::UserData> user_data) override;

  // NodeChannel::Delegate:
  void OnMessageReceived(const ports::NodeName& from_node,
                         NodeChannel::IncomingMessagePtr message) override;
  void OnChannelError(const ports::NodeName& from_node) override;

  void AcceptEventOnEventThread(ports::Event event);

  Core* core_;
  base::Thread event_thread_;

  // These are safe to access from any thread without locking as long as the
  // Node is alive.
  ports::NodeName name_;
  scoped_ptr<ports::Node> node_;

  base::Lock observers_lock_;
  std::set<Observer*> observers_;

  scoped_ptr<NodeController> controller_;

  base::Lock peers_lock_;
  std::unordered_map<ports::NodeName, scoped_ptr<NodeChannel>> peers_;

  DISALLOW_COPY_AND_ASSIGN(Node);
};

}  // namespace edk
}  // namespace mojo

#endif  // PORTS_MOJO_SYSTEM_NODE_H_
