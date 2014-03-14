// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "smalloc.h"

#include "env.h"
#include "env-inl.h"
#include "node.h"
#include "node_internals.h"
#include "v8-profiler.h"
#include "v8.h"

#include <string.h>
#include <assert.h>

#define ALLOC_ID (0xA10C)

namespace node {
namespace smalloc {

using v8::Context;
using v8::External;
using v8::ExternalArrayType;
using v8::FunctionCallbackInfo;
using v8::Handle;
using v8::HandleScope;
using v8::HeapProfiler;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::Persistent;
using v8::RetainedObjectInfo;
using v8::Uint32;
using v8::Value;
using v8::WeakCallbackData;
using v8::kExternalUnsignedByteArray;


struct CallbackInfo {
  void* hint;
  FreeCallback cb;
  Persistent<Object> p_obj;
};

void TargetCallback(const WeakCallbackData<Object, char>& data);
void TargetFreeCallback(Isolate* isolate,
                        Persistent<Object>* target,
                        CallbackInfo* cb_info);
void TargetFreeCallback(const WeakCallbackData<Object, CallbackInfo>& data);


// return size of external array type, or 0 if unrecognized
size_t ExternalArraySize(enum ExternalArrayType type) {
  switch (type) {
    case v8::kExternalUnsignedByteArray:
      return sizeof(uint8_t);
    case v8::kExternalByteArray:
      return sizeof(int8_t);
    case v8::kExternalShortArray:
      return sizeof(int16_t);
    case v8::kExternalUnsignedShortArray:
      return sizeof(uint16_t);
    case v8::kExternalIntArray:
      return sizeof(int32_t);
    case v8::kExternalUnsignedIntArray:
      return sizeof(uint32_t);
    case v8::kExternalFloatArray:
      return sizeof(float);   // NOLINT(runtime/sizeof)
    case v8::kExternalDoubleArray:
      return sizeof(double);  // NOLINT(runtime/sizeof)
    case v8::kExternalPixelArray:
      return sizeof(uint8_t);
  }
  return 0;
}


// copyOnto(source, source_start, dest, dest_start, copy_length)
void CopyOnto(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  if (!args[0]->IsObject())
    return env->ThrowTypeError("source must be an object");
  if (!args[2]->IsObject())
    return env->ThrowTypeError("dest must be an object");

  Local<Object> source = args[0].As<Object>();
  Local<Object> dest = args[2].As<Object>();

  if (!source->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowError("source has no external array data");
  if (!dest->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowError("dest has no external array data");

  size_t source_start = args[1]->Uint32Value();
  size_t dest_start = args[3]->Uint32Value();
  size_t copy_length = args[4]->Uint32Value();
  char* source_data = static_cast<char*>(
      source->GetIndexedPropertiesExternalArrayData());
  char* dest_data = static_cast<char*>(
      dest->GetIndexedPropertiesExternalArrayData());

  size_t source_length = source->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType source_type =
    source->GetIndexedPropertiesExternalArrayDataType();
  size_t source_size = ExternalArraySize(source_type);

  size_t dest_length = dest->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType dest_type =
    dest->GetIndexedPropertiesExternalArrayDataType();
  size_t dest_size = ExternalArraySize(dest_type);

  // optimization for Uint8 arrays (i.e. Buffers)
  if (source_size != 1 && dest_size != 1) {
    if (source_size == 0)
      return env->ThrowTypeError("unknown source external array type");
    if (dest_size == 0)
      return env->ThrowTypeError("unknown dest external array type");

    if (source_length * source_size < source_length)
      return env->ThrowRangeError("source_length * source_size overflow");
    if (copy_length * source_size < copy_length)
      return env->ThrowRangeError("copy_length * source_size overflow");
    if (dest_length * dest_size < dest_length)
      return env->ThrowRangeError("dest_length * dest_size overflow");

    source_length *= source_size;
    copy_length *= source_size;
    dest_length *= dest_size;
  }

  // necessary to check in case (source|dest)_start _and_ copy_length overflow
  if (copy_length > source_length)
    return env->ThrowRangeError("copy_length > source_length");
  if (copy_length > dest_length)
    return env->ThrowRangeError("copy_length > dest_length");
  if (source_start > source_length)
    return env->ThrowRangeError("source_start > source_length");
  if (dest_start > dest_length)
    return env->ThrowRangeError("dest_start > dest_length");

  // now we can guarantee these will catch oob access and *_start overflow
  if (source_start + copy_length > source_length)
    return env->ThrowRangeError("source_start + copy_length > source_length");
  if (dest_start + copy_length > dest_length)
    return env->ThrowRangeError("dest_start + copy_length > dest_length");

  memmove(dest_data + dest_start, source_data + source_start, copy_length);
}


// dest will always be same type as source
// for internal use:
//    dest._data = sliceOnto(source, dest, start, end);
void SliceOnto(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  Local<Object> source = args[0].As<Object>();
  Local<Object> dest = args[1].As<Object>();

  assert(source->HasIndexedPropertiesInExternalArrayData());
  assert(!dest->HasIndexedPropertiesInExternalArrayData());

  char* source_data = static_cast<char*>(
      source->GetIndexedPropertiesExternalArrayData());
  size_t source_len = source->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType source_type =
    source->GetIndexedPropertiesExternalArrayDataType();
  size_t source_size = ExternalArraySize(source_type);

  assert(source_size != 0);

  size_t start = args[2]->Uint32Value();
  size_t end = args[3]->Uint32Value();
  size_t length = end - start;

  if (source_size > 1) {
    assert(length * source_size >= length);
    length *= source_size;
  }

  assert(source_data != NULL || length == 0);
  assert(end <= source_len);
  assert(start <= end);

  dest->SetIndexedPropertiesToExternalArrayData(source_data + start,
                                                source_type,
                                                length);
  args.GetReturnValue().Set(source);
}


// for internal use:
//    alloc(obj, n[, type]);
void Alloc(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  Local<Object> obj = args[0].As<Object>();

  // can't perform this check in JS
  if (obj->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowTypeError("object already has external array data");

  size_t length = args[1]->Uint32Value();
  enum ExternalArrayType array_type;

  // it's faster to not pass the default argument then use Uint32Value
  if (args[2]->IsUndefined()) {
    array_type = kExternalUnsignedByteArray;
  } else {
    array_type = static_cast<ExternalArrayType>(args[2]->Uint32Value());
    size_t type_length = ExternalArraySize(array_type);
    assert(type_length * length >= length);
    length *= type_length;
  }

  Alloc(env, obj, length, array_type);
  args.GetReturnValue().Set(obj);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           size_t length,
           enum ExternalArrayType type) {
  size_t type_size = ExternalArraySize(type);

  assert(length <= kMaxLength);
  assert(type_size > 0);

  if (length == 0)
    return Alloc(env, obj, NULL, length, type);

  char* data = static_cast<char*>(malloc(length));
  if (data == NULL) {
    FatalError("node::smalloc::Alloc(v8::Handle<v8::Object>, size_t,"
               " v8::ExternalArrayType)", "Out Of Memory");
  }

  Alloc(env, obj, data, length, type);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           char* data,
           size_t length,
           enum ExternalArrayType type) {
  assert(!obj->HasIndexedPropertiesInExternalArrayData());
  Persistent<Object> p_obj(env->isolate(), obj);
  env->isolate()->AdjustAmountOfExternalAllocatedMemory(length);
  p_obj.SetWeak(data, TargetCallback);
  p_obj.MarkIndependent();
  p_obj.SetWrapperClassId(ALLOC_ID);
  size_t size = length / ExternalArraySize(type);
  obj->SetIndexedPropertiesToExternalArrayData(data, type, size);
}


void TargetCallback(const WeakCallbackData<Object, char>& data) {
  HandleScope handle_scope(data.GetIsolate());
  char* info = data.GetParameter();

  Local<Object> obj = data.GetValue();
  size_t len = obj->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
    obj->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = ExternalArraySize(array_type);
  assert(array_size > 0);
  assert(array_size * len >= len);
  len *= array_size;
  if (info != NULL && len > 0) {
    int64_t change_in_bytes = -static_cast<int64_t>(len);
    data.GetIsolate()->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
    free(info);
  }

  // XXX: Persistent is no longer passed here
  // persistent.Reset();
}


// for internal use: dispose(obj);
void AllocDispose(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  AllocDispose(env, args[0].As<Object>());
}


void AllocDispose(Environment* env, Handle<Object> obj) {
  HandleScope handle_scope(env->isolate());

  if (env->using_smalloc_alloc_cb()) {
    Local<Value> ext_v = obj->GetHiddenValue(env->smalloc_p_string());
    if (ext_v->IsExternal()) {
      Local<External> ext = ext_v.As<External>();
      CallbackInfo* cb_info = static_cast<CallbackInfo*>(ext->Value());
      TargetFreeCallback(env->isolate(), &cb_info->p_obj, cb_info);
      return;
    }
  }

  char* data = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
  size_t length = obj->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
    obj->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = ExternalArraySize(array_type);

  assert(array_size > 0);
  assert(length * array_size >= length);

  length *= array_size;

  if (data != NULL) {
    obj->SetIndexedPropertiesToExternalArrayData(NULL,
                                                 kExternalUnsignedByteArray,
                                                 0);
    free(data);
  }
  if (length != 0) {
    int64_t change_in_bytes = -static_cast<int64_t>(length);
    env->isolate()->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  }
}


void Alloc(Environment* env,
           Handle<Object> obj,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  assert(length <= kMaxLength);

  size_t type_size = ExternalArraySize(type);

  assert(type_size > 0);
  assert(length * type_size >= length);

  length *= type_size;

  char* data = new char[length];
  Alloc(env, obj, data, length, fn, hint, type);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           char* data,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  assert(!obj->HasIndexedPropertiesInExternalArrayData());

  HandleScope handle_scope(env->isolate());
  env->set_using_smalloc_alloc_cb(true);

  CallbackInfo* cb_info = new CallbackInfo;
  cb_info->cb = fn;
  cb_info->hint = hint;
  cb_info->p_obj.Reset(env->isolate(), obj);
  obj->SetHiddenValue(env->smalloc_p_string(),
                      External::New(env->isolate(), cb_info));

  env->isolate()->AdjustAmountOfExternalAllocatedMemory(length +
                                                        sizeof(*cb_info));
  cb_info->p_obj.SetWeak(cb_info, TargetFreeCallback);
  cb_info->p_obj.MarkIndependent();
  cb_info->p_obj.SetWrapperClassId(ALLOC_ID);
  size_t size = length / ExternalArraySize(type);
  obj->SetIndexedPropertiesToExternalArrayData(data, type, size);
}


void TargetFreeCallback(Isolate* isolate,
                        Persistent<Object>* target,
                        CallbackInfo* cb_info) {
  HandleScope handle_scope(isolate);
  Local<Object> obj = PersistentToLocal(isolate, *target);
  char* data = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
  size_t len = obj->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
      obj->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = ExternalArraySize(array_type);
  assert(array_size > 0);
  if (array_size > 1) {
    assert(len * array_size > len);
    len *= array_size;
  }
  int64_t change_in_bytes = -static_cast<int64_t>(len + sizeof(*cb_info));
  isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  cb_info->p_obj.Reset();
  cb_info->cb(data, cb_info->hint);
  delete cb_info;
}


void TargetFreeCallback(const WeakCallbackData<Object, CallbackInfo>& data) {
  CallbackInfo* cb_info = data.GetParameter();
  TargetFreeCallback(data.GetIsolate(), &cb_info->p_obj, cb_info);
}


void HasExternalData(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  args.GetReturnValue().Set(args[0]->IsObject() &&
                            HasExternalData(env, args[0].As<Object>()));
}


bool HasExternalData(Environment* env, Local<Object> obj) {
  return obj->HasIndexedPropertiesInExternalArrayData();
}


class RetainedAllocInfo: public RetainedObjectInfo {
 public:
  explicit RetainedAllocInfo(Handle<Value> wrapper);

  virtual void Dispose();
  virtual bool IsEquivalent(RetainedObjectInfo* other);
  virtual intptr_t GetHash();
  virtual const char* GetLabel();
  virtual intptr_t GetSizeInBytes();

 private:
  static const char label_[];
  char* data_;
  int length_;
};


const char RetainedAllocInfo::label_[] = "smalloc";


RetainedAllocInfo::RetainedAllocInfo(Handle<Value> wrapper) {
  Local<Object> obj = wrapper->ToObject();
  length_ = obj->GetIndexedPropertiesExternalArrayDataLength();
  data_ = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
}


void RetainedAllocInfo::Dispose() {
  delete this;
}


bool RetainedAllocInfo::IsEquivalent(RetainedObjectInfo* other) {
  return label_ == other->GetLabel() &&
         data_ == static_cast<RetainedAllocInfo*>(other)->data_;
}


intptr_t RetainedAllocInfo::GetHash() {
  return reinterpret_cast<intptr_t>(data_);
}


const char* RetainedAllocInfo::GetLabel() {
  return label_;
}


intptr_t RetainedAllocInfo::GetSizeInBytes() {
  return length_;
}


RetainedObjectInfo* WrapperInfo(uint16_t class_id, Handle<Value> wrapper) {
  return new RetainedAllocInfo(wrapper);
}


void Initialize(Handle<Object> exports,
                Handle<Value> unused,
                Handle<Context> context) {
  Environment* env = Environment::GetCurrent(context);

  NODE_SET_METHOD(exports, "copyOnto", CopyOnto);
  NODE_SET_METHOD(exports, "sliceOnto", SliceOnto);

  NODE_SET_METHOD(exports, "alloc", Alloc);
  NODE_SET_METHOD(exports, "dispose", AllocDispose);

  NODE_SET_METHOD(exports, "hasExternalData", HasExternalData);

  exports->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kMaxLength"),
               Uint32::NewFromUnsigned(env->isolate(), kMaxLength));

  HeapProfiler* heap_profiler = env->isolate()->GetHeapProfiler();
  heap_profiler->SetWrapperClassInfoProvider(ALLOC_ID, WrapperInfo);
}


}  // namespace smalloc
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(smalloc, node::smalloc::Initialize)