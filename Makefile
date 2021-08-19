default: all

thrift_proto: distbench.thrift
	../../opt/thrift/bin/thrift --gen cpp distbench.thrift

testlog:
	bazel test --test_output=all  -c dbg --cxxopt='-std=c++17' :all

test:
	bazel test --cxxopt='-std=c++17' -c dbg :all

all:
	bazel build --cxxopt='-std=c++17' :distbench

test_with_thrift: thrift_proto
	bazel test --cxxopt='-std=c++17' :all --//:with-thrift

all_with_thrift: thrift_proto
	bazel build --cxxopt='-std=c++17' :distbench --//:with-thrift

clean:
	bazel clean
