#pragma once

#include "Common.h"

class ThreadCache
{
public:
	// 申请和释放内存对象
	void* Allocate(size_t size);
	void Deallocate(void* ptr, size_t size);

	// 从中心缓存获取对象
	void* FetchFromCentralCache(size_t index, size_t size);

	// 释放对象时，链表过长时，回收内存回到中心缓存
	void ListTooLong(FreeList& list, size_t size);
private:
	FreeList _freeLists[NFREELIST];
};

//变量在它所在的线程是全局可访问的，但是不能被其他线程访问到
// TLS thread local storage
//static _declspec(thread) ThreadCache* pTLSThreadCache = nullptr;
static thread_local ThreadCache* pTLSThreadCache = nullptr;