namespace cpp thrift

struct GenericRequest {
	1: string payload,
}

struct GenericResponse {
	1: string payload,
}

service Distbench {
	GenericResponse GenericRPC(1: GenericRequest generic_request);
}
