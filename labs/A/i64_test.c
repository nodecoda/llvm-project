
#include <stdint.h>

// 1. 64位加法 (核心：进位链)
uint64_t add64(uint64_t a, uint64_t b) {
    return a + b;
}

// 2. 64位比较 (核心：先比 hi 再比 lo)
int cmp64_signed(int64_t a, int64_t b) {
    return a < b;
}

// 3. 64位变量移位 (核心：>=32 位的分支处理)
uint64_t shl64(uint64_t x, unsigned amt) {
    return x << amt;
}

// 4. 64位乘法 (核心：展开为部分积累加)
uint64_t mul64(uint64_t a, uint64_t b) {
    return a * b;
}

