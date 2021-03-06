// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/core.h"

#include <string.h>

#include <utility>

#include "base/bind.h"
#include "base/containers/stack_container.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/message_loop/message_loop.h"
#include "base/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "crypto/random.h"
#include "mojo/edk/embedder/embedder_internal.h"
#include "mojo/edk/system/async_waiter.h"
#include "mojo/edk/system/channel.h"
#include "mojo/edk/system/configuration.h"
#include "mojo/edk/system/data_pipe_consumer_dispatcher.h"
#include "mojo/edk/system/data_pipe_producer_dispatcher.h"
#include "mojo/edk/system/handle_signals_state.h"
#include "mojo/edk/system/message_pipe_dispatcher.h"
#include "mojo/edk/system/platform_handle_dispatcher.h"
#include "mojo/edk/system/ports/node.h"
#include "mojo/edk/system/shared_buffer_dispatcher.h"
#include "mojo/edk/system/wait_set_dispatcher.h"
#include "mojo/edk/system/waiter.h"

namespace mojo {
namespace edk {

namespace {

// This is an unnecessarily large limit that is relatively easy to enforce.
const uint32_t kMaxHandlesPerMessage = 1024 * 1024;

}  // namespace

Core::Core() : node_controller_(this) {}

Core::~Core() {}

void Core::SetIOTaskRunner(scoped_refptr<base::TaskRunner> io_task_runner) {
  node_controller_.SetIOTaskRunner(io_task_runner);
}

scoped_refptr<Dispatcher> Core::GetDispatcher(MojoHandle handle) {
  base::AutoLock lock(handles_lock_);
  return handles_.GetDispatcher(handle);
}

void Core::AddChild(ScopedPlatformHandle platform_handle) {
  node_controller_.ConnectToChild(std::move(platform_handle));
}

void Core::InitChild(ScopedPlatformHandle platform_handle) {
  node_controller_.ConnectToParent(std::move(platform_handle));
}

MojoHandle Core::AddDispatcher(scoped_refptr<Dispatcher> dispatcher) {
  base::AutoLock lock(handles_lock_);
  return handles_.AddDispatcher(dispatcher);
}

bool Core::AddDispatchersForReceivedPorts(const ports::Message& message,
                                          MojoHandle* handles) {
  std::vector<Dispatcher::DispatcherInTransit> dispatchers(message.num_ports());
  for (size_t i = 0; i < message.num_ports(); ++i) {
    ports::PortRef port;
    CHECK_EQ(ports::OK,
             node_controller_.node()->GetPort(message.ports()[i], &port));

    Dispatcher::DispatcherInTransit& d = dispatchers[i];
    d.dispatcher = new MessagePipeDispatcher(&node_controller_, port,
                                             true /* connected */);
  }
  return AddDispatchersFromTransit(dispatchers, handles);
}

bool Core::AddDispatchersFromTransit(
    const std::vector<Dispatcher::DispatcherInTransit>& dispatchers,
    MojoHandle* handles) {
  bool failed = false;
  {
    base::AutoLock lock(handles_lock_);
    if (!handles_.AddDispatchersFromTransit(dispatchers, handles))
      failed = true;
  }
  if (failed) {
    for (auto d : dispatchers)
      d.dispatcher->Close();
    return false;
  }
  return true;
}

MojoResult Core::CreatePlatformHandleWrapper(
    ScopedPlatformHandle platform_handle,
    MojoHandle* wrapper_handle) {
  MojoHandle h = AddDispatcher(
      PlatformHandleDispatcher::Create(std::move(platform_handle)));
  if (h == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  *wrapper_handle = h;
  return MOJO_RESULT_OK;
}

MojoResult Core::PassWrappedPlatformHandle(
    MojoHandle wrapper_handle,
    ScopedPlatformHandle* platform_handle) {
  base::AutoLock lock(handles_lock_);
  scoped_refptr<Dispatcher> d;
  MojoResult result = handles_.GetAndRemoveDispatcher(wrapper_handle, &d);
  if (result != MOJO_RESULT_OK)
    return result;
  PlatformHandleDispatcher* phd =
      static_cast<PlatformHandleDispatcher*>(d.get());
  *platform_handle = phd->PassPlatformHandle();
  phd->Close();
  return MOJO_RESULT_OK;
}

ScopedMessagePipeHandle Core::CreateParentMessagePipe(
    const std::string& token) {
  ports::PortRef port;
  node_controller_.ReservePort(token, &port);
  MojoHandle handle = AddDispatcher(
      new MessagePipeDispatcher(&node_controller_, port,
                                false /* connected */));
  return ScopedMessagePipeHandle(MessagePipeHandle(handle));
}

ScopedMessagePipeHandle Core::CreateChildMessagePipe(
    const std::string& token) {
  ports::PortRef port;
  node_controller_.node()->CreateUninitializedPort(&port);

  MojoHandle handle = AddDispatcher(
      new MessagePipeDispatcher(&node_controller_, port,
                                false /* connected */));

  // Note: It's important that we create the MPD before calling
  // ConnectToParentPort(), as the corresponding request and the parent's
  // response could otherwise race with MPD creation, and the pipe could miss
  // incoming messages.
  node_controller_.ConnectToParentPort(port, token);

  return ScopedMessagePipeHandle(MessagePipeHandle(handle));
}

MojoResult Core::AsyncWait(MojoHandle handle,
                           MojoHandleSignals signals,
                           const base::Callback<void(MojoResult)>& callback) {
  scoped_refptr<Dispatcher> dispatcher = GetDispatcher(handle);
  DCHECK(dispatcher);

  scoped_ptr<AsyncWaiter> waiter = make_scoped_ptr(new AsyncWaiter(callback));
  MojoResult rv = dispatcher->AddAwakable(waiter.get(), signals, 0, nullptr);
  if (rv == MOJO_RESULT_OK)
    ignore_result(waiter.release());
  return rv;
}

MojoTimeTicks Core::GetTimeTicksNow() {
  return base::TimeTicks::Now().ToInternalValue();
}

MojoResult Core::Close(MojoHandle handle) {
  scoped_refptr<Dispatcher> dispatcher;
  {
    base::AutoLock lock(handles_lock_);
    MojoResult rv = handles_.GetAndRemoveDispatcher(handle, &dispatcher);
    if (rv != MOJO_RESULT_OK)
      return rv;
  }
  dispatcher->Close();
  return MOJO_RESULT_OK;
}

MojoResult Core::Wait(MojoHandle handle,
                      MojoHandleSignals signals,
                      MojoDeadline deadline,
                      MojoHandleSignalsState* signals_state) {
  uint32_t unused = static_cast<uint32_t>(-1);
  HandleSignalsState hss;
  MojoResult rv = WaitManyInternal(&handle, &signals, 1, deadline, &unused,
                                   signals_state ? &hss : nullptr);
  if (rv != MOJO_RESULT_INVALID_ARGUMENT && signals_state)
    *signals_state = hss;
  return rv;
}

MojoResult Core::WaitMany(const MojoHandle* handles,
                          const MojoHandleSignals* signals,
                          uint32_t num_handles,
                          MojoDeadline deadline,
                          uint32_t* result_index,
                          MojoHandleSignalsState* signals_state) {
  if (num_handles < 1)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (num_handles > GetConfiguration().max_wait_many_num_handles)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  uint32_t index = static_cast<uint32_t>(-1);
  MojoResult rv;
  if (!signals_state) {
    rv = WaitManyInternal(handles, signals, num_handles, deadline, &index,
                          nullptr);
  } else {
    // Note: The |reinterpret_cast| is safe, since |HandleSignalsState| is a
    // subclass of |MojoHandleSignalsState| that doesn't add any data members.
    rv = WaitManyInternal(handles, signals, num_handles, deadline, &index,
                          reinterpret_cast<HandleSignalsState*>(signals_state));
  }
  if (index != static_cast<uint32_t>(-1) && result_index)
    *result_index = index;
  return rv;
}

MojoResult Core::CreateWaitSet(MojoHandle* wait_set_handle) {
  if (!wait_set_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<WaitSetDispatcher> dispatcher = new WaitSetDispatcher();
  MojoHandle h = AddDispatcher(dispatcher);
  if (h == MOJO_HANDLE_INVALID) {
    LOG(ERROR) << "Handle table full";
    dispatcher->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *wait_set_handle = h;
  return MOJO_RESULT_OK;
}

MojoResult Core::AddHandle(MojoHandle wait_set_handle,
                           MojoHandle handle,
                           MojoHandleSignals signals) {
  scoped_refptr<Dispatcher> wait_set_dispatcher(GetDispatcher(wait_set_handle));
  if (!wait_set_dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return wait_set_dispatcher->AddWaitingDispatcher(dispatcher, signals, handle);
}

MojoResult Core::RemoveHandle(MojoHandle wait_set_handle,
                              MojoHandle handle) {
  scoped_refptr<Dispatcher> wait_set_dispatcher(GetDispatcher(wait_set_handle));
  if (!wait_set_dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return wait_set_dispatcher->RemoveWaitingDispatcher(dispatcher);
}

MojoResult Core::GetReadyHandles(MojoHandle wait_set_handle,
                                 uint32_t* count,
                                 MojoHandle* handles,
                                 MojoResult* results,
                                 MojoHandleSignalsState* signals_states) {
  if (!handles || !count || !(*count) || !results)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> wait_set_dispatcher(GetDispatcher(wait_set_handle));
  if (!wait_set_dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  DispatcherVector awoken_dispatchers;
  base::StackVector<uintptr_t, 16> contexts;
  contexts->assign(*count, MOJO_HANDLE_INVALID);

  MojoResult result = wait_set_dispatcher->GetReadyDispatchers(
      count, &awoken_dispatchers, results, contexts->data());

  if (result == MOJO_RESULT_OK) {
    for (size_t i = 0; i < *count; i++) {
      handles[i] = static_cast<MojoHandle>(contexts[i]);
      if (signals_states)
        signals_states[i] = awoken_dispatchers[i]->GetHandleSignalsState();
    }
  }

  return result;
}

MojoResult Core::CreateMessagePipe(
    const MojoCreateMessagePipeOptions* options,
    MojoHandle* message_pipe_handle0,
    MojoHandle* message_pipe_handle1) {
  ports::PortRef port0, port1;
  node_controller_.node()->CreatePortPair(&port0, &port1);
  CHECK(message_pipe_handle0);
  CHECK(message_pipe_handle1);
  *message_pipe_handle0 = AddDispatcher(
      new MessagePipeDispatcher(&node_controller_, port0,
                                true /* connected */));
  if (*message_pipe_handle0 == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  *message_pipe_handle1 = AddDispatcher(
      new MessagePipeDispatcher(&node_controller_, port1,
                                true /* connected */));
  if (*message_pipe_handle1 == MOJO_HANDLE_INVALID) {
    scoped_refptr<Dispatcher> unused;
    unused->Close();
    handles_.GetAndRemoveDispatcher(*message_pipe_handle0, &unused);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::WriteMessage(MojoHandle message_pipe_handle,
                              const void* bytes,
                              uint32_t num_bytes,
                              const MojoHandle* handles,
                              uint32_t num_handles,
                              MojoWriteMessageFlags flags) {
  auto dispatcher = GetDispatcher(message_pipe_handle);
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  if (num_handles == 0)  // Fast path: no handles.
    return dispatcher->WriteMessage(bytes, num_bytes, nullptr, 0, flags);

  CHECK(handles);

  if (num_handles > kMaxHandlesPerMessage)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  for (size_t i = 0; i < num_handles; ++i) {
    if (message_pipe_handle == handles[i])
      return MOJO_RESULT_BUSY;
  }

  std::vector<Dispatcher::DispatcherInTransit> dispatchers;
  {
    base::AutoLock lock(handles_lock_);
    MojoResult rv = handles_.BeginTransit(handles, num_handles, &dispatchers);
    if (rv != MOJO_RESULT_OK) {
      handles_.CancelTransit(dispatchers);
      return rv;
    }
  }
  DCHECK_EQ(num_handles, dispatchers.size());

  MojoResult rv = dispatcher->WriteMessage(
      bytes, num_bytes, dispatchers.data(), num_handles, flags);

  {
    base::AutoLock lock(handles_lock_);
    if (rv == MOJO_RESULT_OK) {
      handles_.CompleteTransit(dispatchers);
    } else {
      handles_.CancelTransit(dispatchers);
    }
  }

  return rv;
}

MojoResult Core::ReadMessage(MojoHandle message_pipe_handle,
                             void* bytes,
                             uint32_t* num_bytes,
                             MojoHandle* handles,
                             uint32_t* num_handles,
                             MojoReadMessageFlags flags) {
  CHECK((!num_handles || !*num_handles || handles) &&
        (!num_bytes || !*num_bytes || bytes));
  auto dispatcher = GetDispatcher(message_pipe_handle);
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;
  return dispatcher->ReadMessage(bytes, num_bytes, handles, num_handles, flags);
}

MojoResult Core::CreateDataPipe(
    const MojoCreateDataPipeOptions* options,
    MojoHandle* data_pipe_producer_handle,
    MojoHandle* data_pipe_consumer_handle) {
  // TODO: Use the new data pipe impl when it's ready.

  MojoCreateDataPipeOptions default_options;
  default_options.struct_size = sizeof(MojoCreateDataPipeOptions);
  default_options.flags = 0;
  default_options.element_num_bytes = 1;
  default_options.capacity_num_bytes = 64 * 1024;

  const MojoCreateDataPipeOptions* create_options =
      options ? options : &default_options;

  ports::PortRef port0, port1;
  node_controller_.node()->CreatePortPair(&port0, &port1);
  CHECK(data_pipe_producer_handle);
  CHECK(data_pipe_consumer_handle);
  *data_pipe_producer_handle = AddDispatcher(
      new DataPipeProducerDispatcher(&node_controller_, port0,
                                     *create_options));
  if (*data_pipe_producer_handle == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  *data_pipe_consumer_handle = AddDispatcher(
      new DataPipeConsumerDispatcher(&node_controller_, port1,
                                     *create_options));
  if (*data_pipe_consumer_handle == MOJO_HANDLE_INVALID) {
    scoped_refptr<Dispatcher> unused;
    unused->Close();
    handles_.GetAndRemoveDispatcher(*data_pipe_producer_handle, &unused);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::WriteData(MojoHandle data_pipe_producer_handle,
                           const void* elements,
                           uint32_t* num_bytes,
                           MojoWriteDataFlags flags) {
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_producer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->WriteData(elements, num_bytes, flags);
}

MojoResult Core::BeginWriteData(MojoHandle data_pipe_producer_handle,
                                void** buffer,
                                uint32_t* buffer_num_bytes,
                                MojoWriteDataFlags flags) {
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_producer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->BeginWriteData(buffer, buffer_num_bytes, flags);
}

MojoResult Core::EndWriteData(MojoHandle data_pipe_producer_handle,
                              uint32_t num_bytes_written) {
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_producer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->EndWriteData(num_bytes_written);
}

MojoResult Core::ReadData(MojoHandle data_pipe_consumer_handle,
                          void* elements,
                          uint32_t* num_bytes,
                          MojoReadDataFlags flags) {
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_consumer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->ReadData(elements, num_bytes, flags);
}

MojoResult Core::BeginReadData(MojoHandle data_pipe_consumer_handle,
                               const void** buffer,
                               uint32_t* buffer_num_bytes,
                               MojoReadDataFlags flags) {
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_consumer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->BeginReadData(buffer, buffer_num_bytes, flags);
}

MojoResult Core::EndReadData(MojoHandle data_pipe_consumer_handle,
                             uint32_t num_bytes_read) {
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_consumer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->EndReadData(num_bytes_read);
}

MojoResult Core::CreateSharedBuffer(
    const MojoCreateSharedBufferOptions* options,
    uint64_t num_bytes,
    MojoHandle* shared_buffer_handle) {
  MojoCreateSharedBufferOptions validated_options = {};
  MojoResult result = SharedBufferDispatcher::ValidateCreateOptions(
      options, &validated_options);
  if (result != MOJO_RESULT_OK)
    return result;

  scoped_refptr<SharedBufferDispatcher> dispatcher;
  result = SharedBufferDispatcher::Create(
      internal::g_platform_support, validated_options, num_bytes, &dispatcher);
  if (result != MOJO_RESULT_OK) {
    DCHECK(!dispatcher);
    return result;
  }

  *shared_buffer_handle = AddDispatcher(dispatcher);
  if (*shared_buffer_handle == MOJO_HANDLE_INVALID) {
    LOG(ERROR) << "Handle table full";
    dispatcher->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::DuplicateBufferHandle(
    MojoHandle buffer_handle,
    const MojoDuplicateBufferHandleOptions* options,
    MojoHandle* new_buffer_handle) {
  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(buffer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  // Don't verify |options| here; that's the dispatcher's job.
  scoped_refptr<Dispatcher> new_dispatcher;
  MojoResult result =
      dispatcher->DuplicateBufferHandle(options, &new_dispatcher);
  if (result != MOJO_RESULT_OK)
    return result;

  *new_buffer_handle = AddDispatcher(new_dispatcher);
  if (*new_buffer_handle == MOJO_HANDLE_INVALID) {
    LOG(ERROR) << "Handle table full";
    dispatcher->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::MapBuffer(MojoHandle buffer_handle,
                           uint64_t offset,
                           uint64_t num_bytes,
                           void** buffer,
                           MojoMapBufferFlags flags) {
  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(buffer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_ptr<PlatformSharedBufferMapping> mapping;
  MojoResult result = dispatcher->MapBuffer(offset, num_bytes, flags, &mapping);
  if (result != MOJO_RESULT_OK)
    return result;

  DCHECK(mapping);
  void* address = mapping->GetBase();
  {
    base::AutoLock locker(mapping_table_lock_);
    result = mapping_table_.AddMapping(std::move(mapping));
  }
  if (result != MOJO_RESULT_OK)
    return result;

  *buffer = address;
  return MOJO_RESULT_OK;
}

MojoResult Core::UnmapBuffer(void* buffer) {
  base::AutoLock lock(mapping_table_lock_);
  return mapping_table_.RemoveMapping(buffer);
}

void Core::GetActiveHandlesForTest(std::vector<MojoHandle>* handles) {
  base::AutoLock lock(handles_lock_);
  handles_.GetActiveHandlesForTest(handles);
}

MojoResult Core::WaitManyInternal(const MojoHandle* handles,
                                  const MojoHandleSignals* signals,
                                  uint32_t num_handles,
                                  MojoDeadline deadline,
                                  uint32_t *result_index,
                                  HandleSignalsState* signals_states) {
  CHECK(handles);
  CHECK(signals);
  DCHECK_GT(num_handles, 0u);
  if (result_index) {
    DCHECK_EQ(*result_index, static_cast<uint32_t>(-1));
  }

  DispatcherVector dispatchers;
  dispatchers.reserve(num_handles);
  for (uint32_t i = 0; i < num_handles; i++) {
    scoped_refptr<Dispatcher> dispatcher = GetDispatcher(handles[i]);
    if (!dispatcher) {
      if (result_index)
        *result_index = i;
      return MOJO_RESULT_INVALID_ARGUMENT;
    }
    dispatchers.push_back(dispatcher);
  }

  // TODO(vtl): Should make the waiter live (permanently) in TLS.
  Waiter waiter;
  waiter.Init();

  uint32_t i;
  MojoResult rv = MOJO_RESULT_OK;
  for (i = 0; i < num_handles; i++) {
    rv = dispatchers[i]->AddAwakable(
        &waiter, signals[i], i, signals_states ? &signals_states[i] : nullptr);
    if (rv != MOJO_RESULT_OK) {
      if (result_index)
        *result_index = i;
      break;
    }
  }
  uint32_t num_added = i;

  if (rv == MOJO_RESULT_ALREADY_EXISTS) {
    rv = MOJO_RESULT_OK;  // The i-th one is already "triggered".
  } else if (rv == MOJO_RESULT_OK) {
    uintptr_t uintptr_result = *result_index;
    rv = waiter.Wait(deadline, &uintptr_result);
    *result_index = static_cast<uint32_t>(uintptr_result);
  }

  // Make sure no other dispatchers try to wake |waiter| for the current
  // |Wait()|/|WaitMany()| call. (Only after doing this can |waiter| be
  // destroyed, but this would still be required if the waiter were in TLS.)
  for (i = 0; i < num_added; i++) {
    dispatchers[i]->RemoveAwakable(
        &waiter, signals_states ? &signals_states[i] : nullptr);
  }
  if (signals_states) {
    for (; i < num_handles; i++)
      signals_states[i] = dispatchers[i]->GetHandleSignalsState();
  }

  return rv;
}

}  // namespace edk
}  // namespace mojo
