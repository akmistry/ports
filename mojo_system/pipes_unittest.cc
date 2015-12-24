// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>

#include "base/logging.h"
#include "mojo/public/c/system/types.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "ports/mojo_system/multiprocess_test_base.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mojo {
namespace edk {
namespace {

using PipesTest = test::MultiprocessTestBase;

// Echos the primordial channel until "exit".
DEFINE_TEST_CLIENT_WITH_PIPE(ChannelEchoClient, h) {
  for (;;) {
    std::string message = ReadString(h);
    if (message == "exit")
      break;
    WriteString(h, message);
  }
  return 0;
}

// Receives a pipe handle from the primordial channel and echos on it until
// "exit". Used to test simple pipe transfer across processes via channels.
DEFINE_TEST_CLIENT_WITH_PIPE(EchoServiceClient, h) {
  MojoHandle p;
  ReadStringWithHandles(h, &p, 1);
  for (;;) {
    std::string message = ReadString(p);
    if (message == "exit")
      break;
    WriteString(p, message);
  }
  return 0;
}

// Receives a pipe handle from the primordial channel and reads new handles
// from it. Each read handle establishes a new echo channel.
DEFINE_TEST_CLIENT_WITH_PIPE(EchoServiceFactoryClient, h) {
  MojoHandle p;
  ReadStringWithHandles(h, &p, 1);

  std::vector<MojoHandle> handles(2);
  handles[0] = h;
  handles[1] = p;
  std::vector<MojoHandleSignals> signals(2, MOJO_HANDLE_SIGNAL_READABLE);
  for (;;) {
    uint32_t index;
    CHECK_EQ(MojoWaitMany(handles.data(), signals.data(), handles.size(),
                          MOJO_DEADLINE_INDEFINITE, &index, nullptr),
             MOJO_RESULT_OK);
    DCHECK_LE(index, handles.size());
    if (index == 0) {
      // If data is available on the first pipe, it should be an exit command.
      EXPECT_EQ(std::string("exit"), ReadString(h));
      break;
    } else if (index == 1) {
      // If the second pipe, it should be a new handle requesting echo service.
      MojoHandle echo_request;
      ReadStringWithHandles(p, &echo_request, 1);
      handles.push_back(echo_request);
      signals.push_back(MOJO_HANDLE_SIGNAL_READABLE);
    } else {
      // Otherwise it was one of our established echo pipes. Echo!
      WriteString(handles[index], ReadString(handles[index]));
    }
  }
  return 0;
}

TEST_F(PipesTest, CreateMessagePipe) {
  CREATE_PIPE(p0, p1);
  VerifyTransmission(p0, p1, "hey man");
  VerifyTransmission(p1, p0, "slow down");
  VerifyTransmission(p0, p1, std::string(10 * 1024 * 1024, 'a'));
  VerifyTransmission(p1, p0, std::string(10 * 1024 * 1024, 'e'));
}

TEST_F(PipesTest, PassMessagePipeLocal) {
  CREATE_PIPE(p0, p1);
  VerifyTransmission(p0, p1, "testing testing");
  VerifyTransmission(p1, p0, "one two three");

  CREATE_PIPE(p2, p3);
  VerifyTransmission(p2, p3, "testing testing");
  VerifyTransmission(p3, p2, "one two three");

  // Pass p2 over p0 to p1.
  const std::string message = "ceci n'est pas une pipe";
  WriteStringWithHandles(p0, message, &p2, 1);
  EXPECT_EQ(message, ReadStringWithHandles(p1, &p2, 1));

  // Verify that the received handle (now in p2) still works.
  VerifyTransmission(p2, p3, "Easy come, easy go; will you let me go?");
  VerifyTransmission(p3, p2, "Bismillah! NO! We will not let you go!");
}

TEST_F(PipesTest, MultiprocessChannelPipe) {
  RUN_WITH_CHILD(ChannelEchoClient)
  ON_PIPE(h)
    VerifyEcho(h, "in an interstellar burst");
    VerifyEcho(h, "i am back to save the universe");
    VerifyEcho(h, std::string(10 * 1024 * 1024, 'o'));

    WriteString(h, "exit");
  END_CHILD()
}

TEST_F(PipesTest, PassMessagePipeCrossProcess) {
  RUN_WITH_CHILD(EchoServiceClient)
  ON_PIPE(h)
    CREATE_PIPE(p0, p1);

    // Pass one end of the pipe to the other process.
    WriteStringWithHandles(h, "here take this", &p1, 1);

    VerifyEcho(p0, "and you may ask yourself");
    VerifyEcho(p0, "where does that highway go?");
    VerifyEcho(p0, std::string(20 * 1024 * 1024, 'i'));

    WriteString(p0, "exit");
  END_CHILD()
}

TEST_F(PipesTest, PassMoarMessagePipesCrossProcess) {
  RUN_WITH_CHILD(EchoServiceFactoryClient)
  ON_PIPE(h)
    CREATE_PIPE(echo_factory_proxy, echo_factory_request);
    WriteStringWithHandles(
        h, "gief factory naow plz", &echo_factory_request, 1);

    CREATE_PIPE(echo_proxy_a, echo_request_a);
    CREATE_PIPE(echo_proxy_b, echo_request_b);

    WriteStringWithHandles(echo_factory_proxy, "give me an echo service plz!",
                           &echo_request_a, 1);
    WriteStringWithHandles(echo_factory_proxy, "give me one too!",
                           &echo_request_b, 1);

    VerifyEcho(echo_proxy_a, "i came here for an argument");
    VerifyEcho(echo_proxy_a, "shut your festering gob");
    VerifyEcho(echo_proxy_a, "mumble mumble mumble");

    VerifyEcho(echo_proxy_b, "wubalubadubdub");
    VerifyEcho(echo_proxy_b, "wubalubadubdub");

    CREATE_PIPE(echo_proxy_c, echo_request_c);

    WriteStringWithHandles(echo_factory_proxy, "hook me up also thanks",
                           &echo_request_c, 1);

    VerifyEcho(echo_proxy_a, "the frobinators taste like frobinators");
    VerifyEcho(echo_proxy_b, "beep bop boop");
    VerifyEcho(echo_proxy_c, "zzzzzzzzzzzzzzzzzzzzzzzzzz");

    WriteString(h, "exit");
  END_CHILD()
}

TEST_F(PipesTest, ChannelPipesWithMultipleChildren) {
  RUN_WITH_CHILDREN("ChannelEchoClient", "ChannelEchoClient")
  ON_PIPES(pipes)
    VerifyEcho(pipes[0], "hello child 0");
    VerifyEcho(pipes[1], "hello child 1");

    WriteString(pipes[0], "exit");
    WriteString(pipes[1], "exit");
  END_CHILDREN()
}

}  // namespace
}  // namespace edk
}  // namespace mojo
