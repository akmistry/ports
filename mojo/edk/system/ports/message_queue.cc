// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/ports/message_queue.h"

#include <algorithm>

#include "base/logging.h"
#include "mojo/edk/system/ports/event.h"

namespace ports {

inline uint32_t GetSequenceNum(const ScopedMessage& message) {
  return GetEventData<UserEventData>(message)->sequence_num;
}

// Used by std::{push,pop}_heap functions
inline bool operator<(const ScopedMessage& a, const ScopedMessage& b) {
  return GetSequenceNum(a) > GetSequenceNum(b);
}

MessageQueue::MessageQueue() : MessageQueue(kInitialSequenceNum) {}

MessageQueue::MessageQueue(uint32_t next_sequence_num)
    : next_sequence_num_(next_sequence_num) {
  // The message queue is blocked waiting for a message with sequence number
  // equal to |next_sequence_num|.
}

MessageQueue::~MessageQueue() {
}

void MessageQueue::GetNextMessageIf(MessageSelector* selector,
                                    ScopedMessage* message) {
  if (heap_.empty() || GetSequenceNum(heap_[0]) != next_sequence_num_) {
    message->reset();
  } else {
    if (selector && !selector->Select(*heap_[0].get())) {
      message->reset();
      return;
    }

    std::pop_heap(heap_.begin(), heap_.end());
    *message = std::move(heap_.back());
    heap_.pop_back();

    next_sequence_num_++;
  }
}

void MessageQueue::AcceptMessage(ScopedMessage message,
                                 bool* has_next_message) {
  DCHECK(GetEventHeader(message)->type == EventType::kUser);

  // TODO: Handle sequence number roll-over.

  heap_.emplace_back(std::move(message));
  std::push_heap(heap_.begin(), heap_.end());

  if (!signalable_) {
    *has_next_message = false;
  } else {
    *has_next_message = (GetSequenceNum(heap_[0]) == next_sequence_num_);
  }
}

}  // namespace ports