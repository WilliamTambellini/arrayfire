/*******************************************************
 * Copyright (c) 2014, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#include <err_opencl.hpp>
#include <memory.hpp>
#include <platform.hpp>
#include <types.hpp>

#include <common/Logger.hpp>
#include <spdlog/spdlog.h>

#include <common/MemoryManagerImpl.hpp>
template class common::MemoryManager<opencl::MemoryManager>;
template class common::MemoryManager<opencl::MemoryManagerPinned>;

#ifndef AF_MEM_DEBUG
#define AF_MEM_DEBUG 0
#endif

#ifndef AF_OPENCL_MEM_DEBUG
#define AF_OPENCL_MEM_DEBUG 0
#endif

using common::bytesToString;
using common::MemoryEventPair;

using std::function;
using std::move;
using std::unique_ptr;

namespace opencl {
void setMemStepSize(size_t step_bytes) {
    memoryManager().setMemStepSize(step_bytes);
}

size_t getMemStepSize(void) { return memoryManager().getMemStepSize(); }

size_t getMaxBytes() { return memoryManager().getMaxBytes(); }

unsigned getMaxBuffers() { return memoryManager().getMaxBuffers(); }

void garbageCollect() { memoryManager().garbageCollect(); }

void printMemInfo(const char *msg, const int device) {
    memoryManager().printInfo(msg, device);
}

template<typename T>
unique_ptr<cl::Buffer, function<void(cl::Buffer *)>> memAlloc(
    const size_t &elements) {
    MemoryEventPair me = memoryManager().alloc(elements * sizeof(T), false);
    if (me.e) me.e.enqueueWait(getQueue()());
    cl::Buffer *ptr = static_cast<cl::Buffer *>(me.ptr);
    return unique_ptr<cl::Buffer, function<void(cl::Buffer *)>>(ptr,
                                                                bufferFree);
}

void *memAllocUser(const size_t &bytes) {
    MemoryEventPair me = memoryManager().alloc(bytes, true);
    if (me.e) me.e.enqueueWait(getQueue()());
    return me.ptr;
}
template<typename T>
void memFree(T *ptr) {
    Event e = make_event(getQueue());
    return memoryManager().unlock((void *)ptr, move(e), false);
}

void memFreeUser(void *ptr) {
    Event e = make_event(getQueue());
    memoryManager().unlock((void *)ptr, move(e), true);
}

cl::Buffer *bufferAlloc(const size_t &bytes) {
    MemoryEventPair me = memoryManager().alloc(bytes, false);
    me.e.enqueueWait(getQueue()());
    return static_cast<cl::Buffer *>(me.ptr);
}

void bufferFree(cl::Buffer *buf) {
    Event e = make_event(getQueue());
    return memoryManager().unlock((void *)buf, move(e), false);
}

void memLock(const void *ptr) { memoryManager().userLock((void *)ptr); }

void memUnlock(const void *ptr) { memoryManager().userUnlock((void *)ptr); }

bool isLocked(const void *ptr) {
    return memoryManager().isUserLocked((void *)ptr);
}

void deviceMemoryInfo(size_t *alloc_bytes, size_t *alloc_buffers,
                      size_t *lock_bytes, size_t *lock_buffers) {
    memoryManager().bufferInfo(alloc_bytes, alloc_buffers, lock_bytes,
                               lock_buffers);
}

template<typename T>
T *pinnedAlloc(const size_t &elements) {
    MemoryEventPair me =
        pinnedMemoryManager().alloc(elements * sizeof(T), false);
    me.e.enqueueWait(getQueue()());
    return static_cast<T *>(me.ptr);
}

template<typename T>
void pinnedFree(T *ptr) {
    Event e = make_event(getQueue());
    return pinnedMemoryManager().unlock((void *)ptr, move(e), false);
}

bool checkMemoryLimit() { return memoryManager().checkMemoryLimit(); }

#define INSTANTIATE(T)                                                         \
    template unique_ptr<cl::Buffer, function<void(cl::Buffer *)>> memAlloc<T>( \
        const size_t &elements);                                               \
    template void memFree(T *ptr);                                             \
    template T *pinnedAlloc(const size_t &elements);                           \
    template void pinnedFree(T *ptr);

INSTANTIATE(float)
INSTANTIATE(cfloat)
INSTANTIATE(double)
INSTANTIATE(cdouble)
INSTANTIATE(int)
INSTANTIATE(uint)
INSTANTIATE(char)
INSTANTIATE(uchar)
INSTANTIATE(intl)
INSTANTIATE(uintl)
INSTANTIATE(short)
INSTANTIATE(ushort)

MemoryManager::MemoryManager()
    : common::MemoryManager<opencl::MemoryManager>(
          getDeviceCount(), common::MAX_BUFFERS,
          AF_MEM_DEBUG || AF_OPENCL_MEM_DEBUG) {
    this->setMaxMemorySize();
}

MemoryManager::~MemoryManager() {
    for (int n = 0; n < opencl::getDeviceCount(); n++) {
        try {
            opencl::setDevice(n);
            this->garbageCollect();
        } catch (AfError err) {
            continue;  // Do not throw any errors while shutting down
        }
    }
}

int MemoryManager::getActiveDeviceId() { return opencl::getActiveDeviceId(); }

size_t MemoryManager::getMaxMemorySize(int id) {
    return opencl::getDeviceMemorySize(id);
}

void *MemoryManager::nativeAlloc(const size_t bytes) {
    auto ptr = (void *)(new cl::Buffer(getContext(), CL_MEM_READ_WRITE, bytes));
    AF_TRACE("nativeAlloc: {} {}", bytesToString(bytes), ptr);
    return ptr;
}

void MemoryManager::nativeFree(void *ptr) {
    AF_TRACE("nativeFree:          {}", ptr);
    delete (cl::Buffer *)ptr;
}

MemoryManagerPinned::MemoryManagerPinned()
    : common::MemoryManager<MemoryManagerPinned>(
          getDeviceCount(), common::MAX_BUFFERS,
          AF_MEM_DEBUG || AF_OPENCL_MEM_DEBUG)
    , pinnedMaps(getDeviceCount()) {
    this->setMaxMemorySize();
}

MemoryManagerPinned::~MemoryManagerPinned() {
    for (int n = 0; n < opencl::getDeviceCount(); n++) {
        opencl::setDevice(n);
        this->garbageCollect();
        auto currIterator = pinnedMaps[n].begin();
        auto endIterator  = pinnedMaps[n].end();
        while (currIterator != endIterator) {
            pinnedMaps[n].erase(currIterator++);
        }
    }
}

int MemoryManagerPinned::getActiveDeviceId() {
    return opencl::getActiveDeviceId();
}

size_t MemoryManagerPinned::getMaxMemorySize(int id) {
    return opencl::getDeviceMemorySize(id);
}

void *MemoryManagerPinned::nativeAlloc(const size_t bytes) {
    void *ptr = NULL;
    cl::Buffer *buf =
        new cl::Buffer(getContext(), CL_MEM_ALLOC_HOST_PTR, bytes);
    ptr = getQueue().enqueueMapBuffer(*buf, true, CL_MAP_READ | CL_MAP_WRITE, 0,
                                      bytes);
    AF_TRACE("Pinned::nativeAlloc: {:>7} {}", bytesToString(bytes), ptr);
    pinnedMaps[opencl::getActiveDeviceId()].emplace(ptr, buf);
    return ptr;
}

void MemoryManagerPinned::nativeFree(void *ptr) {
    AF_TRACE("Pinned::nativeFree:          {}", ptr);
    int n     = opencl::getActiveDeviceId();
    auto map  = pinnedMaps[n];
    auto iter = map.find(ptr);

    if (iter != map.end()) {
        cl::Buffer *buf = map[ptr];
        getQueue().enqueueUnmapMemObject(*buf, ptr);
        delete buf;
        map.erase(iter);
    }
}
}  // namespace opencl
