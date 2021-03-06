// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/ports/node.h"

#include <string.h>

#include "base/logging.h"
#include "mojo/edk/system/ports/node_delegate.h"

namespace mojo {
namespace edk {
namespace ports {

namespace {

int DebugError(const char* message, int error_code) {
  CHECK(false) << "Oops: " << message;
  return error_code;
}

#define OOPS(x) DebugError(#x, x)

bool CanAcceptMoreMessages(const Port* port) {
  // Have we already doled out the last message (i.e., do we expect to NOT
  // receive further messages)?
  uint64_t next_sequence_num = port->message_queue.next_sequence_num();
  if (port->peer_closed || port->remove_proxy_on_last_message) {
    if (port->last_sequence_num_to_receive == next_sequence_num - 1)
      return false;
  }
  return true;
}

}  // namespace

Node::Node(const NodeName& name, NodeDelegate* delegate)
    : name_(name),
      delegate_(delegate) {
}

Node::~Node() {
  if (!ports_.empty())
    DLOG(WARNING) << "Unclean shutdown for node " << name_;
}

int Node::GetPort(const PortName& port_name, PortRef* port_ref) {
  std::shared_ptr<Port> port = GetPort(port_name);
  if (!port)
    return ERROR_PORT_UNKNOWN;

  *port_ref = PortRef(port_name, std::move(port));
  return OK;
}

int Node::CreateUninitializedPort(PortRef* port_ref) {
  PortName port_name;
  delegate_->GenerateRandomPortName(&port_name);

  std::shared_ptr<Port> port = std::make_shared<Port>(kInitialSequenceNum,
                                                      kInitialSequenceNum);
  int rv = AddPortWithName(port_name, port);
  if (rv != OK)
    return rv;

  *port_ref = PortRef(port_name, std::move(port));
  return OK;
}

int Node::InitializePort(const PortRef& port_ref,
                         const NodeName& peer_node_name,
                         const PortName& peer_port_name) {
  Port* port = port_ref.port();

  {
    std::lock_guard<std::mutex> guard(port->lock);
    if (port->state != Port::kUninitialized)
      return ERROR_PORT_STATE_UNEXPECTED;

    port->state = Port::kReceiving;
    port->peer_node_name = peer_node_name;
    port->peer_port_name = peer_port_name;

    FlushOutgoingMessages_Locked(port);
  }

  delegate_->PortStatusChanged(port_ref);

  return OK;
}

int Node::CreatePortPair(PortRef* port0_ref, PortRef* port1_ref) {
  int rv;

  rv = CreateUninitializedPort(port0_ref);
  if (rv != OK)
    return rv;

  rv = CreateUninitializedPort(port1_ref);
  if (rv != OK)
    return rv;

  rv = InitializePort(*port0_ref, name_, port1_ref->name());
  if (rv != OK)
    return rv;

  rv = InitializePort(*port1_ref, name_, port0_ref->name());
  if (rv != OK)
    return rv;

  return OK;
}

int Node::SetUserData(const PortRef& port_ref,
                      std::shared_ptr<UserData> user_data) {
  Port* port = port_ref.port();

  std::lock_guard<std::mutex> guard(port->lock);
  if (port->state == Port::kClosed)
    return ERROR_PORT_STATE_UNEXPECTED;

  port->user_data = std::move(user_data);

  return OK;
}

int Node::GetUserData(const PortRef& port_ref,
                      std::shared_ptr<UserData>* user_data) {
  Port* port = port_ref.port();

  std::lock_guard<std::mutex> guard(port->lock);
  if (port->state == Port::kClosed)
    return ERROR_PORT_STATE_UNEXPECTED;

  *user_data = port->user_data;

  return OK;
}

int Node::ClosePort(const PortRef& port_ref) {
  ObserveClosureEventData data;
  NodeName peer_name;

  Port* port = port_ref.port();
  {
    std::lock_guard<std::mutex> guard(port->lock);
    if (port->state != Port::kReceiving)
      return ERROR_PORT_STATE_UNEXPECTED;

    port->state = Port::kClosed;

    // We pass along the sequence number of the last message sent from this
    // port to allow the peer to have the opportunity to consume all inbound
    // messages before notifying the embedder that this port is closed.
    data.last_sequence_num = port->next_sequence_num_to_send - 1;

    peer_name = port->peer_node_name;
  }

  delegate_->ForwardMessage(
      port->peer_node_name,
      NewInternalMessage(port->peer_port_name, EventType::kObserveClosure,
                         data));

  ErasePort(port_ref.name());
  return OK;
}

int Node::GetStatus(const PortRef& port_ref, PortStatus* port_status) {
  Port* port = port_ref.port();

  std::lock_guard<std::mutex> guard(port->lock);

  if (port->state != Port::kReceiving)
    return ERROR_PORT_STATE_UNEXPECTED;

  port_status->has_messages = port->message_queue.HasNextMessage();
  port_status->peer_closed = port->peer_closed;
  return OK;
}

int Node::GetMessage(const PortRef& port_ref, ScopedMessage* message) {
  return GetMessageIf(port_ref, nullptr, message);
}

int Node::GetMessageIf(const PortRef& port_ref,
                       std::function<bool(const Message&)> selector,
                       ScopedMessage* message) {
  *message = nullptr;

  DVLOG(1) << "GetMessageIf for " << port_ref.name() << "@" << name_;

  Port* port = port_ref.port();
  {
    std::lock_guard<std::mutex> guard(port->lock);

    // This could also be treated like the port being unknown since the
    // embedder should no longer be referring to a port that has been sent.
    if (port->state != Port::kReceiving)
      return ERROR_PORT_STATE_UNEXPECTED;

    // Let the embedder get messages until there are no more before reporting
    // that the peer closed its end.
    if (!CanAcceptMoreMessages(port))
      return ERROR_PORT_PEER_CLOSED;

    port->message_queue.GetNextMessageIf(selector, message);
  }

  // Allow referenced ports to trigger PortStatusChanged calls.
  if (*message) {
    for (size_t i = 0; i < (*message)->num_ports(); ++i) {
      const PortName& new_port_name = (*message)->ports()[i];
      std::shared_ptr<Port> new_port = GetPort(new_port_name);

      DCHECK(new_port) << "Port " << new_port_name << "@" << name_
                       << " does not exist!";

      std::lock_guard<std::mutex> guard(new_port->lock);

      DCHECK(new_port->state == Port::kReceiving);
      new_port->message_queue.set_signalable(true);
    }
  }

  return OK;
}

int Node::AllocMessage(size_t num_payload_bytes,
                       size_t num_ports,
                       ScopedMessage* message) {
  size_t num_header_bytes = sizeof(EventHeader) +
                            sizeof(UserEventData) +
                            num_ports * sizeof(PortDescriptor);
  size_t num_ports_bytes = num_ports * sizeof(PortName);

  delegate_->AllocMessage(
      num_header_bytes, num_payload_bytes, num_ports_bytes, message);

  memset((*message)->mutable_header_bytes(), 0, (*message)->num_header_bytes());

  GetMutableEventHeader(message->get())->type = EventType::kUser;
  GetMutableEventData<UserEventData>(message->get())->num_ports =
      static_cast<uint32_t>(num_ports);

  return OK;
}

int Node::SendMessage(const PortRef& port_ref, ScopedMessage message) {
  for (size_t i = 0; i < message->num_ports(); ++i) {
    if (message->ports()[i] == port_ref.name())
      return ERROR_PORT_CANNOT_SEND_SELF;
  }

  Port* port = port_ref.port();
  {
    std::lock_guard<std::mutex> guard(port->lock);

    if (port->state != Port::kReceiving && port->state != Port::kUninitialized)
      return ERROR_PORT_STATE_UNEXPECTED;

    if (port->state == Port::kReceiving && port->peer_closed)
      return ERROR_PORT_PEER_CLOSED;

    std::vector<std::shared_ptr<Port>> ports_taken;
    int rv = WillSendMessage_Locked(port, port_ref.name(), message.get(),
                                    &ports_taken);
    if (rv != OK)
      return rv;

    if (port->state == Port::kUninitialized) {
      port->outgoing_messages.emplace(std::move(message));
      std::copy(ports_taken.begin(), ports_taken.end(),
                std::back_inserter(port->outgoing_ports));
      return OK;
    }

    CHECK_EQ(port->state, Port::kReceiving);

    if (port->peer_node_name != name_) {
      delegate_->ForwardMessage(port->peer_node_name, std::move(message));
      return OK;
    }
  }

  bool deliver_local_messages = false;
  {
    std::lock_guard<std::mutex> guard(local_message_lock_);
    if (!is_delivering_local_messages_)
      deliver_local_messages = is_delivering_local_messages_ = true;
    local_messages_.emplace(std::move(message));
  }

  if (deliver_local_messages) {
    // Flush the local message queue. Note that any given call to AcceptMessage
    // may result in further calls to SendMessage. Re-entrancy into SendMessage
    // is allowed because we prevent re-entrancy into this specific code path.
    ports::ScopedMessage next_message;
    for(;;) {
      {
        std::lock_guard<std::mutex> guard(local_message_lock_);
        if (local_messages_.empty()) {
          is_delivering_local_messages_ = false;
          return OK;
        }

        next_message = std::move(local_messages_.front());
        local_messages_.pop();
      }

      int rv = AcceptMessage(std::move(next_message));
      if (rv != OK)
        return rv;
    }
  }

  return OK;
}

int Node::AcceptMessage(ScopedMessage message) {
  const EventHeader* header = GetEventHeader(*message);
  switch (header->type) {
    case EventType::kUser:
      return OnUserMessage(std::move(message));
    case EventType::kPortAccepted:
      return OnPortAccepted(header->port_name);
    case EventType::kObserveProxy:
      return OnObserveProxy(
          header->port_name,
          *GetEventData<ObserveProxyEventData>(*message));
    case EventType::kObserveProxyAck:
      return OnObserveProxyAck(
          header->port_name,
          GetEventData<ObserveProxyAckEventData>(*message)->last_sequence_num);
    case EventType::kObserveClosure:
      return OnObserveClosure(
          header->port_name,
          GetEventData<ObserveClosureEventData>(*message)->last_sequence_num);
  }
  return OOPS(ERROR_NOT_IMPLEMENTED);
}

int Node::LostConnectionToNode(const NodeName& node_name) {
  // We can no longer send events to the given node. We also can't expect any
  // PortAccepted events.

  DVLOG(1) << "Observing lost connection from node " << name_
           << " to node " << node_name;

  std::vector<PortRef> ports_to_notify;

  {
    std::lock_guard<std::mutex> guard(ports_lock_);

    for (auto iter = ports_.begin(); iter != ports_.end(); ) {
      std::shared_ptr<Port>& port = iter->second;

      bool remove_port = false;
      {
        std::lock_guard<std::mutex> port_guard(port->lock);

        if (port->peer_node_name == node_name) {
          // We can no longer send messages to this port's peer. We assume we
          // will not receive any more messages from this port's peer as well.
          if (!port->peer_closed) {
            port->peer_closed = true;
            port->last_sequence_num_to_receive =
                port->message_queue.next_sequence_num() - 1;

            if (port->state == Port::kReceiving)
              ports_to_notify.push_back(PortRef(iter->first, port));
          }

          // We do not expect to forward any further messages, and we do not
          // expect to receive a Port{Accepted,Rejected} event.
          if (port->state != Port::kReceiving)
            remove_port = true;
        }
      }

      if (remove_port) {
        DVLOG(1) << "Deleted port " << iter->first << "@" << name_;
        iter = ports_.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  for (size_t i = 0; i < ports_to_notify.size(); ++i)
    delegate_->PortStatusChanged(ports_to_notify[i]);

  return OK;
}

int Node::OnUserMessage(ScopedMessage message) {
  PortName port_name = GetEventHeader(*message)->port_name;
  const auto* event = GetEventData<UserEventData>(*message);

#ifndef NDEBUG
  std::ostringstream ports_buf;
  for (size_t i = 0; i < message->num_ports(); ++i) {
    if (i > 0)
      ports_buf << ",";
    ports_buf << message->ports()[i];
  }

  DVLOG(1) << "AcceptMessage " << event->sequence_num
             << " [ports=" << ports_buf.str() << "] at "
             << port_name << "@" << name_;
#endif

  std::shared_ptr<Port> port = GetPort(port_name);

  // Even if this port does not exist, cannot receive anymore messages or is
  // buffering or proxying messages, we still need these ports to be bound to
  // this node. When the message is forwarded, these ports will get transferred
  // following the usual method. If the message cannot be accepted, then the
  // newly bound ports will simply be closed.

  for (size_t i = 0; i < message->num_ports(); ++i) {
    int rv = AcceptPort(message->ports()[i], GetPortDescriptors(event)[i]);
    if (rv != OK)
      return rv;
  }

  bool has_next_message = false;
  bool message_accepted = false;

  if (port) {
    std::lock_guard<std::mutex> guard(port->lock);

    // Reject spurious messages if we've already received the last expected
    // message.
    if (CanAcceptMoreMessages(port.get())) {
      message_accepted = true;
      port->message_queue.AcceptMessage(std::move(message), &has_next_message);

      if (port->state == Port::kBuffering) {
        has_next_message = false;
      } else if (port->state == Port::kProxying) {
        has_next_message = false;

        // Forward messages. We forward messages in sequential order here so
        // that we maintain the message queue's notion of next sequence number.
        // That's useful for the proxy removal process as we can tell when this
        // port has seen all of the messages it is expected to see.
        int rv = ForwardMessages_Locked(port.get(), port_name);
        if (rv != OK)
          return rv;

        MaybeRemoveProxy_Locked(port.get(), port_name);
      }
    }
  }

  if (!message_accepted) {
    DVLOG(1) << "Message not accepted!\n";
    // Close all newly accepted ports as they are effectively orphaned.
    for (size_t i = 0; i < message->num_ports(); ++i) {
      PortRef port_ref;
      if (GetPort(message->ports()[i], &port_ref) == OK) {
        ClosePort(port_ref);
      } else {
        DLOG(WARNING) << "Cannot close non-existent port!\n";
      }
    }
  } else if (has_next_message) {
    PortRef port_ref(port_name, port);
    delegate_->PortStatusChanged(port_ref);
  }

  return OK;
}

int Node::OnPortAccepted(const PortName& port_name) {
  std::shared_ptr<Port> port = GetPort(port_name);
  if (!port)
    return OOPS(ERROR_PORT_UNKNOWN);

  {
    std::lock_guard<std::mutex> guard(port->lock);

    DVLOG(1) << "PortAccepted at " << port_name << "@" << name_
             << " pointing to "
             << port->peer_port_name << "@" << port->peer_node_name;

    if (port->state != Port::kBuffering)
      return OOPS(ERROR_PORT_STATE_UNEXPECTED);

    port->state = Port::kProxying;

    int rv = ForwardMessages_Locked(port.get(), port_name);
    if (rv != OK)
      return rv;

    // We may have observed closure before receiving PortAccepted. In that
    // case, we can advance to removing the proxy without sending out an
    // ObserveProxy message. We already know the last expected message, etc.

    if (port->remove_proxy_on_last_message) {
      MaybeRemoveProxy_Locked(port.get(), port_name);
    } else {
      InitiateProxyRemoval_Locked(port.get(), port_name);
    }
  }
  return OK;
}

int Node::OnObserveProxy(const PortName& port_name,
                         const ObserveProxyEventData& event) {
  // The port may have already been closed locally, in which case the
  // ObserveClosure message will contain the last_sequence_num field.
  // We can then silently ignore this message.
  std::shared_ptr<Port> port = GetPort(port_name);
  if (!port) {
    DVLOG(1) << "ObserveProxy: " << port_name << "@" << name_ << " not found";
    return OK;
  }

  DVLOG(1) << "ObserveProxy at " << port_name << "@" << name_ << ", proxy at "
           << event.proxy_port_name << "@"
           << event.proxy_node_name << " pointing to "
           << event.proxy_to_port_name << "@"
           << event.proxy_to_node_name;

  {
    std::lock_guard<std::mutex> guard(port->lock);

    if (port->peer_node_name == event.proxy_node_name &&
        port->peer_port_name == event.proxy_port_name) {
      if (port->state == Port::kReceiving) {
        port->peer_node_name = event.proxy_to_node_name;
        port->peer_port_name = event.proxy_to_port_name;

        ObserveProxyAckEventData ack;
        ack.last_sequence_num = port->next_sequence_num_to_send - 1;

        delegate_->ForwardMessage(
            event.proxy_node_name,
            NewInternalMessage(event.proxy_port_name,
                               EventType::kObserveProxyAck,
                               ack));
      } else {
        // As a proxy ourselves, we don't know how to honor the ObserveProxy
        // event or to populate the last_sequence_num field of ObserveProxyAck.
        // Afterall, another port could be sending messages to our peer now
        // that we've sent out our own ObserveProxy event.  Instead, we will
        // send an ObserveProxyAck indicating that the ObserveProxy event
        // should be re-sent (last_sequence_num set to kInvalidSequenceNum).
        // However, this has to be done after we are removed as a proxy.
        // Otherwise, we might just find ourselves back here again, which
        // would be akin to a busy loop.

        DVLOG(1) << "Delaying ObserveProxyAck to "
                 << event.proxy_port_name << "@" << event.proxy_node_name;

        ObserveProxyAckEventData ack;
        ack.last_sequence_num = kInvalidSequenceNum;

        port->send_on_proxy_removal.reset(
            new std::pair<NodeName, ScopedMessage>(
                event.proxy_node_name,
                NewInternalMessage(event.proxy_port_name,
                                   EventType::kObserveProxyAck,
                                   ack)));
      }
    } else {
      // Forward this event along to our peer. Eventually, it should find the
      // port referring to the proxy.
      delegate_->ForwardMessage(
          port->peer_node_name,
          NewInternalMessage(port->peer_port_name,
                             EventType::kObserveProxy,
                             event));
    }
  }
  return OK;
}

int Node::OnObserveProxyAck(const PortName& port_name,
                            uint64_t last_sequence_num) {
  DVLOG(1) << "ObserveProxyAck at " << port_name << "@" << name_
           << " (last_sequence_num=" << last_sequence_num << ")";

  std::shared_ptr<Port> port = GetPort(port_name);
  if (!port)
    return ERROR_PORT_UNKNOWN;  // The port may have observed closure first, so
                                // this is not an "Oops".

  {
    std::lock_guard<std::mutex> guard(port->lock);

    if (port->state != Port::kProxying)
      return OOPS(ERROR_PORT_STATE_UNEXPECTED);

    if (last_sequence_num == kInvalidSequenceNum) {
      // Send again.
      InitiateProxyRemoval_Locked(port.get(), port_name);
      return OK;
    }

    // We can now remove this port once we have received and forwarded the last
    // message addressed to this port.
    port->remove_proxy_on_last_message = true;
    port->last_sequence_num_to_receive = last_sequence_num;

    MaybeRemoveProxy_Locked(port.get(), port_name);
  }
  return OK;
}

int Node::OnObserveClosure(const PortName& port_name,
                           uint64_t last_sequence_num) {
  // OK if the port doesn't exist, as it may have been closed already.
  std::shared_ptr<Port> port = GetPort(port_name);
  if (!port)
    return OK;

  // This message tells the port that it should no longer expect more messages
  // beyond last_sequence_num. This message is forwarded along until we reach
  // the receiving end, and this message serves as an equivalent to
  // ObserveProxyAck.

  bool notify_delegate = false;
  {
    std::lock_guard<std::mutex> guard(port->lock);

    port->peer_closed = true;
    port->last_sequence_num_to_receive = last_sequence_num;

    DVLOG(1) << "ObserveClosure at " << port_name << "@" << name_
             << " (state=" << port->state << ") pointing to "
             << port->peer_port_name << "@" << port->peer_node_name
             << " (last_sequence_num=" << last_sequence_num << ")";

    if (port->state == Port::kReceiving) {
      notify_delegate = true;
    } else {
      NodeName next_node_name = port->peer_node_name;
      PortName next_port_name = port->peer_port_name;

      port->remove_proxy_on_last_message = true;

      // See about removing the port if it is a proxy as our peer won't be able
      // to participate in proxy removal.
      if (port->state == Port::kProxying) {
        MaybeRemoveProxy_Locked(port.get(), port_name);

        // Forward this event along.
        ObserveClosureEventData data;
        data.last_sequence_num = last_sequence_num;

        delegate_->ForwardMessage(
            next_node_name,
            NewInternalMessage(next_port_name,
                               EventType::kObserveClosure,
                               data));
      }
    }
  }
  if (notify_delegate) {
    PortRef port_ref(port_name, port);
    delegate_->PortStatusChanged(port_ref);
  }
  return OK;
}

int Node::AddPortWithName(const PortName& port_name,
                          const std::shared_ptr<Port>& port) {
  std::lock_guard<std::mutex> guard(ports_lock_);

  if (!ports_.insert(std::make_pair(port_name, port)).second)
    return OOPS(ERROR_PORT_EXISTS);  // Suggests a bad UUID generator.

  DVLOG(1) << "Created port " << port_name << "@" << name_;
  return OK;
}

void Node::ErasePort(const PortName& port_name) {
  std::lock_guard<std::mutex> guard(ports_lock_);

  ports_.erase(port_name);
  DVLOG(1) << "Deleted port " << port_name << "@" << name_;
}

std::shared_ptr<Port> Node::GetPort(const PortName& port_name) {
  std::lock_guard<std::mutex> guard(ports_lock_);

  auto iter = ports_.find(port_name);
  if (iter == ports_.end())
    return std::shared_ptr<Port>();

  return iter->second;
}

void Node::WillSendPort_Locked(Port* port,
                               const NodeName& to_node_name,
                               PortName* port_name,
                               PortDescriptor* port_descriptor) {
  PortName local_port_name = *port_name;

  PortName new_port_name;
  delegate_->GenerateRandomPortName(&new_port_name);

  // Make sure we don't send messages to the new peer until after we know it
  // exists. In the meantime, just buffer messages locally.
  DCHECK(port->state == Port::kReceiving);
  port->state = Port::kBuffering;

  *port_name = new_port_name;

  port_descriptor->peer_node_name = port->peer_node_name;
  port_descriptor->peer_port_name = port->peer_port_name;
  port_descriptor->referring_node_name = name_;
  port_descriptor->referring_port_name = local_port_name;
  port_descriptor->next_sequence_num_to_send = port->next_sequence_num_to_send;
  port_descriptor->next_sequence_num_to_receive =
      port->message_queue.next_sequence_num();

  // Configure the local port to point to the new port.
  port->peer_node_name = to_node_name;
  port->peer_port_name = new_port_name;
}

int Node::AcceptPort(const PortName& port_name,
                     const PortDescriptor& port_descriptor) {
  std::shared_ptr<Port> port =
      std::make_shared<Port>(port_descriptor.next_sequence_num_to_send,
                             port_descriptor.next_sequence_num_to_receive);
  port->state = Port::kReceiving;
  port->peer_node_name = port_descriptor.peer_node_name;
  port->peer_port_name = port_descriptor.peer_port_name;

  // A newly accepted port is not signalable until the message referencing the
  // new port finds its way to the consumer (see GetMessageIf).
  port->message_queue.set_signalable(false);

  int rv = AddPortWithName(port_name, port);
  if (rv != OK)
    return rv;

  // Allow referring port to forward messages.
  delegate_->ForwardMessage(
      port_descriptor.referring_node_name,
      NewInternalMessage(port_descriptor.referring_port_name,
                         EventType::kPortAccepted));
  return OK;
}

int Node::WillSendMessage_Locked(
    Port* port,
    const PortName& port_name,
    Message* message,
    std::vector<std::shared_ptr<Port>>* ports_taken) {
  DCHECK(message);

  // Messages may already have a sequence number if they're being forwarded
  // by a proxy. Otherwise, use the next outgoing sequence number.
  uint64_t* sequence_num =
      &GetMutableEventData<UserEventData>(message)->sequence_num;
  if (*sequence_num == 0)
    *sequence_num = port->next_sequence_num_to_send++;

#ifndef NDEBUG
  std::ostringstream ports_buf;
  for (size_t i = 0; i < message->num_ports(); ++i) {
    if (i > 0)
      ports_buf << ",";
    ports_buf << message->ports()[i];
  }
#endif

  if (message->num_ports() > 0) {
    // Note: Another thread could be trying to send the same ports, so we need
    // to ensure that they are ours to send before we mutate their state.

    std::vector<std::shared_ptr<Port>> ports;
    ports.resize(message->num_ports());
    if (ports_taken)
      ports_taken->resize(message->num_ports());

    {
      // Exclude other threads from locking multiple ports in arbitrary order.
      std::lock_guard<std::mutex> guard(send_with_ports_lock_);

      for (size_t i = 0; i < message->num_ports(); ++i) {
        ports[i] = GetPort(message->ports()[i]);
        if (ports_taken)
          ports_taken->at(i) = ports[i];
        ports[i]->lock.lock();

        int error = OK;
        if (ports[i]->state != Port::kReceiving)
          error = ERROR_PORT_STATE_UNEXPECTED;
        else if (message->ports()[i] == port->peer_port_name)
          error = ERROR_PORT_CANNOT_SEND_PEER;

        if (error != OK) {
          // Oops, we cannot send this port.
          for (size_t j = 0; j <= i; ++j)
            ports[i]->lock.unlock();
          // Backpedal on the sequence number.
          port->next_sequence_num_to_send--;
          return error;
        }
      }
    }

    PortDescriptor* port_descriptors =
        GetMutablePortDescriptors(GetMutableEventData<UserEventData>(message));

    for (size_t i = 0; i < message->num_ports(); ++i) {
      WillSendPort_Locked(ports[i].get(),
                          port->peer_node_name,
                          message->mutable_ports() + i,
                          port_descriptors + i);
    }

    for (size_t i = 0; i < message->num_ports(); ++i)
      ports[i]->lock.unlock();
  }

#ifndef NDEBUG
  DVLOG(1) << "Sending message "
           << GetEventData<UserEventData>(*message)->sequence_num
           << " [ports=" << ports_buf.str() << "]"
           << " from " << port_name << "@" << name_
           << " to " << port->peer_port_name << "@" << port->peer_node_name;
#endif

  GetMutableEventHeader(message)->port_name = port->peer_port_name;
  return OK;
}

int Node::ForwardMessages_Locked(Port* port, const PortName &port_name) {
  for (;;) {
    ScopedMessage message;
    port->message_queue.GetNextMessageIf(nullptr, &message);
    if (!message)
      break;

    int rv = WillSendMessage_Locked(port, port_name, message.get(), nullptr);
    if (rv != OK)
      return rv;

    delegate_->ForwardMessage(port->peer_node_name, std::move(message));
  }
  return OK;
}

void Node::InitiateProxyRemoval_Locked(Port* port,
                                       const PortName& port_name) {
  // To remove this node, we start by notifying the connected graph that we are
  // a proxy. This allows whatever port is referencing this node to skip it.
  // Eventually, this node will receive ObserveProxyAck (or ObserveClosure if
  // the peer was closed in the meantime).

  ObserveProxyEventData data;
  data.proxy_node_name = name_;
  data.proxy_port_name = port_name;
  data.proxy_to_node_name = port->peer_node_name;
  data.proxy_to_port_name = port->peer_port_name;

  delegate_->ForwardMessage(
      port->peer_node_name,
      NewInternalMessage(port->peer_port_name, EventType::kObserveProxy, data));
}

void Node::MaybeRemoveProxy_Locked(Port* port,
                                   const PortName& port_name) {
  DCHECK(port->state == Port::kProxying);

  // Make sure we have seen ObserveProxyAck before removing the port.
  if (!port->remove_proxy_on_last_message)
    return;

  if (!CanAcceptMoreMessages(port)) {
    // This proxy port is done. We can now remove it!
    ErasePort(port_name);

    if (port->send_on_proxy_removal) {
      NodeName to_node = port->send_on_proxy_removal->first;
      ScopedMessage& message = port->send_on_proxy_removal->second;

      delegate_->ForwardMessage(to_node, std::move(message));
    }
  } else {
    DVLOG(1) << "Cannot remove port " << port_name << "@" << name_
             << " now; waiting for more messages";
  }
}

void Node::FlushOutgoingMessages_Locked(Port* port) {
  DCHECK(port->peer_node_name != kInvalidNodeName);

  // Rewrite the peer node names for all ports that are about to start proxying.
  std::vector<std::shared_ptr<Port>> outgoing_ports;
  std::swap(outgoing_ports, port->outgoing_ports);
  for (const auto& outgoing_port : outgoing_ports)
    outgoing_port->peer_node_name = port->peer_node_name;

  while (!port->outgoing_messages.empty()) {
    ScopedMessage& message = port->outgoing_messages.front();

    // Rewrite the message destination port.
    EventHeader* header = GetMutableEventHeader(message.get());
    header->port_name = port->peer_port_name;

    DCHECK(header->type == EventType::kUser);

    delegate_->ForwardMessage(port->peer_node_name, std::move(message));
    port->outgoing_messages.pop();
  }
}

ScopedMessage Node::NewInternalMessage_Helper(const PortName& port_name,
                                              const EventType& type,
                                              const void* data,
                                              size_t num_data_bytes) {
  ScopedMessage message;
  delegate_->AllocMessage(sizeof(EventHeader) + num_data_bytes, 0, 0, &message);

  EventHeader* header = GetMutableEventHeader(message.get());
  header->port_name = port_name;
  header->type = type;
  header->padding = 0;

  if (num_data_bytes)
    memcpy(header + 1, data, num_data_bytes);

  return message;
}

}  // namespace ports
}  // namespace edk
}  // namespace mojo
