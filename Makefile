default: all

thrift_proto: distbench.thrift
	../../opt/thrift/bin/thrift --gen cpp distbench.thrift

testlog:
	bazel test --test_output=all --cxxopt='-std=c++17' :all

test:
	bazel test --cxxopt='-std=c++17' :all

all: thrift_proto
	bazel build --cxxopt='-std=c++17' :all

clean:
	bazel clean
