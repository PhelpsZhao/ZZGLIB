//ZZG_Config.h by Phelps Zhao
//Version 20231001
#ifndef ZZG_CONFIG_H_2310
#define ZZG_CONFIG_H_2310
/*****说明********
//设置一些平台相关的参数
*******************/

#ifndef NULL
#define NULL 0
#endif
//**********编译器设定*************
//MS的VC编译器
#if defined(_MSC_VER)
#define ZZG_MSVC
//GNU的GCC编译器
#elif defined(__GNUC__)
#define ZZG_GNUC
#else 
#error Only MS's or GNU's compiler is supported! 
#endif
//*************END****************

#endif
