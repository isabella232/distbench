// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "distbench_test_sequencer.h"

#include "distbench_node_manager.h"
#include "distbench_utils.h"
#include "protocol_driver_allocator.h"
#include "gtest/gtest.h"
#include "gtest_utils.h"
#include "glog/logging.h"
#include "google/protobuf/text_format.h"

namespace distbench {

struct DistBenchTester {
  ~DistBenchTester();
  absl::Status Initialize(int num_nodes);

  std::unique_ptr<TestSequencer> test_sequencer;
  std::unique_ptr<DistBenchTestSequencer::Stub> test_sequencer_stub;
  std::vector<std::unique_ptr<NodeManager>> nodes;
};

DistBenchTester::~DistBenchTester() {
  test_sequencer->Shutdown();
  for (size_t i = 0; i < nodes.size(); ++i) {
    nodes[i]->Shutdown();
  }
  test_sequencer->Wait();
  for (size_t i = 0; i < nodes.size(); ++i) {
    nodes[i]->Wait();
  }
}

absl::Status DistBenchTester::Initialize(int num_nodes) {
  test_sequencer = std::make_unique<TestSequencer>();
  distbench::TestSequencerOpts ts_opts = {};
  int port = 0;
  ts_opts.port = &port;
  test_sequencer->Initialize(ts_opts);
  nodes.resize(num_nodes);
  for (int i = 0; i < num_nodes; ++i) {
    distbench::NodeManagerOpts nm_opts = {};
    int port = 0;
    nm_opts.port = &port;
    nm_opts.test_sequencer_service_address =
      test_sequencer->service_address();
    nodes[i] = std::make_unique<NodeManager>();
    auto ret = nodes[i]->Initialize(nm_opts);
    if (!ret.ok())
      return ret;
  }
  std::shared_ptr<grpc::ChannelCredentials> client_creds =
    MakeChannelCredentials();
  std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
      test_sequencer->service_address(), client_creds,
      DistbenchCustomChannelArguments());
  test_sequencer_stub = DistBenchTestSequencer::NewStub(channel);
  return absl::OkStatus();
}

TEST(DistBenchTestSequencer, ctor) {
  TestSequencer test_sequencer;
}

TEST(DistBenchTestSequencer, init) {
  distbench::TestSequencerOpts ts_opts = {};
  int port = 0;
  ts_opts.port = &port;
  TestSequencer test_sequencer;
  test_sequencer.Initialize(ts_opts);
  test_sequencer.Shutdown();
}

TEST(DistBenchTestSequencer, empty_group) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(0));
}

TEST(DistBenchTestSequencer, nonempty_group) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(3));

  TestSequence test_sequence;
  auto* test = test_sequence.add_tests();
  auto* s1 = test->add_services();
  s1->set_name("s1");
  s1->set_count(1);
  auto* s2 = test->add_services();
  s2->set_name("s2");
  s2->set_count(2);

  auto* l1 = test->add_action_lists();
  l1->set_name("s1");
  l1->add_action_names("s1/ping");

  auto a1 = test->add_actions();
  a1->set_name("s1/ping");
  a1->set_rpc_name("echo");
  a1->mutable_iterations()->set_max_iteration_count(10);

  auto* r1 = test->add_rpc_descriptions();
  r1->set_name("echo");
  r1->set_client("s1");
  r1->set_server("s2");

  auto* l2 = test->add_action_lists();
  l2->set_name("echo");

  TestSequenceResults results;
  grpc::ClientContext context;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(70);
  context.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  ASSERT_OK(status);

  ASSERT_EQ(results.test_results().size(), 1);
  auto& test_results = results.test_results(0);
  ASSERT_EQ(test_results.service_logs().instance_logs_size(), 1);
  const auto& instance_results_it =
    test_results.service_logs().instance_logs().find("s1/0");
  ASSERT_NE(instance_results_it,
            test_results.service_logs().instance_logs().end());
  auto s2_0 = instance_results_it->second.peer_logs().find("s2/0");
  ASSERT_NE(s2_0, instance_results_it->second.peer_logs().end());
  auto s2_0_echo = s2_0->second.rpc_logs().find(0);
  ASSERT_NE(s2_0_echo, s2_0->second.rpc_logs().end());
  ASSERT_TRUE(s2_0_echo->second.failed_rpc_samples().empty());
  ASSERT_EQ(s2_0_echo->second.successful_rpc_samples_size(), 10);

  auto s2_1 = instance_results_it->second.peer_logs().find("s2/1");
  ASSERT_NE(s2_1, instance_results_it->second.peer_logs().end());
  auto s2_1_echo = s2_1->second.rpc_logs().find(0);
  ASSERT_NE(s2_1_echo, s2_1->second.rpc_logs().end());
  ASSERT_TRUE(s2_1_echo->second.failed_rpc_samples().empty());
  ASSERT_EQ(s2_1_echo->second.successful_rpc_samples_size(), 10);
}

void RunIntenseTraffic(const char* protocol) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(6));

  TestSequence test_sequence;
  auto* test = test_sequence.add_tests();
  test->set_default_protocol(protocol);
  auto* s1 = test->add_services();
  s1->set_name("s1");
  s1->set_count(1);
  auto* s2 = test->add_services();
  s2->set_name("s2");
  s2->set_count(5);

  auto* l1 = test->add_action_lists();
  l1->set_name("s1");
  l1->add_action_names("s1/ping");

  auto a1 = test->add_actions();
  a1->set_name("s1/ping");
  a1->set_rpc_name("echo");

  auto* iterations = a1->mutable_iterations();
  iterations->set_max_duration_us(200000);
  iterations->set_max_iteration_count(2000);
  iterations->set_max_parallel_iterations(100);

  auto* r1 = test->add_rpc_descriptions();
  r1->set_name("echo");
  r1->set_client("s1");
  r1->set_server("s2");

  auto* l2 = test->add_action_lists();
  l2->set_name("echo");

  TestSequenceResults results;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(200);
  grpc::ClientContext context;
  grpc::ClientContext context2;
  grpc::ClientContext context3;
  context.set_deadline(deadline);
  context2.set_deadline(deadline);
  context3.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  LOG(INFO) << status.error_message();
  ASSERT_OK(status);

  iterations->clear_max_iteration_count();
  status = tester.test_sequencer_stub->RunTestSequence(
      &context2, test_sequence, &results);
  LOG(INFO) << status.error_message();
  ASSERT_OK(status);

  iterations->clear_max_duration_us();
  iterations->set_max_iteration_count(2000);
  status = tester.test_sequencer_stub->RunTestSequence(
      &context3, test_sequence, &results);
  LOG(INFO) << status.error_message();
  ASSERT_OK(status);
}

TEST(DistBenchTestSequencer, TestReservoirSampling) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(2));

  TestSequence test_sequence;
  auto* test = test_sequence.add_tests();
  test->set_default_protocol("grpc");
  auto* s1 = test->add_services();
  s1->set_name("s1");
  s1->set_count(1);
  auto* s2 = test->add_services();
  s2->set_name("s2");
  s2->set_count(1);

  auto* l1 = test->add_action_lists();
  l1->set_name("s1");
  l1->add_action_names("s1/ping");
  l1->set_max_rpc_samples(1000);

  auto a1 = test->add_actions();
  a1->set_name("s1/ping");
  a1->set_rpc_name("echo");
  a1->mutable_iterations()->set_max_iteration_count(10);

  auto* iterations = a1->mutable_iterations();
  iterations->set_max_parallel_iterations(100);
  iterations->set_max_iteration_count(2000);

  auto* r1 = test->add_rpc_descriptions();
  r1->set_name("echo");
  r1->set_client("s1");
  r1->set_server("s2");

  auto* l2 = test->add_action_lists();
  l2->set_name("echo");

  TestSequenceResults results;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(200);
  grpc::ClientContext context;
  context.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  LOG(INFO) << status.error_message();
  ASSERT_OK(status);
  ASSERT_EQ(results.test_results_size(), 1);
  ASSERT_EQ(results.test_results(0).service_logs().instance_logs_size(), 1);
  auto it = results.test_results(0).service_logs().instance_logs().begin();
  EXPECT_EQ(it->first, "s1/0");
  ASSERT_EQ(it->second.peer_logs_size(), 1);
  auto it2 = it->second.peer_logs().begin();
  EXPECT_EQ(it2->first, "s2/0");
  ASSERT_EQ(it2->second.rpc_logs_size(), 1);
  auto it3 = it2->second.rpc_logs().begin();
  EXPECT_EQ(it3->first, 0);
  EXPECT_EQ(it3->second.successful_rpc_samples_size(), 1000);
  EXPECT_EQ(it3->second.failed_rpc_samples_size(), 0);
}

TEST(DistBenchTestSequencer, 100k_grpc) {
  RunIntenseTraffic("grpc");
}

TEST(DistBenchTestSequencer, 100k_grpc_async_callback) {
  RunIntenseTraffic("grpc_async_callback");
}

TEST(DistBenchTestSequencer, clique_test) {
  int nb_cliques = 3;

  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(nb_cliques));

  TestSequence test_sequence;
  auto* test = test_sequence.add_tests();

  auto* lo_opts = test->add_protocol_driver_options();
  lo_opts->set_name("lo_opts");
  lo_opts->set_netdev_name("lo");

  auto* s1 = test->add_services();
  s1->set_name("clique");
  s1->set_count(nb_cliques);
  s1->set_protocol_driver_options_name("lo_opts");

  auto* l1 = test->add_action_lists();
  l1->set_name("clique");
  l1->add_action_names("clique_queries");

  auto a1 = test->add_actions();
  a1->set_name("clique_queries");
  a1->mutable_iterations()->set_max_duration_us(10000000);
  a1->mutable_iterations()->set_open_loop_interval_ns(16000000);
  a1->mutable_iterations()->set_open_loop_interval_distribution("sync_burst");
  a1->set_rpc_name("clique_query");

  auto* r1 = test->add_rpc_descriptions();
  r1->set_name("clique_query");
  r1->set_client("clique");
  r1->set_server("clique");
  r1->set_fanout_filter("all");

  auto* l2 = test->add_action_lists();
  l2->set_name("clique_query");

  TestSequenceResults results;
  grpc::ClientContext context;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(75);
  context.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  ASSERT_OK(status);

  ASSERT_EQ(results.test_results().size(), 1);
  auto& test_results = results.test_results(0);

  const auto& log_summary = test_results.log_summary();
  const auto& latency_summary = log_summary[1];
  size_t pos = latency_summary.find("N: ") + 3;
  ASSERT_NE(pos, std::string::npos);
  const std::string N_value = latency_summary.substr(pos);

  std::string N_value2 = N_value.substr(0, N_value.find(' '));
  int N;
  ASSERT_EQ(absl::SimpleAtoi(N_value2, &N), true);
  int min = 624 * (nb_cliques * (nb_cliques - 1));
  int max = 626 * (nb_cliques * (nb_cliques - 1));
  ASSERT_LE(N, max);
  ASSERT_GE(N, min);

  ASSERT_EQ(test_results.service_logs().instance_logs_size(), 3);
  const auto& instance_results_it =
    test_results.service_logs().instance_logs().find("clique/0");
  ASSERT_NE(instance_results_it,
            test_results.service_logs().instance_logs().end());
}

TEST(DistBenchTestSequencer, stochastic_test) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(6));

  const std::string proto = R"(
tests {
  services {
    name: "client"
    count: 1
  }
  services {
    name: "server"
    count: 5
  }
  rpc_descriptions {
    name: "client_server_rpc"
    client: "client"
    server: "server"
    request_payload_name: "request_payload"
    response_payload_name: "response_payload"
    fanout_filter: "stochastic{0.7:1,0.3:4}"
  }
  payload_descriptions {
    name: "request_payload"
    size: 196
  }
  payload_descriptions {
    name: "response_payload"
    size: 262144
  }
  action_lists {
    name: "client"
    action_names: "run_queries"
  }
  actions {
    name: "run_queries"
    rpc_name: "client_server_rpc"
    iterations {
      max_iteration_count: 1000
    }
  }
  action_lists {
    name: "client_server_rpc"
  }
})";

  TestSequence test_sequence;
  bool parse_result = google::protobuf::TextFormat::ParseFromString(
      proto, &test_sequence);
  ASSERT_EQ(parse_result, true);

  TestSequenceResults results;
  grpc::ClientContext context;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(75);
  context.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  ASSERT_OK(status);

  auto& test_results = results.test_results(0);
  ASSERT_EQ(test_results.service_logs().instance_logs_size(), 1);

  const auto& log_summary = test_results.log_summary();
  const auto& latency_summary = log_summary[1];
  size_t pos = latency_summary.find("N: ") + 3;
  ASSERT_NE(pos, std::string::npos);
  const std::string N_value = latency_summary.substr(pos);

  std::string N_value2 = N_value.substr(0, N_value.find(' '));
  int N;
  ASSERT_EQ(absl::SimpleAtoi(N_value2, &N), true);
  ASSERT_LE(N, 2300);
  ASSERT_GE(N, 1500);
}

TEST(DistBenchTestSequencer, protocol_driver_options_test) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(2));

  TestSequence test_sequence;

  const std::string proto = R"(
tests {
  services {
    name: "client"
    count: 1
  }
  services {
    name: "server"
    count: 1
    protocol_driver_options_name: "loopback_pd"
  }
  rpc_descriptions {
    name: "client_server_rpc"
    client: "client"
    server: "server"
  }
  action_lists {
    name: "client"
    action_names: "run_queries"
  }
  actions {
    name: "run_queries"
    rpc_name: "client_server_rpc"
    iterations {
      max_iteration_count: 1000
    }
  }
  action_lists {
    name: "client_server_rpc"
  }
  protocol_driver_options {
    name: "loopback_pd"
    netdev_name: "lo"
  }
})";
  bool parse_result = google::protobuf::TextFormat::ParseFromString(
      proto, &test_sequence);
  ASSERT_EQ(parse_result, true);

  TestSequenceResults results;
  grpc::ClientContext context;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(15);
  context.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  ASSERT_OK(status);

  auto& test_results = results.test_results(0);
  ASSERT_EQ(test_results.service_logs().instance_logs_size(), 1);
}

TEST(DistBenchTestSequencer, FanoutRoundRobinTest) {
  DistBenchTester tester;
  ASSERT_OK(tester.Initialize(5));

  const std::string proto = R"(
tests {
  services {
    name: "client"
    count: 1
  }
  services {
    name: "server"
    count: 4
    protocol_driver_options_name: "loopback_pd"
  }
  rpc_descriptions {
    name: "client_server_rpc"
    client: "client"
    server: "server"
    fanout_filter: "round_robin"
  }
  action_lists {
    name: "client"
    action_names: "run_queries"
  }
  actions {
    name: "run_queries"
    rpc_name: "client_server_rpc"
    iterations {
      max_iteration_count: 1024
    }
  }
  action_lists {
    name: "client_server_rpc"
  }
  protocol_driver_options {
    name: "loopback_pd"
    netdev_name: "lo"
  }
})";
  TestSequence test_sequence;
  bool parse_result = google::protobuf::TextFormat::ParseFromString(
      proto, &test_sequence);
  ASSERT_EQ(parse_result, true);

  TestSequenceResults results;
  grpc::ClientContext context;
  std::chrono::system_clock::time_point deadline =
    std::chrono::system_clock::now() + std::chrono::seconds(15);
  context.set_deadline(deadline);
  grpc::Status status = tester.test_sequencer_stub->RunTestSequence(
      &context, test_sequence, &results);
  ASSERT_OK(status);

  auto& test_results = results.test_results(0);
  ASSERT_EQ(test_results.service_logs().instance_logs_size(), 1);

  auto serv_log_it = test_results.service_logs().instance_logs().find(
      "client/0");
  for (int i = 0; i < 4; i++) {
    auto peer_log_it = serv_log_it->second.peer_logs().find(
        absl::StrCat("server/", i));
    auto rpc_log_it = peer_log_it->second.rpc_logs().find(0);
    ASSERT_EQ(rpc_log_it->second.successful_rpc_samples_size(), 256);
  }
}

}  // namespace distbench
