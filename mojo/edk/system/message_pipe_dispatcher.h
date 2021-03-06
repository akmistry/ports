// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_EDK_SYSTEM_MESSAGE_PIPE_DISPATCHER_H_
#define MOJO_EDK_SYSTEM_MESSAGE_PIPE_DISPATCHER_H_

#include <queue>

#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "mojo/edk/system/awakable_list.h"
#include "mojo/edk/system/dispatcher.h"
#include "mojo/edk/system/ports/port_ref.h"

namespace mojo {
namespace edk {

class NodeController;
class PortsMessage;

class MessagePipeDispatcher : public Dispatcher {
 public:
  // Constructs a MessagePipeDispatcher permanently tied to a specific port.
  // |connected| must indicate the state of the port at construction time; if
  // the port is initialized with a peer, |connected| must be true. Otherwise it
  // must be false.
  //
  // A MessagePipeDispatcher may not be transferred while in a disconnected
  // state, and one can never return to a disconnected once connected.
  MessagePipeDispatcher(NodeController* node_controller,
                        const ports::PortRef& port,
                        bool connected);

  // Dispatcher:
  Type GetType() const override;
  MojoResult Close() override;
  MojoResult WriteMessage(const void* bytes,
                          uint32_t num_bytes,
                          const DispatcherInTransit* dispatchers,
                          uint32_t num_dispatchers,
                          MojoWriteMessageFlags flags) override;
  MojoResult ReadMessage(void* bytes,
                         uint32_t* num_bytes,
                         MojoHandle* handles,
                         uint32_t* num_handles,
                         MojoReadMessageFlags flags) override;
  HandleSignalsState GetHandleSignalsState() const override;
  MojoResult AddAwakable(Awakable* awakable,
                         MojoHandleSignals signals,
                         uintptr_t context,
                         HandleSignalsState* signals_state) override;
  void RemoveAwakable(Awakable* awakable,
                      HandleSignalsState* signals_state) override;
  void StartSerialize(uint32_t* num_bytes,
                      uint32_t* num_ports,
                      uint32_t* num_handles) override;
  bool EndSerializeAndClose(void* destination,
                            ports::PortName* ports,
                            PlatformHandleVector* handles) override;
  bool BeginTransit() override;
  void CompleteTransit() override;
  void CancelTransit() override;

  static scoped_refptr<Dispatcher> Deserialize(
      const void* data,
      size_t num_bytes,
      const ports::PortName* ports,
      size_t num_ports,
      PlatformHandle* handles,
      size_t num_handles);

 private:
  class PortObserverThunk;
  friend class PortObserverThunk;

  ~MessagePipeDispatcher() override;

  MojoResult CloseNoLock();
  HandleSignalsState GetHandleSignalsStateNoLock() const;
  void OnPortStatusChanged();

  // These are safe to access from any thread without locking.
  NodeController* const node_controller_;
  const ports::PortRef port_;

  // Guards access to all the fields below.
  mutable base::Lock signal_lock_;

  bool port_connected_ = false;
  bool port_transferred_ = false;
  bool port_closed_ = false;
  AwakableList awakables_;

  DISALLOW_COPY_AND_ASSIGN(MessagePipeDispatcher);
};

}  // namespace edk
}  // namespace mojo

#endif  // MOJO_EDK_SYSTEM_MESSAGE_PIPE_DISPATCHER_H_
