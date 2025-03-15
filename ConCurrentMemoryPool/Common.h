#pragma once

#include <iostream>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <stdexcept>  // 引入标准异常库

#include <time.h>
#include <assert.h>

#include <thread>
#include <mutex>
#include <atomic>

using std::cout;
using std::endl;

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>  // 引入 unistd.h 用于 sbrk
#endif

static const size_t MAX_BYTES = 256 * 1024;
//thread cache中哈希桶的个数
static const size_t NFREELIST = 208;
//page cache中哈希桶的个数
static const size_t NPAGES = 129;
//页大小转换偏移，即一页定义为2^13，也就是8KB
static const size_t PAGE_SHIFT = 13;

#ifdef _WIN64
	typedef unsigned long long PAGE_ID;
#elif _WIN32
	typedef size_t PAGE_ID;
#else
	// linux
	typedef size_t PAGE_ID;
#endif

// 直接去堆上按页申请空间
inline static void* SystemAlloc(size_t kpage)
{
#ifdef _WIN32
	void* ptr = VirtualAlloc(0, kpage << 13, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	
#else
	// linux下brk mmap等
	void* ptr = mmap(nullptr, kpage << 13, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED) {
		ptr = nullptr;
#endif
	if (ptr == nullptr)
		throw std::bad_alloc();
	return ptr;
}


inline static void SystemFree(void* ptr)
{
#ifdef _WIN32
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// sbrk unmmap等
	if(munmap(ptr, MAX_BYTES) == -1) {
		// 处理释放失败的情况
		std::cerr << "Failed to free memory using munmap." << std::endl;
	}
#endif
}

// 获取对象的下一个指针
static void*& NextObj(void* obj)
{
	return *(void**)obj;
}


// 管理切分好的小对象的自由链表
class FreeList
{
public:
	// 向自由链表中插入一个对象
	void Push(void* obj)
	{
		assert(obj);

		// 头插
		//*(void**)obj = _freeList;
		NextObj(obj) = _freeList;
		_freeList = obj;

		++_size;
	}

	// 向自由链表中插入一段对象
	void PushRange(void* start, void* end, size_t n)
	{
		NextObj(end) = _freeList;
		_freeList = start;

		// 测试验证+条件断点
		/*int i = 0;
		void* cur = start;
		while (cur)
		{
			cur = NextObj(cur);
			++i;
		}

		if (n != i)
		{
			int x = 0;
		}*/

		_size += n;
	}
	
	// 从自由链表中取出一段对象
	void PopRange(void*& start, void*& end, size_t n)
	{
		assert(n <= _size);
		start = _freeList;
		end = start;

		for (size_t i = 0; i < n - 1; ++i)
		{
			end = NextObj(end);
		}

		_freeList = NextObj(end);
		NextObj(end) = nullptr;
		_size -= n;
	}

	// 从自由链表中取出一个对象
	void* Pop()
	{
		assert(_freeList);

		// 头删
		void* obj = _freeList;
		_freeList = NextObj(obj);
		--_size;

		return obj;
	}

	bool Empty()
	{
		return _freeList == nullptr;
	}

	// 获取最大大小的引用
	size_t& MaxSize()
	{
		return _maxSize;
	}

	// 获取自由链表的大小
	size_t Size()
	{
		return _size;
	}

private:
	void* _freeList = nullptr;
	size_t _maxSize = 1;
	size_t _size = 0;
};

// 计算对象大小的对齐映射规则
class SizeClass
{
public:
	// 整体控制在最多10%左右的内碎片浪费
	// [1,128]					8byte对齐	    freelist[0,16)
	// [128+1,1024]				16byte对齐	    freelist[16,72)
	// [1024+1,8*1024]			128byte对齐	    freelist[72,128)
	// [8*1024+1,64*1024]		1024byte对齐     freelist[128,184)
	// [64*1024+1,256*1024]		8*1024byte对齐   freelist[184,208)

	/*size_t _RoundUp(size_t size, size_t alignNum)
	{
		size_t alignSize;
		if (size % alignNum != 0)
		{
			alignSize = (size / alignNum + 1)*alignNum;
		}
		else
		{
			alignSize = size;
		}

		return alignSize;
	}*/
	// 1-8 
	// 向上对齐后的字节数
	static inline size_t _RoundUp(size_t bytes, size_t alignNum)
	{
		return ((bytes + alignNum - 1) & ~(alignNum - 1));
	}
	/*对于上述位运算，我们以10字节按8字节对齐为例进行分析。
	* 8 − 1 = 7 8 - 1 = 78−1 = 7，7就是一个低三位为1其余位为0的二进制序列，我们将10与7相加，
	* 相当于将10字节当中不够8字节的剩余字节数补上了。
	*     01010
	*	 + 00111
	* ————————
	*      10001
	* 
	* 然后我们再将该值与7按位取反后的值进行与运算，
	* 而7按位取反后是一个低三位为0其余位为1的二进制序列，
	* 该操作进行后相当于屏蔽了该值的低三位而该值的其余位保持不变，
	* 此时得到的值就是10字节按8字节对齐后的值，即16字节。
	*	10001
	* & 11000
	* ——————
	*   10000
	*/

	// 根据对象大小进行向上对齐
	static inline size_t RoundUp(size_t size)
	{
		if (size <= 128)
		{
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			return _RoundUp(size, 16);
		}
		else if (size <= 8 * 1024)
		{
			return _RoundUp(size, 128);
		}
		else if (size <= 64 * 1024)
		{
			return _RoundUp(size, 1024);
		}
		else if (size <= 256 * 1024)
		{
			return _RoundUp(size, 8 * 1024);
		}
		else
		{
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
	}

	/*size_t _Index(size_t bytes, size_t alignNum)
	{
	if (bytes % alignNum == 0)
	{
	return bytes / alignNum - 1;
	}
	else
	{
	return bytes / alignNum;
	}
	}*/

	// 1 + 7  8
	// 2      9
	// ...
	// 8      15

	// 9 + 7 16
	// 10
	// ...
	// 16    23
	
	// 计算索引
	//不是传入该字节数的对齐数，而是将对齐数写成2的n次方的形式后，
	// 将这个n值进行传入。比如对齐数是8，传入的就是3。
	static inline size_t _Index(size_t bytes, size_t align_shift)
	{
		return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
	}
	/*以10字节按8字节对齐为例进行分析。
	此时传入的alignShift就是3，将1左移3位后得到的实际上就是对齐数8，8 − 1 = 7 8 - 1 = 78−1 = 7，
	相当于我们还是让10与7相加。
		 01010
	   + 00111
	————————
		 10001
	再将该值向右移3位，实际上就是让这个值除以8，
	相当于屏蔽了该值二进制的低三位，因为除以8得到的值与其二进制的低三位无关，
	将10对齐后的字节数除以了8，此时得到了2，
	而最后还需要减一是因为数组的下标是从0开始的。
	
	*/


	// 计算映射的哪一个自由链表桶
	static inline size_t Index(size_t bytes)
	{
		assert(bytes <= MAX_BYTES);

		// 每个区间有多少个链
		static int group_array[4] = { 16, 56, 56, 56 };
		if (bytes <= 128) {
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024) {
			return _Index(bytes - 128, 4) + group_array[0];
		}
		else if (bytes <= 8 * 1024) {
			return _Index(bytes - 1024, 7) + group_array[1] + group_array[0];
		}
		else if (bytes <= 64 * 1024) {
			return _Index(bytes - 8 * 1024, 10) + group_array[2] + group_array[1] + group_array[0];
		}
		else if (bytes <= 256 * 1024) {
			return _Index(bytes - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0];
		}
		else {
			assert(false);
		}

		return -1;
	}

	// 一次thread cache从中心缓存获取多少个
	static size_t NumMoveSize(size_t size)
	{
		assert(size > 0);

		//慢开始反馈调节算法
		// [2, 512]，一次批量移动多少个对象的(慢启动)上限值
		// 小对象一次批量上限高
		// 小对象一次批量上限低
		int num = MAX_BYTES / size;
		if (num < 2)
			num = 2;

		if (num > 512)
			num = 512;

		return num;
	}

	// central cache一次向page cache获取多少页
	// 单个对象 8byte
	// ...
	// 单个对象 256KB
	static size_t NumMovePage(size_t size)
	{
		size_t num = NumMoveSize(size);//计算出thread cache一次向central cache申请对象的个数上限
		size_t npage = num * size;//num个size大小的对象所需的字节数

		npage >>= PAGE_SHIFT;//将字节数转换为页数
		if (npage == 0)//至少一页
			npage = 1;

		return npage;
	}
};

// 管理多个连续页大块内存跨度结构
struct Span
{
	PAGE_ID _pageId = 0; // 大块内存起始页的页号
	size_t  _n = 0;      // 页的数量

	Span* _next = nullptr;	// 双向链表的结构
	Span* _prev = nullptr;

	size_t _objSize = 0;  // 切好的小对象的大小
	size_t _useCount = 0; // 切好小块内存，被分配给thread cache的计数
	void* _freeList = nullptr;  // 切好的小块内存的自由链表

	bool _isUse = false;          // 是否在被使用
};

// 带头双向循环链表 
class SpanList
{
public:
	// 构造函数，初始化头节点
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}

	// 获取链表的起始位置
	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	bool Empty()
	{
		return _head->_next == _head;
	}

	void PushFront(Span* span)
	{
		Insert(Begin(), span);
	}

	Span* PopFront()
	{
		Span* front = _head->_next;
		Erase(front);
		return front;
	}

	// 在指定位置插入一个新的 Span
	void Insert(Span* pos, Span* newSpan)
	{
		assert(pos);
		assert(newSpan);

		Span* prev = pos->_prev;
		// prev newspan pos
		prev->_next = newSpan;
		newSpan->_prev = prev;
		newSpan->_next = pos;
		pos->_prev = newSpan;
	}

	// 移除指定位置的 Span
	void Erase(Span* pos)
	{
		assert(pos);
		assert(pos != _head);

		// 1、条件断点
		// 2、查看栈帧
		/*if (pos == _head)
		{
		int x = 0;
		}*/

		Span* prev = pos->_prev;
		Span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;
	}

private:
	Span* _head;
	//static ObjectPool<Span> _spanpool;
public:
	std::mutex _mtx; // 桶锁
};