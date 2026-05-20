#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* Sabit noktalı sayıları 16.14 formatında tutuyoruz. */
#define F (1 << 14)

/* Tamsayıyı fixed-point'e çevir */
#define CONVERT_N_TO_FP(n) ((n) * F)

/* Fixed-point sayıyı tamsayıya çevir (yuvarlama yaparak) */
#define CONVERT_FP_TO_INT_ROUND(x) ((x) >= 0 ? ((x) + F / 2) / F : ((x) - F / 2) / F)

/* İki fixed-point sayıyı topla */
#define ADD_FP(x, y) ((x) + (y))

/* Fixed-point sayı ile tamsayıyı topla */
#define ADD_MIXED(x, n) ((x) + (n) * F)

/* İki fixed-point sayıyı çıkar */
#define SUB_FP(x, y) ((x) - (y))

/* Fixed-point sayıdan tamsayı çıkar */
#define SUB_MIXED(x, n) ((x) - (n) * F)

/* İki fixed-point sayıyı çarp */
#define MUL_FP(x, y) (((int64_t)(x)) * (y) / F)

/* Fixed-point sayıyı tamsayı ile çarp */
#define MUL_MIXED(x, n) ((x) * (n))

/* İki fixed-point sayıyı böl */
#define DIV_FP(x, y) (((int64_t)(x)) * F / (y))

/* Fixed-point sayıyı tamsayıya böl */
#define DIV_MIXED(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */
