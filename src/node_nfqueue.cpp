/*
 * Copyright (C) 2014  Anthony Hinsinger
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <node.h>
#include <node_buffer.h>
#include <node_version.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <arpa/inet.h>
#include <linux/netfilter.h>
#include <linux/ip.h>
#include <string.h>

using namespace v8;

static void PollAsync(uv_poll_t* handle, int status, int events);
//static int nf_callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *data);

class nfqueue : node::ObjectWrap {
  public:
    static void Init(Handle<Object> exports);
    struct nfq_handle* getHandle() { return handle; };
    Persistent<Function> callback;

  private:
    nfqueue() {}
    ~nfqueue() {}

    static v8::Persistent<v8::Function> constructor;
    static Handle<Value> New(const Arguments& args);
    static Handle<Value> Open(const Arguments& args);
    static Handle<Value> Read(const Arguments& args);

    static int nf_callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *data);

    struct nfq_handle *handle;
    struct nfq_q_handle *qhandle;
    char *buffer_data;
    size_t buffer_length;
};

struct RecvBaton {
  uv_poll_t poll;
  nfqueue *queue;
};

Persistent<Function> nfqueue::constructor;

void nfqueue::Init(Handle<Object> exports) {
  // Prepare constructor template
  Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
  tpl->SetClassName(String::NewSymbol("NFQueue"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  tpl->PrototypeTemplate()->Set(String::NewSymbol("open"), FunctionTemplate::New(Open)->GetFunction());
  tpl->PrototypeTemplate()->Set(String::NewSymbol("read"), FunctionTemplate::New(Read)->GetFunction());

  constructor = Persistent<Function>::New(tpl->GetFunction());  
  exports->Set(String::NewSymbol("NFQueue"), constructor);
}

Handle<Value> nfqueue::New(const Arguments& args) {
  HandleScope scope;

  nfqueue* nfqueue_instance = new nfqueue();
  nfqueue_instance->Wrap(args.This());

  return args.This();
}

Handle<Value> nfqueue::Open(const Arguments& args) {
  HandleScope scope;

  nfqueue* obj = ObjectWrap::Unwrap<nfqueue>(args.This());

  if (!args[0]->IsNumber()) {
    ThrowException(Exception::TypeError(String::New("Bad queue number")));
    return scope.Close(Undefined());
  }

  if (!node::Buffer::HasInstance(args[1])) {
    ThrowException(Exception::TypeError(String::New("2nd argument must be a Buffer instance")));
    return scope.Close(Undefined());
  }

#if NODE_VERSION_AT_LEAST(0,3,0)
  Local<Object> buffer_obj = args[1]->ToObject();
  obj->buffer_data = node::Buffer::Data(buffer_obj);
  obj->buffer_length = node::Buffer::Length(buffer_obj);
#else
  node::Buffer *buffer_obj = ObjectWrap::Unwrap<node::Buffer>(args[1]->ToObject());
  obj->buffer_data = buffer_obj->data();
  obj->buffer_length = buffer_obj->length();
#endif

  obj->handle = nfq_open();
  if (obj->handle == NULL) {
    ThrowException(Exception::TypeError(String::New("Unable to open queue")));
    return scope.Close(Undefined());
  }

  if (nfq_unbind_pf(obj->handle, AF_INET)) {
    ThrowException(Exception::TypeError(String::New("Unable to unbind queue")));
    return scope.Close(Undefined());
  }
  nfq_bind_pf(obj->handle, AF_INET);

  obj->qhandle = nfq_create_queue(obj->handle, args[0]->Uint32Value(), &nf_callback, (void*)obj);

  if (obj->qhandle == NULL) {
    ThrowException(Exception::TypeError(String::New("Unable to create queue")));
    return scope.Close(Undefined());
  }

  if (nfq_set_mode(obj->qhandle, NFQNL_COPY_PACKET, 0xffff) < 0) {
    ThrowException(Exception::TypeError(String::New("Unable to set queue mode")));
    return scope.Close(Undefined());
  }

  return scope.Close(Undefined());
}

Handle<Value> nfqueue::Read(const Arguments& args) {
  nfqueue* obj = ObjectWrap::Unwrap<nfqueue>(args.This());

  Handle<Function> cb = Handle<Function>::Cast(args[0]);
  obj->callback = Persistent<Function>::New(cb);

  RecvBaton *baton = new RecvBaton();
  baton->poll.data = baton;
  baton->queue = obj;

  uv_poll_init_socket(uv_default_loop(), &baton->poll, nfq_fd(obj->getHandle()));
  uv_poll_start(&baton->poll, UV_READABLE, PollAsync);

  return Undefined();
}

static void PollAsync(uv_poll_t* handle, int status, int events) {
  char buf[65535];
  RecvBaton *baton = static_cast<RecvBaton*>(handle->data);
  nfqueue* queue = baton->queue;

  int count = recv(nfq_fd(queue->getHandle()), buf, sizeof(buf), 0);
  nfq_handle_packet(queue->getHandle(), buf, count);
  
}

int nfqueue::nf_callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfad, void *data) {
  nfqueue* queue = (nfqueue*)data;
  int id = 0;
  struct nfqnl_msg_packet_hdr *ph;
  int payload_len;
  unsigned char *payload_data;

  ph = nfq_get_msg_packet_hdr(nfad);
  payload_len = nfq_get_payload(nfad, &payload_data);

  if (ph)
    id = ntohl(ph->packet_id);

  // copy payload
  size_t copy_len = payload_len;
  if (copy_len > queue->buffer_length) {
    copy_len = queue->buffer_length;
  }
  memcpy(queue->buffer_data, payload_data, copy_len);

  Local<Object> p = Object::New();
  p->Set(String::NewSymbol("len"), Number::New(payload_len));
  Handle<Value> argv[] = { p };

  Local<Value> reply = queue->callback->Call(Context::GetCurrent()->Global(), 1, argv);

  if (!reply->IsNumber()) {
    return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
  }
  
  return nfq_set_verdict(qh, id, reply->Uint32Value(), 0, NULL);
}

void init(Handle<Object> exports) {
  nfqueue::Init(exports);
}

NODE_MODULE(nfqueue, init)