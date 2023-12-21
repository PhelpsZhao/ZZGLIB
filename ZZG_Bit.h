//ZZG_Bit.h by Phelps Zhao
//Version 20231001
#ifndef ZZG_BIT_H_2310
#define ZZG_BIT_H_2310
/*****说明***************
一些必要的快速位运算函数
*******************************/
#include "ZZG_Config.h"
#include <type_traits>
#include <stdint.h>
namespace ZZG {

//1000011100001110000111000011100001110000111000011100001110000111
#define MASK64_73	0x870E1C3870E1C387
//1011101101110110111011011101101110110111011011101101110110111011
#define MASK64_732	0xBB76EDDBB76EDDBB
//1001100100110010011001001100100110010011001001100100110010011001
#define MASK64_721  0x993264C993264C99
//0000100000010000001000000100000010000001000000100000010000001000
#define MASK64_7h   0x810204081020408
//模版函数zBitCount统计一个二进制数中包含的1的个数。这个数可以是布尔值、字符、整数、浮点数、指针等。但是这个数的长度必须是
//1字节、2字节、4字节或者8字节，否则编译出错。算法基于无符号整数，其它所有类型用无符号整数指针存取操作
template <typename T,typename=std::enable_if_t<sizeof(T)==8 ||sizeof(T)==4 ||sizeof(T)==2||sizeof(T)==1,T>>
inline uint16_t zBitCount(T n)
{
    if constexpr(sizeof(T)==8)
    {
        //只有T为无符号整数(包括bool值)时std::is_unsigned_v才是true
        if constexpr(std::is_unsigned_v<T>)
        {
            uint64_t tmp = n - ((n >> 1)&MASK64_732) - ((n >> 2)&MASK64_721)-((n>>3)&MASK64_7h);
            tmp = (tmp + (tmp >> 3))&MASK64_73;
            return (tmp % 127);
        }
        else
        {
            uint64_t tmp=*(uint64_t*)&n;
            tmp = tmp - ((tmp >> 1)&MASK64_732) - ((tmp >> 2)&MASK64_721)-((tmp>>3)&MASK64_7h);
            tmp = (tmp + (tmp >> 3))&MASK64_73;
            return (tmp % 127);
        }
    }
    else if constexpr(sizeof(T)==4)
    {
        if constexpr(std::is_unsigned_v<T>)
        {
            //下面几个模数是8进制
            uint32_t tmp = n - ((n >> 1) & 033333333333) - ((n >> 2) & 011111111111);
            tmp = (tmp + (tmp >> 3)) & 030707070707;
            return (tmp % 63);
        }
        else
        {
            uint32_t tmp=*(uint32_t*)&n;
            tmp = tmp - ((tmp >> 1) & 033333333333) - ((tmp >> 2) & 011111111111);
            tmp = (tmp + (tmp >> 3)) & 030707070707;
            return (tmp % 63);
        }
    }
    else if constexpr(sizeof(T)==2)
    {
        if constexpr(std::is_unsigned_v<T>)
        {
            uint16_t tmp = n - ((n >> 1) & 0133333) - ((n >> 2) & 0111111);
            tmp = (tmp + (tmp >> 3)) & 070707;
            return (tmp % 63);
        }
        else
        {
            uint16_t tmp=*(uint16_t*)&n;
            tmp= tmp - ((tmp >> 1) & 0133333) - ((tmp >> 2) & 0111111);
            tmp = (tmp + (tmp >> 3)) & 070707;
            return (tmp % 63);
        }
    }
    else
    {
        if constexpr(std::is_unsigned_v<T>)
        {
            n=((n>>1)&0x55)+(n&0x55);
            n=(n&0x33)+((n>>2)&0x33);
            n=(n&0xf)+((n>>4)&0xf);
            return n;
        }
        else
        {
            uint8_t tmp=*(uint8_t*)&n;
            tmp=((tmp>>1)&0x55)+(tmp&0x55);
            tmp=(tmp&0x33)+((tmp>>2)&0x33);
            tmp=(tmp&0xf)+((tmp>>4)&0xf);
            return tmp;
        }
    }
}

#if defined(ZZG_MSVC)
//从最低位开始扫描第一个是1的位。32位版本
//@paras[Index:out]:扫描到的置1的位索引号，低位第一位是0位，往高位依次为1,2,3...
//@paras[x:in]需要扫描的数字
//@ret:如果发现了1，那么返回1.否则返回0。
inline uint16_t zBSF(unsigned long * Index, uint32_t x)
{
    return _BitScanForward(Index, x);
}
//从最高位开始扫描第一个是1的位。32位版本
inline uint16_t zBSR(unsigned long * Index, uint32_t x)
{
    return _BitScanReverse(Index, x);
}
#endif

#if defined(ZZG_GNUC)
//从最低位开始扫描第一个是1的位。32位版本
//@paras[Index:out]:扫描到的置1的位索引号，低位第一位是0位，往高位依次为1,2,3...
//@paras[x:in]需要扫描的数字
//@ret:如果发现了1，那么返回1.否则返回0。
inline int zBSF(unsigned long *Index, uint32_t x)
{
    *Index = __builtin_ffs(x) - 1;
	if (!x)return 0;
	else return 1;
}
//从最高位开始扫描第一个是1的位。32位版本
inline  uint16_t zBSR(unsigned long * Index, uint32_t x)
{
    if(!x)return 0;
    *Index=31-__builtin_clz ( x);
    return 1;
}
#
#endif

//zBitSet64的32位版本
inline void zBitSet(uint32_t *x, uint16_t Index)
{
	*x |= ((uint32_t)0x1 << Index);
}
//指定位置0，最低位为0位
inline void zBitReset64(uint64_t *x, uint16_t Index)
{
    *x &= ~(0x1 << Index);
}
//zBitReset64的32位版本
inline void zBitReset(uint32_t *x, uint16_t Index)
{
	*x &= ~(1U << Index);
}

//测试指定位，返回值为指定位的值
inline uint8_t zBitTest64(uint64_t *x, uint16_t Index)
{
    return (*x >> Index) & 0x1;
}

//测试指定位，返回值为指定位的值
inline uint8_t zBitTest(uint32_t *x, uint16_t Index)
{
    return (*x >> Index) &0x1;
}

}//NAME SPACE ZZG
#endif
