// 用 Zig（clang + mingw-w64）编这个项目时，编译前强制包含这一个头。
//
// 它补的是**别人的代码**，不是项目的：一堆库把「Windows」当成了「MSVC」，在 _WIN32 分支里直接
// 用只有 MSVC 才有的东西。往这个头里加东西，比去改 vendored 源码好 —— 那些目录是从上游抄来的，
// 改了就跟上游分家，下次升级要重新打一遍补丁。
//
// 规矩两条：
//   1. **写成函数，不要写成宏。** mingw 自己的 psdk_inc/intrin-impl.h 就是拿同名函数式宏搭起来
//      的，一个同名的对象式宏会让它报「宏参数个数不对」—— 缺符号的诊断会变成完全看不懂的诊断。
//   2. **一个头都别引。** 上面那条也是这个原因：早引一个头，就会在 intrin-impl.h 之前把它的宏
//      机制搅乱。要什么自己写。
#pragma once

#if defined(_WIN32) && !defined(_MSC_VER)

#if defined(__i386__) || defined(__x86_64__)
// xbyak（dynarmic 捆的汇编器）在 _WIN32 上直接调 __cpuidex，但没引它所在的头。MSVC 从编译器拿
// 这个名字，GNU 目标下 clang 不提供 —— 同一条指令而已，自己写。
static inline void __cpuidex(int data[4], int eax, int ecx) {
    __asm__ __volatile__("cpuid"
                         : "=a"(data[0]), "=b"(data[1]), "=c"(data[2]), "=d"(data[3])
                         : "a"(eax), "c"(ecx));
}
#endif

#endif  // _WIN32 && !_MSC_VER
