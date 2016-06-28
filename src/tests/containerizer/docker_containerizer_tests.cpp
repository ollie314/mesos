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

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/slave/container_logger.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#endif // __linux__

#include "messages/messages.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using namespace mesos::internal::slave::paths;
using namespace mesos::internal::slave::state;

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::DockerContainerizerProcess;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerLogger;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Invoke;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class DockerContainerizerTest : public MesosTest
{
public:
  static string containerName(
      const SlaveID& slaveId,
      const ContainerID& containerId)
  {
    return slave::DOCKER_NAME_PREFIX + slaveId.value() +
      slave::DOCKER_NAME_SEPERATOR + containerId.value();
  }

  enum ContainerState
  {
    EXISTS,
    RUNNING
  };

  static bool exists(
      const process::Shared<Docker>& docker,
      const SlaveID& slaveId,
      const ContainerID& containerId,
      ContainerState state = ContainerState::EXISTS)
  {
    Duration waited = Duration::zero();
    string expectedName = containerName(slaveId, containerId);

    do {
      Future<Docker::Container> inspect = docker->inspect(expectedName);

      if (!inspect.await(Seconds(3))) {
        return false;
      }

      if (inspect.isReady()) {
        switch (state) {
          case ContainerState::RUNNING:
            if (inspect.get().pid.isSome()) {
              return true;
            }
            // Retry looking for running pid until timeout.
            break;
          case ContainerState::EXISTS:
            return true;
        }
      }

      os::sleep(Milliseconds(200));
      waited += Milliseconds(200);
    } while (waited < Seconds(5));

    return false;
  }

  static bool containsLine(
    const vector<string>& lines,
    const string& expectedLine)
  {
    foreach (const string& line, lines) {
      if (line == expectedLine) {
        return true;
      }
    }

    return false;
  }

  virtual void TearDown()
  {
    Try<Owned<Docker>> docker = Docker::create(
        tests::flags.docker,
        tests::flags.docker_socket,
        false);

    ASSERT_SOME(docker);

    Future<list<Docker::Container>> containers =
      docker.get()->ps(true, slave::DOCKER_NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }
  }
};


// Only enable executor launch on linux as other platforms
// requires running linux VM and need special port forwarding
// to get host networking to work.
#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch_Executor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("/bin/test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));
}


// This test verifies that a custom executor can be launched and
// registered with the slave with docker bridge network enabled.
// We're assuming that the custom executor is registering its public
// ip instead of 0.0.0.0 or equivalent to the slave as that's the
// default behavior for libprocess.
//
// Currently this test fails on ubuntu and centos since the slave is
// binding and advertising 127.0.x.x address and unreachable by executor
// in bridge network.
// TODO(tnachen): Re-enable this test when we are able to fix MESOS-3123.
TEST_F(DockerContainerizerTest, DISABLED_ROOT_DOCKER_Launch_Executor_Bridged)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("/bin/test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  ASSERT_TRUE(statusRunning.get().has_data());

  Try<JSON::Array> array = JSON::parse<JSON::Array>(statusRunning.get().data());
  ASSERT_SOME(array);

  // Check if container information is exposed through master's state endpoint.
  Future<http::Response> response = http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Value> find = parse.get().find<JSON::Value>(
      "frameworks[0].tasks[0].container.docker.privileged");

  EXPECT_SOME_FALSE(find);

  // Check if container information is exposed through slave's state endpoint.
  response = http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  find = parse.get().find<JSON::Value>(
      "frameworks[0].executors[0].tasks[0].container.docker.privileged");

  EXPECT_SOME_FALSE(find);

  // Now verify that the TaskStatus contains the container IP address.
  ASSERT_TRUE(statusRunning.get().has_container_status());
  EXPECT_EQ(1, statusRunning.get().container_status().network_infos().size());
  EXPECT_EQ(1, statusRunning.get().container_status().network_infos(0).ip_addresses().size()); // NOLINT(whitespace/line_length)

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Kill)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  driver.stop();
  driver.join();
}


// Ensures that the framework will receive a TASK_KILLING update
// before TASK_KILLED, if the capability is supported.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_TaskKillingCapability)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  // Start the framework with the task killing capability.
  FrameworkInfo::Capability capability;
  capability.set_type(FrameworkInfo::Capability::TASK_KILLING_STATE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->CopyFrom(capability);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  Future<TaskStatus> statusKilling, statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilling))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilling);
  EXPECT_EQ(TASK_KILLING, statusKilling->state());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  driver.stop();
  driver.join();
}


// This test tests DockerContainerizer::usage().
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Usage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>("cpus:2;mem:1024");

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  // Run a CPU intensive command, so we can measure utime and stime later.
  command.set_value("dd if=/dev/zero of=/dev/null");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // We ignore all update calls to prevent resizing cgroup limits.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Nothing()));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  // Verify the usage.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage =
      dockerContainerizer.usage(containerId.get());
    // TODO(tnachen): Replace await with AWAIT_COMPLETED once
    // implemented.
    ASSERT_TRUE(usage.await(Seconds(3)));

    if (usage.isReady()) {
      statistics = usage.get();

      if (statistics.cpus_user_time_secs() > 0 &&
          statistics.cpus_system_time_secs() > 0) {
        break;
      }
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(3));

  // Usage includes the executor resources.
  EXPECT_EQ(2.0 + slave::DEFAULT_EXECUTOR_CPUS, statistics.cpus_limit());
  EXPECT_EQ((Gigabytes(1) + slave::DEFAULT_EXECUTOR_MEM).bytes(),
            statistics.mem_limit_bytes());
  EXPECT_LT(0, statistics.cpus_user_time_secs());
  EXPECT_LT(0, statistics.cpus_system_time_secs());
  EXPECT_GT(statistics.mem_rss_bytes(), 0u);

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  dockerContainerizer.destroy(containerId.get());

  AWAIT_READY(termination);

  // Usage() should fail again since the container is destroyed.
  Future<ResourceStatistics> usage =
    dockerContainerizer.usage(containerId.get());

  AWAIT_FAILED(usage);

  driver.stop();
  driver.join();
}


#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Update)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  string name = containerName(slaveId, containerId.get());

  Future<Docker::Container> inspect = docker->inspect(name);

  AWAIT_READY(inspect);

  Try<Resources> newResources = Resources::parse("cpus:1;mem:128");

  ASSERT_SOME(newResources);

  Future<Nothing> update =
    dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  Result<string> memoryHierarchy = cgroups::hierarchy("memory");

  ASSERT_SOME(cpuHierarchy);
  ASSERT_SOME(memoryHierarchy);

  Option<pid_t> pid = inspect.get().pid;
  ASSERT_SOME(pid);

  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());
  ASSERT_SOME(cpuCgroup);

  Result<string> memoryCgroup = cgroups::memory::cgroup(pid.get());
  ASSERT_SOME(memoryCgroup);

  Try<uint64_t> cpu = cgroups::cpu::shares(
      cpuHierarchy.get(),
      cpuCgroup.get());

  ASSERT_SOME(cpu);

  Try<Bytes> mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(128u, mem.get().megabytes());

  newResources = Resources::parse("cpus:1;mem:144");

  // Issue second update that uses the cached pid instead of inspect.
  update = dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  cpu = cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get());

  ASSERT_SOME(cpu);

  mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(144u, mem.get().megabytes());

  driver.stop();
  driver.join();
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Recover)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  SlaveID slaveId;
  slaveId.set_value("s1");
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());
  ContainerID reapedContainerId;
  reapedContainerId.set_value(UUID::random().toString());

  string container1 = containerName(slaveId, containerId);
  string container2 = containerName(slaveId, reapedContainerId);

  // Clean up artifacts if containers still exists.
  ASSERT_TRUE(docker->rm(container1, true).await(Seconds(30)));
  ASSERT_TRUE(docker->rm(container2, true).await(Seconds(30)));

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value("sleep 1000");

  docker->run(
      containerInfo,
      commandInfo,
      container1,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  Future<Option<int>> orphanRun =
    docker->run(
        containerInfo,
        commandInfo,
        container2,
        flags.work_dir,
        flags.sandbox_directory,
        resources);

  ASSERT_TRUE(
    exists(docker, slaveId, containerId, ContainerState::RUNNING));
  ASSERT_TRUE(
    exists(docker, slaveId, reapedContainerId, ContainerState::RUNNING));

  Future<Docker::Container> inspect = docker->inspect(container2);
  AWAIT_READY(inspect);

  SlaveState slaveState;
  slaveState.id = slaveId;
  FrameworkState frameworkState;

  ExecutorID execId;
  execId.set_value("e1");

  ExecutorState execState;
  ExecutorInfo execInfo;
  execState.info = execInfo;
  execState.latest = containerId;

  Try<process::Subprocess> wait =
    process::subprocess(tests::flags.docker + " wait " + container1);

  ASSERT_SOME(wait);

  FrameworkID frameworkId;

  RunState runState;
  runState.id = containerId;
  runState.forkedPid = wait.get().pid();

  execState.runs.put(containerId, runState);
  frameworkState.executors.put(execId, execState);

  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);

  AWAIT_READY(recover);

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId);

  ASSERT_FALSE(termination.isFailed());

  AWAIT_FAILED(dockerContainerizer.wait(reapedContainerId));

  // Expect the orphan to be stopped!
  AWAIT_READY(orphanRun);
  ASSERT_SOME(orphanRun.get());
  EXPECT_TRUE(WIFEXITED(orphanRun->get())) << orphanRun->get();
  EXPECT_EQ(128 + SIGKILL, WEXITSTATUS(orphanRun->get())) << orphanRun->get();
}


// This test ensures that orphaned docker containers unknown to the current
// agent instance are properly cleaned up.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_KillOrphanContainers)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  SlaveID slaveId;
  slaveId.set_value("s1");

  SlaveID oldSlaveId;
  oldSlaveId.set_value("old-agent-id");

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ContainerID orphanContainerId;
  orphanContainerId.set_value(UUID::random().toString());

  string container1 = containerName(slaveId, containerId);

  // Start the orphan container with the old slave id.
  string container2 = containerName(oldSlaveId, orphanContainerId);

  // Clean up artifacts if containers still exists.
  ASSERT_TRUE(docker->rm(container1, true).await(Seconds(30)));
  ASSERT_TRUE(docker->rm(container2, true).await(Seconds(30)));

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value("sleep 1000");

  docker->run(
      containerInfo,
      commandInfo,
      container1,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  Future<Option<int>> orphanRun =
    docker->run(
        containerInfo,
        commandInfo,
        container2,
        flags.work_dir,
        flags.sandbox_directory,
        resources);

  ASSERT_TRUE(
    exists(docker, slaveId, containerId, ContainerState::RUNNING));

  ASSERT_TRUE(
    exists(docker, oldSlaveId, orphanContainerId, ContainerState::RUNNING));

  SlaveState slaveState;
  slaveState.id = slaveId;

  FrameworkState frameworkState;

  ExecutorID execId;
  execId.set_value("e1");

  ExecutorState execState;
  ExecutorInfo execInfo;

  execState.info = execInfo;
  execState.latest = containerId;

  Try<process::Subprocess> wait =
    process::subprocess(tests::flags.docker + " wait " + container1);

  ASSERT_SOME(wait);

  FrameworkID frameworkId;

  RunState runState;
  runState.id = containerId;
  runState.forkedPid = wait.get().pid();

  execState.runs.put(containerId, runState);
  frameworkState.executors.put(execId, execState);

  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);

  AWAIT_READY(recover);

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId);

  ASSERT_FALSE(termination.isFailed());

  // The orphaned container should be correctly cleaned up.
  AWAIT_FAILED(dockerContainerizer.wait(orphanContainerId));
  ASSERT_FALSE(exists(docker, oldSlaveId, orphanContainerId));

  AWAIT_READY(orphanRun);
  ASSERT_SOME(orphanRun.get());
  EXPECT_TRUE(WIFEXITED(orphanRun->get())) << orphanRun->get();
  EXPECT_EQ(128 + SIGKILL, WEXITSTATUS(orphanRun->get())) << orphanRun->get();
}


// This test checks the docker containerizer doesn't recover executors
// that were started by another containerizer (e.g: mesos).
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SkipRecoverNonDocker)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorID executorId;
  executorId.set_value(UUID::random().toString());

  ExecutorInfo executorInfo;
  executorInfo.mutable_container()->set_type(ContainerInfo::MESOS);

  ExecutorState executorState;
  executorState.info = executorInfo;
  executorState.latest = containerId;

  RunState runState;
  runState.id = containerId;
  executorState.runs.put(containerId, runState);

  FrameworkState frameworkState;
  frameworkState.executors.put(executorId, executorState);

  SlaveState slaveState;
  FrameworkID frameworkId;
  frameworkId.set_value(UUID::random().toString());
  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);
  AWAIT_READY(recover);

  Future<hashset<ContainerID>> containers = dockerContainerizer.containers();
  AWAIT_READY(containers);

  // A MesosContainerizer task shouldn't be recovered by
  // DockerContainerizer.
  EXPECT_EQ(0u, containers.get().size());
}


#ifdef __linux__
// This test verifies that we can launch a docker container with
// persistent volume.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_LaunchWithPersistentVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpu:2;mem:2048;disk(role1):2048";

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  Resource volume = createPersistentVolume(
    Megabytes(64),
    "role1",
    "id1",
    "path1",
    None(),
    None(),
    frameworkInfo.principal());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:64;").get() + volume);

  CommandInfo command;
  command.set_value("echo abc > " +
                    path::join(flags.sandbox_directory, "path1", "file"));

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), LAUNCH({task})},
      filters);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  const string& volumePath = getPersistentVolumePath(
      flags.work_dir,
      volume);

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volumePath, "file")));

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  EXPECT_SOME(table);

  // Verify that the persistent volume is unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    EXPECT_FALSE(
        strings::contains(entry.target, path::join(directory.get(), "path1")));
  }
}


// This test checks the docker containerizer is able to recover containers
// with persistent volumes and destroy it properly.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_RecoverPersistentVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpu:2;mem:2048;disk(role1):2048";

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Filters filters;
  filters.set_refuse_seconds(0);

  // NOTE: We set filter explicitly here so that the resources will
  // not be filtered for 5 seconds (the default).
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(filters));      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  Resource volume = createPersistentVolume(
    Megabytes(64),
    "role1",
    "id1",
    "path1",
    None(),
    None(),
    frameworkInfo.principal());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:64;").get() + volume);

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), LAUNCH({task})},
      filters);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  // Recreate containerizer and start slave again.
  slave.get()->terminate();
  slave->reset();

  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Wait until containerizer recover is complete.
  AWAIT_READY(_recover);

  Future<containerizer::Termination> termination =
    dockerContainerizer->wait(containerId.get());

  dockerContainerizer->destroy(containerId.get());

  AWAIT_READY(termination);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  EXPECT_SOME(table);

  // Verify that the recovered container's persistent volume is
  // unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    EXPECT_FALSE(
        strings::contains(entry.target, path::join(directory.get(), "path1")));
  }

  driver.stop();
  driver.join();
}


// This test checks the docker containerizer is able to clean up
// orphaned containers with persistent volumes.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_RecoverOrphanedPersistentVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpu:2;mem:2048;disk(role1):2048";

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Filters filters;
  filters.set_refuse_seconds(0);

  // NOTE: We set filter explicitly here so that the resources will
  // not be filtered for 5 seconds (the default).
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(filters)); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];

  Resource volume = createPersistentVolume(
    Megabytes(64),
    "role1",
    "id1",
    "path1",
    None(),
    None(),
    frameworkInfo.principal());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:64;").get() + volume);

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume), LAUNCH({task})},
      filters);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  // Recreate containerizer and start slave again.
  slave.get()->terminate();
  slave->reset();

  // Wipe the framework directory so that the slave will treat the
  // above running task as an orphan. We don't want to wipe the whole
  // meta directory since Docker Containerizer will skip recover if
  // state is not found.
  ASSERT_SOME(
      os::rmdir(getFrameworkPath(
          getMetaRootDir(flags.work_dir),
          offer.slave_id(),
          frameworkId.get())));

  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Wait until containerizer recover is complete.
  AWAIT_READY(_recover);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  EXPECT_SOME(table);

  // Verify that the orphaned container's persistent volume is
  // unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    EXPECT_FALSE(
        strings::contains(entry.target, path::join(directory.get(), "path1")));
  }

  driver.stop();
  driver.join();

  slave->reset();

  EXPECT_FALSE(exists(docker, offer.slave_id(), containerId.get()));
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Logs)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  // NOTE: We prefix `echo` with `unbuffer` so that we can immediately
  // flush the output of `echo`.  This mitigates a race in Docker where
  // it mangles reads from stdout/stderr and commits suicide.
  // See MESOS-4676 for more information.
  CommandInfo command;
  command.set_value(
      "unbuffer echo out" + uuid + " ; "
      "unbuffer echo err" + uuid + " 1>&2");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  // NOTE: This is an image that is exactly
  // `docker run -t -i alpine /bin/sh -c "apk add --update expect"`.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/alpine-expect");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stderr and stdout (which
  // might also contain other things, hence the use of a UUID).
  Try<string> read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  EXPECT_TRUE(containsLine(lines, "err" + uuid));
  EXPECT_FALSE(containsLine(lines, "out" + uuid));

  read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_TRUE(containsLine(lines, "out" + uuid));
  EXPECT_FALSE(containsLine(lines, "err" + uuid));

  driver.stop();
  driver.join();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_shell(false);

  // NOTE: By not setting CommandInfo::value we're testing that we
  // will still be able to run the container because it has a default
  // entrypoint!

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // Since we're not passing any command value, we're expecting the
  // default entry point to be run which is 'echo' with the default
  // command from the image which is 'inky'.
  EXPECT_TRUE(containsLine(lines, "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));

  driver.stop();
  driver.join();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Override)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping  a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We can set the value to just the 'uuid' since it should get
  // passed as an argument to the entrypoint, i.e., 'echo uuid'.
  command.set_value(uuid);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stderr and stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // We expect the passed in command value to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(containsLine(lines, uuid));
  EXPECT_FALSE(containsLine(lines, "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));
  EXPECT_FALSE(containsLine(lines, uuid));

  driver.stop();
  driver.join();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Args)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We should also be able to skip setting the comamnd value and just
  // set the arguments and those should also get passed through to the
  // entrypoint!
  command.add_arguments(uuid);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stderr and stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // We expect the passed in command arguments to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(containsLine(lines, uuid));
  EXPECT_FALSE(containsLine(lines, "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));
  EXPECT_FALSE(containsLine(lines, uuid));

  driver.stop();
  driver.join();
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// re-registers and the slave properly sends the update.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SlaveRecoveryTaskContainer)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  // This is owned by the containerizer, so we'll need one per containerizer.
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  // Stop the slave before the status update is received.
  AWAIT_READY(statusUpdateMessage);

  slave.get()->terminate();

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // This is owned by the containerizer, so we'll need one per containerizer.
  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer->wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// re-registers and the slave properly sends the update.
//
// TODO(benh): This test is currently disabled because the executor
// inside the image mesosphere/test-executor does not properly set the
// executor PID that is uses during registration, so when the new
// slave recovers it can't reconnect and instead destroys that
// container. In particular, it uses '0' for its IP which we properly
// parse and can even properly use for sending other messages, but the
// current implementation of 'UPID::operator bool()' fails if the IP
// component of a PID is '0'.
TEST_F(DockerContainerizerTest,
       DISABLED_ROOT_DOCKER_SlaveRecoveryExecutorContainer)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  // This is owned by the containerizer, so we'll need one per containerizer.
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<ContainerID> containerId;
  Future<SlaveID> slaveId;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<4>(&slaveId),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launchExecutor)));

  // We need to wait until the container's pid has been been
  // checkpointed so that when the next slave recovers it won't treat
  // the executor as having gone lost! We know this has completed
  // after Containerizer::launch returns and the
  // Slave::executorLaunched gets dispatched.
  Future<Nothing> executorLaunched =
    FUTURE_DISPATCH(_, &Slave::executorLaunched);

  // The test-executor in the image immediately sends a TASK_RUNNING
  // followed by TASK_FINISHED (no sleep/delay in between) so we need
  // to drop the first TWO updates that come from the executor rather
  // than only the first update like above where we can control how
  // the length of the task.
  Future<StatusUpdateMessage> statusUpdateMessage1 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdateMessage2 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);
  AWAIT_READY(slaveId);

  AWAIT_READY(executorLaunched);
  AWAIT_READY(statusUpdateMessage1);
  AWAIT_READY(statusUpdateMessage2);

  slave.get()->terminate();

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // This is owned by the containerizer, so we'll need one per containerizer.
  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  ASSERT_TRUE(exists(docker, slaveId.get(), containerId.get()));

  driver.stop();
  driver.join();
}


// This test verifies that port mapping with bridge network is
// exposing the host port to the container port, by sending data
// to the host port and receiving it in the container by listening
// to the mapped container port.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_NC_PortMapping)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:1;mem:1024;ports:[10000-10000]";

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("nc");
  command.add_arguments("-l");
  command.add_arguments("-p");
  command.add_arguments("1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  ContainerInfo::DockerInfo::PortMapping portMapping;
  portMapping.set_host_port(10000);
  portMapping.set_container_port(1000);

  dockerInfo.add_port_mappings()->CopyFrom(portMapping);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  string uuid = UUID::random().toString();

  // Write uuid to docker mapped host port.
  Try<process::Subprocess> s = process::subprocess(
      "echo " + uuid + " | nc localhost 10000");

  ASSERT_SOME(s);
  AWAIT_READY_FOR(s.get().status(), Seconds(60));

  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  const vector<string> lines = strings::split(read.get(), "\n");

  // We expect the uuid that is sent to host port to be written
  // to stdout by the docker container running nc -l.
  EXPECT_TRUE(containsLine(lines, uuid));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
}


// This test verifies that sandbox with ':' in the path can still
// run successfully. This a limitation of the Docker CLI where
// the volume map parameter treats colons (:) as seperators,
// and incorrectly seperates the sandbox directory.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_LaunchSandboxWithColon)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("test:colon");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhileFetching)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Promise<Nothing> promise;
  Future<Nothing> fetch;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, fetch(_, _))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(promise.future())));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(fetch);

  dockerContainerizer.destroy(containerId.get());

  // Resume docker launch.
  promise.set(Nothing());

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhilePulling)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Future<Nothing> fetch;
  EXPECT_CALL(*process, fetch(_, _))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(Nothing())));

  Promise<Nothing> promise;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, pull(_))
    .WillOnce(Return(promise.future()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  // Wait until fetch is finished.
  AWAIT_READY(fetch);

  dockerContainerizer.destroy(containerId.get());

  // Resume docker launch.
  promise.set(Nothing());

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();
}


// This test checks that when a docker containerizer update failed
// and the container failed before the executor started, the executor
// is properly killed and cleaned up.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_ExecutorCleanupWhenLaunchFailed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("ls");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusLost;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusLost));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // Fail the update so we don't proceed to send run task to the executor.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Failure("Fail resource update")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusLost);
  EXPECT_EQ(TASK_LOST, statusLost.get().state());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_UPDATE_FAILED,
            statusLost.get().reason());

  driver.stop();
  driver.join();
}


// When the fetch fails we should send the scheduler a status
// update with message the shows the actual error.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_FetchFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("ls");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  EXPECT_CALL(*process, fetch(_, _))
    .WillOnce(Return(Failure("some error from fetch")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());
  EXPECT_EQ("Failed to launch container: some error from fetch",
             statusFailed.get().message());

  // TODO(jaybuff): When MESOS-2035 is addressed we should validate
  // that statusFailed.get().reason() is correctly set here.

  driver.stop();
  driver.join();
}


// When the docker pull fails we should send the scheduler a status
// update with message the shows the actual error.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DockerPullFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("ls");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  EXPECT_CALL(*mockDocker, pull(_, _, _))
    .WillOnce(Return(Failure("some error from docker pull")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());
  EXPECT_EQ("Failed to launch container: some error from docker pull",
             statusFailed.get().message());

  // TODO(jaybuff): When MESOS-2035 is addressed we should validate
  // that statusFailed.get().reason() is correctly set here.

  driver.stop();
  driver.join();
}


// When the docker executor container fails to launch, docker inspect
// future that is in a retry loop should be discarded.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DockerInspectDiscard)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Future<Docker::Container> inspect;
  EXPECT_CALL(*mockDocker, inspect(_, _))
    .WillOnce(FutureResult(&inspect,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_inspect)));

  EXPECT_CALL(*mockDocker, run(_, _, _, _, _, _, _, _, _))
    .WillOnce(Return(Failure("Run failed")));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("/bin/test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  AWAIT_DISCARDED(inspect);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
