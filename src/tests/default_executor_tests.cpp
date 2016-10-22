// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <set>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/hashset.hpp>

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Mesos;

using process::Future;
using process::Owned;

using process::http::OK;
using process::http::Response;

using std::pair;
using std::set;
using std::string;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// Tests that exercise the default executor implementation
// should be located in this file.

class DefaultExecutorTest
  : public MesosTest,
    public WithParamInterface<string> {};


// These tests are parameterized by the containerizers enabled on the agent.
INSTANTIATE_TEST_CASE_P(
    Containterizers,
    DefaultExecutorTest,
    ::testing::Values("mesos", "docker,mesos"));


// This test verifies that the default executor can launch a task group.
TEST_P(DefaultExecutorTest, ROOT_TaskRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  scheduler::v1::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo =
    evolve(createTask(slaveId, resources, "sleep 1000"));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update);

  ASSERT_EQ(TASK_RUNNING, update->status().state());
  EXPECT_EQ(taskInfo.task_id(), update->status().task_id());
  EXPECT_TRUE(update->status().has_timestamp());

  // Ensure that the task sandbox symbolic link is created.
  EXPECT_TRUE(os::exists(path::join(
      slave::paths::getExecutorLatestRunPath(
          flags.work_dir,
          slaveId,
          devolve(frameworkId),
          executorInfo.executor_id()),
      "tasks",
      taskInfo.task_id().value())));

  // Verify that the executor's type is exposed in the agent's state
  // endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);
  JSON::Object state = parse.get();

  EXPECT_SOME_EQ(
      JSON::String(ExecutorInfo::Type_Name(executorInfo.type())),
      state.find<JSON::String>("frameworks[0].executors[0].type"));
}


// This test verifies that if the default executor is asked
// to kill a task from a task group, it kills all tasks in
// the group and sends TASK_KILLED updates for them.
TEST_P(DefaultExecutorTest, ROOT_KillTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  scheduler::v1::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate1))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo1 =
    evolve(createTask(slaveId, resources, "sleep 1000"));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(slaveId, resources, "sleep 1000"));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate1);
  ASSERT_EQ(TASK_RUNNING, runningUpdate1->status().state());
  EXPECT_EQ(taskInfo1.task_id(), runningUpdate1->status().task_id());

  AWAIT_READY(runningUpdate2);
  ASSERT_EQ(TASK_RUNNING, runningUpdate2->status().state());
  EXPECT_EQ(taskInfo2.task_id(), runningUpdate2->status().task_id());

  // Acknowledge the TASK_RUNNING updates to receive the next updates.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo1.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo2.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate2->status().uuid());

    mesos.send(call);
  }

  // When killing a task, TASK_KILLED update for the tasks in a task
  // group can be received in any order.
  hashset<v1::TaskID> tasks;

  tasks.insert(taskInfo1.task_id());
  tasks.insert(taskInfo2.task_id());

  Future<v1::scheduler::Event::Update> killedUpdate1;
  Future<v1::scheduler::Event::Update> killedUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate1))
    .WillOnce(FutureArg<1>(&killedUpdate2));

  // Now kill one task in the task group.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo1.task_id());

    mesos.send(call);
  }

  // All the tasks in the task group should be killed.

  AWAIT_READY(killedUpdate1);
  ASSERT_EQ(TASK_KILLED, killedUpdate1->status().state());
  ASSERT_TRUE(tasks.contains(killedUpdate1->status().task_id()));

  tasks.erase(killedUpdate1->status().task_id());

  AWAIT_READY(killedUpdate2);
  ASSERT_EQ(TASK_KILLED, killedUpdate2->status().state());
  ASSERT_TRUE(tasks.contains(killedUpdate2->status().task_id()));
}


// This test verifies that if the default executor receives a
// non-zero exit status code for a task in the task group, it
// kills all the other tasks (default restart policy).
TEST_F(DefaultExecutorTest, KillTaskGroupOnTaskFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  scheduler::v1::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  // TODO(anand): The TASK_RUNNING updates for the tasks in a task group
  // can arrive in any order.
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&runningUpdate1))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  // The first task exits with a non-zero status code.
  v1::TaskInfo taskInfo1 =
    evolve(createTask(slaveId, resources, "exit 1"));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(slaveId, resources, "sleep 1000"));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(runningUpdate1);
  ASSERT_EQ(TASK_RUNNING, runningUpdate1->status().state());
  EXPECT_EQ(taskInfo1.task_id(), runningUpdate1->status().task_id());

  AWAIT_READY(runningUpdate2);
  ASSERT_EQ(TASK_RUNNING, runningUpdate2->status().state());
  EXPECT_EQ(taskInfo2.task_id(), runningUpdate2->status().task_id());

  // Acknowledge the TASK_RUNNING updates to receive the next updates.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo1.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo2.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(runningUpdate2->status().uuid());

    mesos.send(call);
  }

  // Updates for the tasks in a task group can be received in any order.
  set<pair<v1::TaskID, v1::TaskState>> taskStates;

  taskStates.insert({taskInfo1.task_id(), v1::TASK_FAILED});
  taskStates.insert({taskInfo2.task_id(), v1::TASK_KILLED});

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  set<std::pair<v1::TaskID, v1::TaskState>> expectedTaskStates;

  expectedTaskStates.insert(
      {update1->status().task_id(), update1->status().state()});

  expectedTaskStates.insert(
      {update2->status().task_id(), update2->status().state()});

  ASSERT_EQ(expectedTaskStates, taskStates);
}


// Verifies that a task in a task group with an executor is accepted
// during `TaskGroupInfo` validation.
TEST_P(DefaultExecutorTest, ROOT_TaskUsesExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;
  flags.containerizers = GetParam();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  scheduler::v1::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  Future<v1::scheduler::Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo =
    evolve(createTask(slaveId, resources, "sleep 1000"));

  taskInfo.mutable_executor()->CopyFrom(evolve(executorInfo));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update);

  ASSERT_EQ(TASK_RUNNING, update->status().state());
  EXPECT_EQ(taskInfo.task_id(), update->status().task_id());
  EXPECT_TRUE(update->status().has_timestamp());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
