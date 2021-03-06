// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/node_channel.h"

#include <cstring>
#include <limits>
#include <sstream>

#include "base/logging.h"
#include "mojo/edk/system/channel.h"

namespace mojo {
namespace edk {

namespace {

enum class MessageType : uint32_t {
  ACCEPT_CHILD,
  ACCEPT_PARENT,
  PORTS_MESSAGE,
  REQUEST_PORT_CONNECTION,
  CONNECT_TO_PORT,
  REQUEST_INTRODUCTION,
  INTRODUCE,
};

struct Header {
  MessageType type;
  uint32_t padding;
};

static_assert(sizeof(Header) % kChannelMessageAlignment == 0,
    "Invalid header size.");

struct AcceptChildData {
  ports::NodeName parent_name;
  ports::NodeName token;
};

struct AcceptParentData {
  ports::NodeName token;
  ports::NodeName child_name;
};

// This is followed by arbitrary payload data which is interpreted as a token
// string for port location.
struct RequestPortConnectionData {
  ports::PortName connector_port_name;
};

struct ConnectToPortData {
  ports::PortName connector_port_name;
  ports::PortName connectee_port_name;
};

// Used for both REQUEST_INTRODUCTION and INTRODUCE.
//
// For INTRODUCE the message must also include a platform handle the recipient
// can use to communicate with the named node. If said handle is omitted, the
// peer cannot be introduced.
struct IntroductionData {
  ports::NodeName name;
};

template <typename DataType>
Channel::MessagePtr CreateMessage(MessageType type,
                                  size_t payload_size,
                                  ScopedPlatformHandleVectorPtr handles,
                                  DataType** out_data) {
  Channel::MessagePtr message(
      new Channel::Message(sizeof(Header) + payload_size, std::move(handles)));
  Header* header = reinterpret_cast<Header*>(message->mutable_payload());
  header->type = type;
  header->padding = 0;
  *out_data = reinterpret_cast<DataType*>(&header[1]);
  return message;
};

template <typename DataType>
void GetMessagePayload(const void* bytes, DataType** out_data) {
  *out_data = reinterpret_cast<const DataType*>(
      static_cast<const char*>(bytes) + sizeof(Header));
}

}  // namespace

// static
scoped_refptr<NodeChannel> NodeChannel::Create(
    Delegate* delegate,
    ScopedPlatformHandle platform_handle,
    scoped_refptr<base::TaskRunner> io_task_runner) {
  return new NodeChannel(delegate, std::move(platform_handle), io_task_runner);
}

// static
Channel::MessagePtr NodeChannel::CreatePortsMessage(
    size_t payload_size,
    void** payload,
    ScopedPlatformHandleVectorPtr platform_handles) {
  return CreateMessage(MessageType::PORTS_MESSAGE, payload_size,
                       std::move(platform_handles), payload);
}

void NodeChannel::Start() {
  base::AutoLock lock(channel_lock_);
  DCHECK(channel_);
  channel_->Start();
}

void NodeChannel::ShutDown() {
  base::AutoLock lock(channel_lock_);
  if (channel_) {
    channel_->ShutDown();
    channel_ = nullptr;
  }
}

void NodeChannel::SetRemoteNodeName(const ports::NodeName& name) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  remote_node_name_ = name;
}

void NodeChannel::AcceptChild(const ports::NodeName& parent_name,
                              const ports::NodeName& token) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending AcceptChild on closed Channel.";
    return;
  }

  AcceptChildData* data;
  Channel::MessagePtr message = CreateMessage(
      MessageType::ACCEPT_CHILD, sizeof(AcceptChildData), nullptr, &data);
  data->parent_name = parent_name;
  data->token = token;
  channel_->Write(std::move(message));
}

void NodeChannel::AcceptParent(const ports::NodeName& token,
                               const ports::NodeName& child_name) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending AcceptParent on closed Channel.";
    return;
  }

  AcceptParentData* data;
  Channel::MessagePtr message = CreateMessage(
      MessageType::ACCEPT_PARENT, sizeof(AcceptParentData), nullptr, &data);
  data->token = token;
  data->child_name = child_name;
  channel_->Write(std::move(message));
}

void NodeChannel::PortsMessage(Channel::MessagePtr message) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending PortsMessage on closed Channel.";
    return;
  }

  channel_->Write(std::move(message));
}

void NodeChannel::RequestPortConnection(
    const ports::PortName& connector_port_name,
    const std::string& token) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending RequestPortConnection on closed Channel.";
    return;
  }

  RequestPortConnectionData* data;
  Channel::MessagePtr message = CreateMessage(
      MessageType::REQUEST_PORT_CONNECTION,
      sizeof(RequestPortConnectionData) + token.size(), nullptr, &data);
  data->connector_port_name = connector_port_name;
  memcpy(data + 1, token.data(), token.size());
  channel_->Write(std::move(message));
}

void NodeChannel::ConnectToPort(const ports::PortName& connector_port_name,
                                const ports::PortName& connectee_port_name) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending ConnectToPort on closed Channel.";
    return;
  }

  ConnectToPortData* data;
  Channel::MessagePtr message = CreateMessage(
      MessageType::CONNECT_TO_PORT, sizeof(ConnectToPortData), nullptr, &data);
  data->connector_port_name = connector_port_name;
  data->connectee_port_name = connectee_port_name;
  channel_->Write(std::move(message));
}

void NodeChannel::RequestIntroduction(const ports::NodeName& name) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending RequestIntroduction on closed Channel.";
    return;
  }

  IntroductionData* data;
  Channel::MessagePtr message = CreateMessage(
      MessageType::REQUEST_INTRODUCTION, sizeof(IntroductionData), nullptr,
      &data);
  data->name = name;
  channel_->Write(std::move(message));
}

void NodeChannel::Introduce(const ports::NodeName& name,
                            ScopedPlatformHandle handle) {
  base::AutoLock lock(channel_lock_);
  if (!channel_) {
    DVLOG(2) << "Not sending Introduce on closed Channel.";
    return;
  }

  ScopedPlatformHandleVectorPtr handles;
  if (handle.is_valid()) {
    handles.reset(new PlatformHandleVector(1));
    handles->at(0) = handle.release();
  }
  IntroductionData* data;
  Channel::MessagePtr message = CreateMessage(
      MessageType::INTRODUCE, sizeof(IntroductionData), std::move(handles),
      &data);
  data->name = name;
  channel_->Write(std::move(message));
}

NodeChannel::NodeChannel(Delegate* delegate,
                         ScopedPlatformHandle platform_handle,
                         scoped_refptr<base::TaskRunner> io_task_runner)
    : delegate_(delegate),
      io_task_runner_(io_task_runner),
      channel_(
          Channel::Create(this, std::move(platform_handle), io_task_runner_)) {
}

NodeChannel::~NodeChannel() {
  ShutDown();
}

void NodeChannel::OnChannelMessage(const void* payload,
                                   size_t payload_size,
                                   ScopedPlatformHandleVectorPtr handles) {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  const Header* header = static_cast<const Header*>(payload);
  switch (header->type) {
    case MessageType::ACCEPT_CHILD: {
      const AcceptChildData* data;
      GetMessagePayload(payload, &data);
      delegate_->OnAcceptChild(remote_node_name_, data->parent_name,
                               data->token);
      break;
    }

    case MessageType::ACCEPT_PARENT: {
      const AcceptParentData* data;
      GetMessagePayload(payload, &data);
      delegate_->OnAcceptParent(remote_node_name_, data->token,
                                data->child_name);
      break;
    }

    case MessageType::PORTS_MESSAGE: {
      const void* data;
      GetMessagePayload(payload, &data);
      delegate_->OnPortsMessage(remote_node_name_, data,
                                payload_size - sizeof(Header),
                                std::move(handles));
      break;
    }

    case MessageType::REQUEST_PORT_CONNECTION: {
      const RequestPortConnectionData* data;
      GetMessagePayload(payload, &data);

      const char* token_data = reinterpret_cast<const char*>(data + 1);
      const size_t token_size = payload_size - sizeof(*data) - sizeof(Header);
      std::string token(token_data, token_size);

      delegate_->OnRequestPortConnection(remote_node_name_,
                                         data->connector_port_name, token);
      break;
    }

    case MessageType::CONNECT_TO_PORT: {
      const ConnectToPortData* data;
      GetMessagePayload(payload, &data);
      delegate_->OnConnectToPort(remote_node_name_, data->connector_port_name,
                                 data->connectee_port_name);
      break;
    }

    case MessageType::REQUEST_INTRODUCTION: {
      const IntroductionData* data;
      GetMessagePayload(payload, &data);
      delegate_->OnRequestIntroduction(remote_node_name_, data->name);
      break;
    }

    case MessageType::INTRODUCE: {
      const IntroductionData* data;
      GetMessagePayload(payload, &data);
      ScopedPlatformHandle handle;
      if (handles && !handles->empty()) {
        handle = ScopedPlatformHandle(handles->at(0));
        handles->clear();
      }
      delegate_->OnIntroduce(remote_node_name_, data->name, std::move(handle));
      break;
    }

    default:
      DLOG(ERROR) << "Received unknown message type "
                  << static_cast<uint32_t>(header->type) << " from node "
                  << remote_node_name_;
      delegate_->OnChannelError(remote_node_name_);
      break;
  }
}

void NodeChannel::OnChannelError() {
  DCHECK(io_task_runner_->RunsTasksOnCurrentThread());

  ShutDown();
  delegate_->OnChannelError(remote_node_name_);
}

}  // namespace edk
}  // namespace mojo
