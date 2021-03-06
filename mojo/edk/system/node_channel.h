// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_EDK_SYSTEM_NODE_CHANNEL_H_
#define MOJO_EDK_SYSTEM_NODE_CHANNEL_H_

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task_runner.h"
#include "mojo/edk/embedder/platform_handle_vector.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "mojo/edk/system/channel.h"
#include "mojo/edk/system/ports/name.h"

namespace mojo {
namespace edk {

// Wraps a Channel to send and receive Node control messages.
class NodeChannel : public base::RefCountedThreadSafe<NodeChannel>,
                    public Channel::Delegate {
 public:
  class Delegate {
   public:
    virtual ~Delegate() {}
    virtual void OnAcceptChild(const ports::NodeName& from_node,
                               const ports::NodeName& parent_name,
                               const ports::NodeName& token) = 0;
    virtual void OnAcceptParent(const ports::NodeName& from_node,
                                const ports::NodeName& token,
                                const ports::NodeName& child_name) = 0;
    virtual void OnPortsMessage(
        const ports::NodeName& from_node,
        const void* payload,
        size_t payload_size,
        ScopedPlatformHandleVectorPtr platform_handles) = 0;
    virtual void OnRequestPortConnection(
        const ports::NodeName& from_node,
        const ports::PortName& connector_port_name,
        const std::string& token) = 0;
    virtual void OnConnectToPort(
        const ports::NodeName& from_node,
        const ports::PortName& connector_port_name,
        const ports::PortName& connectee_port_name) = 0;
    virtual void OnRequestIntroduction(const ports::NodeName& from_node,
                                       const ports::NodeName& name) = 0;
    virtual void OnIntroduce(const ports::NodeName& from_name,
                             const ports::NodeName& name,
                             ScopedPlatformHandle channel_handle) = 0;

    virtual void OnChannelError(const ports::NodeName& node) = 0;
  };

  static scoped_refptr<NodeChannel> Create(
      Delegate* delegate,
      ScopedPlatformHandle platform_handle,
      scoped_refptr<base::TaskRunner> io_task_runner);

  static Channel::MessagePtr CreatePortsMessage(
      size_t payload_size,
      void** payload,
      ScopedPlatformHandleVectorPtr platform_handles);

  // Start receiving messages.
  void Start();

  // Permanently stop the channel from sending or receiving messages.
  void ShutDown();

  // Used for context in Delegate calls (via |from_node| arguments.)
  void SetRemoteNodeName(const ports::NodeName& name);

  void AcceptChild(const ports::NodeName& parent_name,
                   const ports::NodeName& token);
  void AcceptParent(const ports::NodeName& token,
                    const ports::NodeName& child_name);
  void PortsMessage(Channel::MessagePtr message);
  void RequestPortConnection(const ports::PortName& connector_port_name,
                             const std::string& token);
  void ConnectToPort(const ports::PortName& connector_port_name,
                     const ports::PortName& connectee_port_name);
  void RequestIntroduction(const ports::NodeName& name);
  void Introduce(const ports::NodeName& name, ScopedPlatformHandle handle);

 private:
  friend class base::RefCountedThreadSafe<NodeChannel>;

  NodeChannel(Delegate* delegate,
              ScopedPlatformHandle platform_handle,
              scoped_refptr<base::TaskRunner> io_task_runner);
  ~NodeChannel() override;

  // Channel::Delegate:
  void OnChannelMessage(const void* payload,
                        size_t payload_size,
                        ScopedPlatformHandleVectorPtr handles) override;
  void OnChannelError() override;

  Delegate* const delegate_;
  const scoped_refptr<base::TaskRunner> io_task_runner_;

  base::Lock channel_lock_;
  scoped_refptr<Channel> channel_;

  // Must only be accessed from |io_task_runner_|'s thread.
  ports::NodeName remote_node_name_;

  DISALLOW_COPY_AND_ASSIGN(NodeChannel);
};

}  // namespace edk
}  // namespace mojo

#endif  // MOJO_EDK_SYSTEM_NODE_CHANNEL_H_
