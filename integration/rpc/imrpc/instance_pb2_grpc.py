# Generated by the gRPC Python protocol compiler plugin. DO NOT EDIT!
"""Client and server classes corresponding to protobuf-defined services."""
import grpc

from google.protobuf import empty_pb2 as google_dot_protobuf_dot_empty__pb2
from imrpc import imrpc_pb2 as imrpc_dot_imrpc__pb2
from imrpc import instance_pb2 as imrpc_dot_instance__pb2


class InstanceServiceStub(object):
    """Missing associated documentation comment in .proto file."""

    def __init__(self, channel):
        """Constructor.

        Args:
            channel: A grpc.Channel.
        """
        self.InstanceCreate = channel.unary_unary(
                '/imrpc.InstanceService/InstanceCreate',
                request_serializer=imrpc_dot_instance__pb2.InstanceCreateRequest.SerializeToString,
                response_deserializer=imrpc_dot_instance__pb2.InstanceResponse.FromString,
                )
        self.InstanceDelete = channel.unary_unary(
                '/imrpc.InstanceService/InstanceDelete',
                request_serializer=imrpc_dot_instance__pb2.InstanceDeleteRequest.SerializeToString,
                response_deserializer=imrpc_dot_instance__pb2.InstanceResponse.FromString,
                )
        self.InstanceGet = channel.unary_unary(
                '/imrpc.InstanceService/InstanceGet',
                request_serializer=imrpc_dot_instance__pb2.InstanceGetRequest.SerializeToString,
                response_deserializer=imrpc_dot_instance__pb2.InstanceResponse.FromString,
                )
        self.InstanceList = channel.unary_unary(
                '/imrpc.InstanceService/InstanceList',
                request_serializer=google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
                response_deserializer=imrpc_dot_instance__pb2.InstanceListResponse.FromString,
                )
        self.InstanceLog = channel.unary_stream(
                '/imrpc.InstanceService/InstanceLog',
                request_serializer=imrpc_dot_instance__pb2.InstanceLogRequest.SerializeToString,
                response_deserializer=imrpc_dot_imrpc__pb2.LogResponse.FromString,
                )
        self.InstanceWatch = channel.unary_stream(
                '/imrpc.InstanceService/InstanceWatch',
                request_serializer=google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
                response_deserializer=google_dot_protobuf_dot_empty__pb2.Empty.FromString,
                )
        self.InstanceReplace = channel.unary_unary(
                '/imrpc.InstanceService/InstanceReplace',
                request_serializer=imrpc_dot_instance__pb2.InstanceReplaceRequest.SerializeToString,
                response_deserializer=imrpc_dot_instance__pb2.InstanceResponse.FromString,
                )
        self.VersionGet = channel.unary_unary(
                '/imrpc.InstanceService/VersionGet',
                request_serializer=google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
                response_deserializer=imrpc_dot_imrpc__pb2.VersionResponse.FromString,
                )


class InstanceServiceServicer(object):
    """Missing associated documentation comment in .proto file."""

    def InstanceCreate(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def InstanceDelete(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def InstanceGet(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def InstanceList(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def InstanceLog(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def InstanceWatch(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def InstanceReplace(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def VersionGet(self, request, context):
        """Missing associated documentation comment in .proto file."""
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')


def add_InstanceServiceServicer_to_server(servicer, server):
    rpc_method_handlers = {
            'InstanceCreate': grpc.unary_unary_rpc_method_handler(
                    servicer.InstanceCreate,
                    request_deserializer=imrpc_dot_instance__pb2.InstanceCreateRequest.FromString,
                    response_serializer=imrpc_dot_instance__pb2.InstanceResponse.SerializeToString,
            ),
            'InstanceDelete': grpc.unary_unary_rpc_method_handler(
                    servicer.InstanceDelete,
                    request_deserializer=imrpc_dot_instance__pb2.InstanceDeleteRequest.FromString,
                    response_serializer=imrpc_dot_instance__pb2.InstanceResponse.SerializeToString,
            ),
            'InstanceGet': grpc.unary_unary_rpc_method_handler(
                    servicer.InstanceGet,
                    request_deserializer=imrpc_dot_instance__pb2.InstanceGetRequest.FromString,
                    response_serializer=imrpc_dot_instance__pb2.InstanceResponse.SerializeToString,
            ),
            'InstanceList': grpc.unary_unary_rpc_method_handler(
                    servicer.InstanceList,
                    request_deserializer=google_dot_protobuf_dot_empty__pb2.Empty.FromString,
                    response_serializer=imrpc_dot_instance__pb2.InstanceListResponse.SerializeToString,
            ),
            'InstanceLog': grpc.unary_stream_rpc_method_handler(
                    servicer.InstanceLog,
                    request_deserializer=imrpc_dot_instance__pb2.InstanceLogRequest.FromString,
                    response_serializer=imrpc_dot_imrpc__pb2.LogResponse.SerializeToString,
            ),
            'InstanceWatch': grpc.unary_stream_rpc_method_handler(
                    servicer.InstanceWatch,
                    request_deserializer=google_dot_protobuf_dot_empty__pb2.Empty.FromString,
                    response_serializer=google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
            ),
            'InstanceReplace': grpc.unary_unary_rpc_method_handler(
                    servicer.InstanceReplace,
                    request_deserializer=imrpc_dot_instance__pb2.InstanceReplaceRequest.FromString,
                    response_serializer=imrpc_dot_instance__pb2.InstanceResponse.SerializeToString,
            ),
            'VersionGet': grpc.unary_unary_rpc_method_handler(
                    servicer.VersionGet,
                    request_deserializer=google_dot_protobuf_dot_empty__pb2.Empty.FromString,
                    response_serializer=imrpc_dot_imrpc__pb2.VersionResponse.SerializeToString,
            ),
    }
    generic_handler = grpc.method_handlers_generic_handler(
            'imrpc.InstanceService', rpc_method_handlers)
    server.add_generic_rpc_handlers((generic_handler,))


 # This class is part of an EXPERIMENTAL API.
class InstanceService(object):
    """Missing associated documentation comment in .proto file."""

    @staticmethod
    def InstanceCreate(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/imrpc.InstanceService/InstanceCreate',
            imrpc_dot_instance__pb2.InstanceCreateRequest.SerializeToString,
            imrpc_dot_instance__pb2.InstanceResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def InstanceDelete(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/imrpc.InstanceService/InstanceDelete',
            imrpc_dot_instance__pb2.InstanceDeleteRequest.SerializeToString,
            imrpc_dot_instance__pb2.InstanceResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def InstanceGet(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/imrpc.InstanceService/InstanceGet',
            imrpc_dot_instance__pb2.InstanceGetRequest.SerializeToString,
            imrpc_dot_instance__pb2.InstanceResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def InstanceList(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/imrpc.InstanceService/InstanceList',
            google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
            imrpc_dot_instance__pb2.InstanceListResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def InstanceLog(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_stream(request, target, '/imrpc.InstanceService/InstanceLog',
            imrpc_dot_instance__pb2.InstanceLogRequest.SerializeToString,
            imrpc_dot_imrpc__pb2.LogResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def InstanceWatch(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_stream(request, target, '/imrpc.InstanceService/InstanceWatch',
            google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
            google_dot_protobuf_dot_empty__pb2.Empty.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def InstanceReplace(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/imrpc.InstanceService/InstanceReplace',
            imrpc_dot_instance__pb2.InstanceReplaceRequest.SerializeToString,
            imrpc_dot_instance__pb2.InstanceResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def VersionGet(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/imrpc.InstanceService/VersionGet',
            google_dot_protobuf_dot_empty__pb2.Empty.SerializeToString,
            imrpc_dot_imrpc__pb2.VersionResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)
