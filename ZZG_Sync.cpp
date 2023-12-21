//ZZG_Sync.cpp by Phelps Zhao
//Version 20231001
#include "ZZG_Sync.h"
//**********空指令**************

//MS的VC编译器
#if defined(ZZG_MSVC)
#include <intrin.h>
void zNop8()
{
    __nop(); __nop(); __nop(); __nop(); __nop(); __nop(); __nop(); __nop();
}

#elif defined(ZZG_GNUC)
void zNop8()
{
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
    __asm__ __volatile__("nop");
}

#else
#error only MSVC and GCC are supported!
#endif

//**********END*********************
namespace ZZG
{
//获得锁。如果其他线程已经上锁，则线程放弃当前剩下的CPU时间，交给有需要线程执行。在下一个CPU时间段再尝试锁定
void zLock::Lock()
{
    int Count = 3;
    do{
        //测试锁定标志位状态，如果返回clear，说明锁定成功，返回。此条指令设置内存屏障模式为acquire，保证后面受保护的所有读写操作
        //在此指令后才被执行
        if(!aFlag.test_and_set(std::memory_order_acquire))return;
        if(!Count)
        {
            //若3次尝试锁定失败后则放弃CPU执行时间，等待下一个CPU执行时间片再尝试锁定.每轮CPU时间片内尝试次数都为3次
            std::this_thread::yield();
            Count=3;
        }
        //如果得不到锁，那么空转相当于约370个时钟周期后再尝试锁定
        for (int k = 0; k < 37; ++k) { zNop8(); }
        --Count;
    }while(true);
}

//尝试获得锁。如成功获得锁则返回true，否则返回false
bool zLock::TryLock()
{
    //不需要内存屏障，因为程序需要判断锁定是否成功再决定如何执行，有逻辑顺序作为执行顺序的保证
    return !aFlag.test_and_set(std::memory_order_relaxed);
}

#define WRITELOCKMASK 0x1000000
void zRWLock::RLock(void)
{
    //尝试锁定次数
    int Count = 3;
    do {
        //读锁定
        std::atomic_fetch_add_explicit(&Flag,1,std::memory_order_relaxed);
        //锁定后还要查看是否有写锁定，有写锁定的话就要释放读锁定
        if (WriteFlag || Flag.load(std::memory_order_relaxed)&WRITELOCKMASK)
        {
            //释放读锁
            std::atomic_fetch_sub_explicit(&Flag,1,std::memory_order_relaxed);
        }
        else return;

        //三次尝试锁定失败后放弃CPU执行时间，等待下一个CPU执行时间片再尝试锁定，并把Count重置为3，下一轮尝试同样是3次
        if (!Count)
        {
            std::this_thread::yield();
            Count = 3;
        }
        else
        {
            //空转约370个指令周期后再尝试
            for (int k = 0; k < 37; ++k) { zNop8(); }
        }
    } while (true);
}

bool zRWLock::TryRLock(void)
{
    //尝试读锁定。无需内存屏障，一般程序需要逻辑判断决定执行哪些代码。执行顺序由逻辑关系保证
     std::atomic_fetch_add_explicit(&Flag,1,std::memory_order_relaxed);
    //如果有线程写锁定，或者有线程希望写锁定，那么释放读锁，返回false
    if (WriteFlag || Flag.load(std::memory_order_relaxed)&WRITELOCKMASK)
	{
        //释放读锁
        std::atomic_fetch_sub_explicit(&Flag,1,std::memory_order_relaxed);
		return false;
	}
	else return true;
}

//解锁读锁定
void zRWLock::RUnlock(void)
{
    //release模式，保证之前受保护的读写操作在解锁前执行完毕
    std::atomic_fetch_sub_explicit(&Flag,1,std::memory_order_release);
}

void zRWLock::WLock(void)
{
    //设置写锁定标志，供读锁定线程检测，减少总线锁定碰撞概率
	WriteFlag = true;
	int Count = 3;
    uint32_t temp=0;
    do
    {
        //如果Flag值是0，那么设置Flag为WRITELOCKMASK，锁定成功,返回。
        //锁定成功用acquire模式，保证受保护读写操作在锁定后才执行；失败用relaxed模式，对执行顺序不加任何限制
        if(std::atomic_compare_exchange_weak_explicit(&Flag,&temp,WRITELOCKMASK,std::memory_order_acquire,std::memory_order_relaxed))
            return;

        //锁定失败重新设置预锁定标志。有可能别的写线程退出写锁定的时候把WriteFlag设置成false了
        WriteFlag = true;

        //若3次尝试锁定失败后则放弃CPU执行时间，等待下一个CPU执行时间片再尝试锁定。下一个CPU周期开始重置Count为4，开始新一轮循环锁定尝试
        if (Count == 1)
        {
            std::this_thread::yield();
            Count = 3;
        }
        else
            for (int k = 0; k < 37; ++k) { zNop8(); }

    } while (true);
}

bool zRWLock::TryWLock(void)
{
	//如果Flag为0,那么Flag的最高字节设为1，表示写锁定。无需内存屏障。有程序的逻辑顺序保证执行顺序
    return std::atomic_compare_exchange_weak_explicit(&Flag,0,WRITELOCKMASK,std::memory_order_relaxed,std::memory_order_relaxed);
}

void zRWLock::WUnlock(void)
{
    WriteFlag = false;	//先清除写锁标志，提升读线程锁定的几率
    //最高字节清零。不能直接清零。因为有可能读锁定改变了低三个字节的值
    std::atomic_fetch_and_explicit(&Flag,0xffffff,std::memory_order_release);
}

void zRWLock::WToRLock(void)
{
	WriteFlag = false;	//清除写锁标志，提升读线程锁定的几率。如果有写线程在等待写锁定的话，不清除这个标志
	//读写屏障，保证前后读写指令的严格顺序执行。既要保证之前的写操作和之后的写解锁的严格顺序，又要保证之后的读操作不能提前
    std::atomic_fetch_add_explicit(&Flag,1,std::memory_order_acquire);
    //最高字节清零。不能直接清零。因为有可能读锁定改变了低三个字节的值
    std::atomic_fetch_and_explicit(&Flag,0xffffff,std::memory_order_release);
}
}//NAME SPACE ZZG
