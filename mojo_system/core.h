// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PORTS_MOJO_SYSTEM_CORE_H_
#define PORTS_MOJO_SYSTEM_CORE_H_

#include "base/callback.h"
#include "base/containers/hash_tables.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task_runner.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "mojo/edk/system/handle_signals_state.h"
#include "mojo/edk/system/system_impl_export.h"
#include "mojo/public/c/system/buffer.h"
#include "mojo/public/c/system/data_pipe.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/c/system/types.h"
#include "ports/mojo_system/dispatcher.h"

namespace mojo {
namespace edk {

class Node;

// |Core| is an object that implements the Mojo system calls. All public methods
// are thread-safe.
class MOJO_SYSTEM_IMPL_EXPORT Core {
 public:
  explicit Core();
  virtual ~Core();

  // Called exactly once, shortly after construction, and before any other
  // methods are called on this object.
  void SetIOTaskRunner(scoped_refptr<base::TaskRunner> io_task_runner);

  // Called in the parent process any time a new child is launched.
  void AddChild(ScopedPlatformHandle platform_handle);

  // Called in a child process exactly once during early initialization.
  void InitChild(ScopedPlatformHandle platform_handle);

  MojoHandle AddDispatcher(scoped_refptr<Dispatcher> dispatcher);

  // Watches on the given handle for the given signals, calling |callback| when
  // a signal is satisfied or when all signals become unsatisfiable. |callback|
  // must satisfy stringent requirements -- see |Awakable::Awake()| in
  // awakable.h. In particular, it must not call any Mojo system functions.
  MojoResult AsyncWait(MojoHandle handle,
                       MojoHandleSignals signals,
                       const base::Callback<void(MojoResult)>& callback);

  // ---------------------------------------------------------------------------

  // The following methods are essentially implementations of the Mojo Core
  // functions of the Mojo API, with the C interface translated to C++ by
  // "mojo/edk/embedder/entrypoints.cc". The best way to understand the contract
  // of these methods is to look at the header files defining the corresponding
  // API functions, referenced below.

  // These methods correspond to the API functions defined in
  // "mojo/public/c/system/functions.h":
  MojoTimeTicks GetTimeTicksNow();
  MojoResult Close(MojoHandle handle);
  MojoResult Wait(MojoHandle handle,
                  MojoHandleSignals signals,
                  MojoDeadline deadline,
                  MojoHandleSignalsState* signals_state);
  MojoResult WaitMany(const MojoHandle* handles,
                      const MojoHandleSignals* signals,
                      uint32_t num_handles,
                      MojoDeadline deadline,
                      uint32_t* result_index,
                      MojoHandleSignalsState* signals_states);

  // These methods correspond to the API functions defined in
  // "mojo/public/c/system/wait_set.h":
  MojoResult CreateWaitSet(MojoHandle* wait_set_handle);
  MojoResult AddHandle(MojoHandle wait_set_handle,
                       MojoHandle handle,
                       MojoHandleSignals signals);
  MojoResult RemoveHandle(MojoHandle wait_set_handle,
                          MojoHandle handle);
  MojoResult GetReadyHandles(MojoHandle wait_set_handle,
                             uint32_t* count,
                             MojoHandle* handles,
                             MojoResult* results,
                             MojoHandleSignalsState* signals_states);

  // These methods correspond to the API functions defined in
  // "mojo/public/c/system/message_pipe.h":
  MojoResult CreateMessagePipe(
      const MojoCreateMessagePipeOptions* options,
      MojoHandle* message_pipe_handle0,
      MojoHandle* message_pipe_handle1);
  MojoResult WriteMessage(MojoHandle message_pipe_handle,
                          const void* bytes,
                          uint32_t num_bytes,
                          const MojoHandle* handles,
                          uint32_t num_handles,
                          MojoWriteMessageFlags flags);
  MojoResult ReadMessage(MojoHandle message_pipe_handle,
                         void* bytes,
                         uint32_t* num_bytes,
                         MojoHandle* handles,
                         uint32_t* num_handles,
                         MojoReadMessageFlags flags);

  // These methods correspond to the API functions defined in
  // "mojo/public/c/system/data_pipe.h":
  MojoResult CreateDataPipe(
      const MojoCreateDataPipeOptions* options,
      MojoHandle* data_pipe_producer_handle,
      MojoHandle* data_pipe_consumer_handle);
  MojoResult WriteData(MojoHandle data_pipe_producer_handle,
                       const void* elements,
                       uint32_t* num_bytes,
                       MojoWriteDataFlags flags);
  MojoResult BeginWriteData(MojoHandle data_pipe_producer_handle,
                            void** buffer,
                            uint32_t* buffer_num_bytes,
                            MojoWriteDataFlags flags);
  MojoResult EndWriteData(MojoHandle data_pipe_producer_handle,
                          uint32_t num_bytes_written);
  MojoResult ReadData(MojoHandle data_pipe_consumer_handle,
                      void* elements,
                      uint32_t* num_bytes,
                      MojoReadDataFlags flags);
  MojoResult BeginReadData(MojoHandle data_pipe_consumer_handle,
                           const void** buffer,
                           uint32_t* buffer_num_bytes,
                           MojoReadDataFlags flags);
  MojoResult EndReadData(MojoHandle data_pipe_consumer_handle,
                         uint32_t num_bytes_read);

  // These methods correspond to the API functions defined in
  // "mojo/public/c/system/buffer.h":
  MojoResult CreateSharedBuffer(
      const MojoCreateSharedBufferOptions* options,
      uint64_t num_bytes,
      MojoHandle* shared_buffer_handle);
  MojoResult DuplicateBufferHandle(
      MojoHandle buffer_handle,
      const MojoDuplicateBufferHandleOptions* options,
      MojoHandle* new_buffer_handle);
  MojoResult MapBuffer(MojoHandle buffer_handle,
                       uint64_t offset,
                       uint64_t num_bytes,
                       void** buffer,
                       MojoMapBufferFlags flags);
  MojoResult UnmapBuffer(void* buffer);

 private:
  using DispatcherMap = base::hash_map<MojoHandle, scoped_refptr<Dispatcher>>;

  scoped_refptr<Dispatcher> GetDispatcher(MojoHandle handle);
  MojoResult WaitManyInternal(const MojoHandle* handles,
                              const MojoHandleSignals* signals,
                              uint32_t num_handles,
                              MojoDeadline deadline,
                              uint32_t *result_index,
                              HandleSignalsState* signals_states);

  scoped_refptr<base::TaskRunner> io_task_runner_;
  scoped_ptr<Node> node_;

  base::Lock dispatchers_lock_;
  MojoHandle next_handle_ = MOJO_HANDLE_INVALID + 1;
  DispatcherMap dispatchers_;

  DISALLOW_COPY_AND_ASSIGN(Core);
};

}  // namespace edk
}  // namespace mojo

#endif  // PORTS_MOJO_SYSTEM_CORE_H_