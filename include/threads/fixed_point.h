#include <stdint.h>

/*
F: fixed point 1 (0 00000000000000001 00000000000000)
INT_MAX: (0 11111111111111111 11111111111111)
INT_MIN: (1 00000000000000000 00000000000000)
매개변수 x, y: fixed_point 형식의 실수
매개변수 n: fixed_point 형식의 정수. 모든 mixed 연산에서 n은 두 번째 파라미터로 들어오게 한다.
*/

#define F (1 << 14)
#define INT_MAX ((1 << 31) - 1)
#define INT_MIN (-(1 << 31))

int int_to_fp (int n) {
    return n * F;
}

int fp_to_int (int x) {
    return x / F;
}

int fp_to_int_round (int x) {
    if (x >= 0) return (x + F / 2) / F;     // F/2 = fixed_point 0.5 (0.5를 더한 후 버림하면 반올림)
    else return (x - F / 2) / F;
}

int add_fp (int x, int y) {
    return x + y;
}

int sub_fp (int x, int y) {
    return x - y;
}

int add_mixed (int x, int n) {
    return x + n * F;
}

int sub_mixed (int x, int n) {
    return x - n * F;
}

int mult_fp (int x, int y) {
    return ((int64_t) x) * y / F;       // x * y는 32bit * 32bit로 32bit를 초과하기 때문에 x를 일시적으로 64bit으로 수정하여 오버플로우 방지
                                        // F로 나눠서 다시 32bit값으로 만들기 (div_fp도 동일)
}

int mult_mixed (int x, int n) {
    return x * n;
}

int div_fp (int x, int y) {
    return ((int64_t) x) * F / y;
}

int div_mixed (int x, int n) {
    return x / n;
}