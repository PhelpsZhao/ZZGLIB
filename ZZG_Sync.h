//ZZG_Sync.h by Phelps Zhao
//Version 20231001
#ifndef ZZG_SYNC_H_2310
#define ZZG_SYNC_H_2310
#include <atomic>
#include <thread>
#include "ZZG_Config.h"
/******使用说明********

*********************/
extern void zNop8();
namespace ZZG {

//等待对象的值到某个预设值。等待期间，线程阻塞
template <typename T>
void zWaitUntil(volatile T &Object, T EndValue)
{
	int count = 3;
	do {
		if (!count)
		{
            std::this_thread::yield();
			count = 3;
		}
		if (Object == EndValue)
			break;
		--count;
		for (int i = 0; i < 31; ++i)
            zNop8();
	} while (1);
}

//自旋锁
class zLock {
    //上锁标志，clear表示无锁，set表示上锁
	std::atomic_flag aFlag; //从C++20开始构造函数会自动把变量设置为clear状态,C++11和C++14还是需要显式初始化的
public:
    zLock()
    {
		aFlag.clear(std::memory_order_relaxed);
	}
	//获得锁。如果其他线程已经上锁，则线程放弃当前剩下的CPU时间，交给有需要线程执行。在下一个CPU时间段再尝试锁定
	void Lock(void);
    //尝试获得锁。如成功获得锁则返回true，否则返回false.
    //如果其它线程已经上锁，函数不会等待，立即返回
	bool TryLock(void);

	// 释放锁。可以重复调用
	void Unlock(void)
	{
        //释放上锁标志，同时设置内存屏障模式为release,保证受保护的所有读写操作在此指令执行前完成
        aFlag.clear(std::memory_order_release);
	}
};

/*版本控制顺序锁原理
 *版本顺序锁主要用在对保护数据的读取频率远大于修改频率的场合
整数Version用来表示数据的修改版本号。初始化为偶数（一般为0）。每次修改之前加1，修改完成后再加1。
当读取数据的时候，读取前后要读取Version，两次比较Version的值是否一致。如果一致，且为偶数，那么
数据读取有效。如果不一致，或者虽然一致但是是奇数那么表示读取期间有修改在进行，需要重读
顺序锁相比zLock和zCRWLock，主要是读数据的时候锁定开销相对比较小，读并发能力较强，且写锁不会被读操作堵塞，可以随时写锁定。
顺序锁读数据的代码形式：
int version;
do{
Version=zCSeqLock::ReadBegin();
//读取代码
}while(zCSeqLock::ReadRetry(Version));
而修改数据的话必须先用WLock()锁定，修改完成后调用WUnLock()函数解锁
*/

class zSeqLock {
    zLock lock;   //用来保证写线程的独占性
    volatile std::atomic<int> Version;  //版本号必须使用原子类型，保证数字完整性
public:
	zSeqLock()
    {
        Version.store(0,std::memory_order_relaxed);
	}

	//开始读数据，如果没有写锁定，那么返回版本号
	int ReadBegin()
    {
        //acquire保证本函数之后的读写代码不会被重排到本指令之前
        int ret = Version.load(std::memory_order_acquire);
		//如果Version是奇数，说明有修改线程锁定，数据不稳定，重读等待
        if (ret & 0x1)
		{
			int Count = 10;
			do {
                for (int i = 0; i < 10; ++i) { zNop8(); }
				if (!(--Count))
				{
                    std::this_thread::yield();
					Count = 10;
				}
                ret = Version.load(std::memory_order_relaxed);
            } while (ret & 0x1);
        }
		return ret;
	}

	//重读比较版本号，如果相同返回0，否则非0
	int ReadRetry(int &StartVersion)
	{
        //release内存屏障，保证本函数之前的代码不会被重排到Version.load操作之后
        std::atomic_thread_fence(std::memory_order_release);
        int temp=Version.load(std::memory_order_relaxed);
        return StartVersion^temp;	//按位异或。相等则返回0，不等返回非0；
	}

	//写锁定
	void WLock()
	{
        lock.Lock();
        //acquire模式保证Version值改变发生在后续所有读写操作之前
        std::atomic_fetch_add_explicit(&Version,1,std::memory_order_acquire);
	}
	//写解锁
	void WUnlock()
    {
        std::atomic_fetch_sub_explicit(&Version,1,std::memory_order_release);
		lock.Unlock();
	}
};




/*****读写锁******
1、可以同时多个线程拥有读锁，但同时只能有一个线程拥有写锁
2、有线程拥有写锁的时候，读锁不能锁定；有线程拥有读锁的时候，写锁不能锁定
*******/
class zRWLock {
	//上锁标志。低三个字节表示读锁锁定次数，每一次读锁定，就增1，解锁读锁就减一。读锁定是共享的，最多同时可以有2^24个线程同时读
	//最高字节表示写锁，0表示没有写锁定，1表示写锁定。写锁定是独占的，每次只能有一个线程可以写锁定
    //当有读锁的时候，不能写锁定。
    std::atomic_uint32_t Flag;
    //有写线程准备锁定写锁的标志，上读锁的时候如果有这个标志就不上，等待写锁完成。以防读锁频繁的时候，写锁上不了
    bool WriteFlag;
public:
	zRWLock()
	{
		WriteFlag = false;
        Flag = 0;
	}
	//读锁定，可以同时多次锁定。为共享锁
	void RLock(void);
	//尝试读锁定。锁定成功返回true。若有其他线程试图写锁定或者已经写锁定，那么返回false
	bool TryRLock(void);
	//解锁读锁定
	void RUnlock(void);
	//写锁定。只允许同时一个线程锁定，为排它锁
	void WLock(void);
	//尝试写锁定。锁定成功返回true。若已经有写锁定或者读锁定，那么返回false
	bool TryWLock(void);
	//解锁写锁定
	void WUnlock(void);

	//写锁降级为读锁
	void WToRLock(void);
};

}
#endif//!ZZG_SYNC_H_2310
