#include<libtransistor/cpp/types.hpp>
#include<libtransistor/cpp/ipcserver.hpp>
#include<libtransistor/cpp/ipc/sm.hpp>
#include<libtransistor/types.h>
#include<libtransistor/ipcserver.h>

#include<stdio.h>

#include<functional>

namespace Transistor {
namespace IPCServer {

void object_dispatch_shim(ipc_server_object_t *obj, ipc_message_t *msg, uint32_t request_id) {
	Object *object = (Object*) obj->userdata;
	IPC::Message message;
	message.msg = *msg;
	ResultCode r = object->Dispatch(message, request_id);
	if(!r.IsOk()) {
		ipc_server_session_close(obj->owning_session);
	}
}

void object_destruct_shim(ipc_server_object_t *obj) {
	delete (Object*) obj->userdata;
}

Object::Object(IPCServer *server) : server(server) {
	object.userdata = this;
	object.dispatch = object_dispatch_shim;
	object.close = object_destruct_shim;
}

Object::~Object() {
}

TransactionFormat::~TransactionFormat() {
	for(auto i = buffers.begin(); i != buffers.end(); i++) {
		delete (*i);
	}
	if(rq.raw_data) { delete[] rq.raw_data; }
	if(rq.objects)  { delete[] rq.objects; }
	if(rs.raw_data) { delete[] rs.raw_data; }
	if(rs.objects)  { delete[] rs.objects; }
	if(out_objects) { delete[] out_objects; }
}

Result<IPCServer> IPCServer::Create() {
	ipc_server_t *server = new ipc_server_t;
	return ResultCode::ExpectOk(ipc_server_create(server))
		.map([server](auto const &v) -> IPCServer {
				IPCServer cpp_server(server);
				return cpp_server;
			})
		.map_error([server](auto const &v) {
				delete server;
				return v;
			});
}

IPCServer::IPCServer(IPCServer &&other) {
	this->server = other.server;
	other.server = nullptr;
}

IPCServer &IPCServer::operator=(IPCServer &&other) {
	this->server = other.server;
	other.server = nullptr;
	return *this;
}

IPCServer::~IPCServer() {
	for(auto i = factories.begin(); i != factories.end(); i++) {
		delete *i;
	}
	if(server != nullptr) {
		ipc_server_destroy(server);
		delete server;
	}
}

static result_t factory_shim(ipc_server_object_t **obj, void *userdata) {
	printf("in factory shim: %p\n", userdata);
	std::function<Result<Object*>()> *factory = static_cast<std::function<Result<Object*>()>*>(userdata);
	Result<Object*> object = (*factory)();
	if(object.has_value()) {
		printf("succeeded\n");
		*obj = &(object.value()->object);
		return RESULT_OK;
	} else {
		printf("failed\n");
		return object.error().code;
	}
}

Result<std::nullopt_t> IPCServer::CreateService(const char *name, std::function<Result<Object*>(IPCServer *server)> factory_src) {
	std::function<Result<Object*>()> *factory = new std::function<Result<Object*>()>(std::bind(factory_src, this));
	factories.push_front(factory);
	return IPC::SM::Initialize().and_then([name](auto sm) -> Result<KPort> {
			return sm.RegisterService(name, 64);
		}).and_then([this, factory](auto port) -> Result<std::nullopt_t> {
				return ResultCode::ExpectOk(ipc_server_add_port(server, port.Claim(), factory_shim, factory));
			}).map_error([this, factory](auto err) {
					factories.remove(factory);
					delete factory;
					return err;
				});
}

Result<std::nullopt_t> IPCServer::Process(uint64_t timeout) {
	return ResultCode::ExpectOk(ipc_server_process(server, timeout));
}

IPCServer::IPCServer(ipc_server_t *server) : server(server) {
}

}
}
