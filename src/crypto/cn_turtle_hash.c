// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers
// Copyright (c) 2014-2025, The Monero Project
// Copyright (c) 2014-2025, The Aeon Project
// Copyright (c) 2018, The TurtleCoin Developers
//
// Please see the included LICENSE file for more information.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "int-util.h"
#include "hash-ops.h"
#include "oaes_lib.h"
#include "variant2_int_sqrt.h"

// Standard Crypto Definitions
#define AES_BLOCK_SIZE         16
#define AES_KEY_SIZE           32
#define INIT_SIZE_BLK          8
#define INIT_SIZE_BYTE         (INIT_SIZE_BLK * AES_BLOCK_SIZE)

extern int aesb_single_round(const uint8_t *in, uint8_t*out, const uint8_t *expandedKey);
extern int aesb_pseudo_round(const uint8_t *in, uint8_t *out, const uint8_t *expandedKey);

#define VARIANT1_1(p) \
  do if (variant == 1) \
  { \
    const uint8_t tmp = ((const uint8_t*)(p))[11]; \
    static const uint32_t table = 0x75310; \
    const uint8_t index = (((tmp >> 3) & 6) | (tmp & 1)) << 1; \
    ((uint8_t*)(p))[11] = tmp ^ ((table >> index) & 0x30); \
  } while(0)

#define VARIANT1_2(p) \
  do if (variant == 1) \
  { \
    xor64(p, tweak1_2); \
  } while(0)

#define VARIANT1_CHECK() \
  do if (length < 43) \
  { \
    fprintf(stderr, "Cryptonight variant 1 need at least 43 bytes of data"); \
    abort(); \
  } while(0)

#define NONCE_POINTER (((const uint8_t*)data)+35)

#define VARIANT1_PORTABLE_INIT() \
  uint8_t tweak1_2[8]; \
  do if (variant == 1) \
  { \
    VARIANT1_CHECK(); \
    memcpy(&tweak1_2, &state.hs.b[192], sizeof(tweak1_2)); \
    xor64(tweak1_2, NONCE_POINTER); \
  } while(0)

#define VARIANT1_INIT64() \
  if (variant == 1) \
  { \
    VARIANT1_CHECK(); \
  } \
  const uint64_t tweak1_2 = (variant == 1) ? (state.hs.w[24] ^ (*((const uint64_t*)NONCE_POINTER))) : 0

#define VARIANT2_INIT64() \
  uint64_t division_result = 0; \
  uint64_t sqrt_result = 0; \
  do if (variant == 2) \
  { \
    U64(b)[2] = state.hs.w[8] ^ state.hs.w[10]; \
    U64(b)[3] = state.hs.w[9] ^ state.hs.w[11]; \
    division_result = state.hs.w[12]; \
    sqrt_result = state.hs.w[13]; \
  } while (0)

#define VARIANT2_PORTABLE_INIT() \
  uint64_t division_result = 0; \
  uint64_t sqrt_result = 0; \
  do if (variant == 2) \
  { \
    memcpy(b + AES_BLOCK_SIZE, state.hs.b + 64, AES_BLOCK_SIZE); \
    xor64(b + AES_BLOCK_SIZE, state.hs.b + 80); \
    xor64(b + AES_BLOCK_SIZE + 8, state.hs.b + 88); \
    division_result = state.hs.w[12]; \
    sqrt_result = state.hs.w[13]; \
  } while (0)

#define VARIANT2_SHUFFLE_ADD_SSE2(base_ptr, offset) \
  do if (variant == 2) \
  { \
    const __m128i chunk1 = _mm_load_si128((__m128i *)((base_ptr) + ((offset) ^ 0x10))); \
    const __m128i chunk2 = _mm_load_si128((__m128i *)((base_ptr) + ((offset) ^ 0x20))); \
    const __m128i chunk3 = _mm_load_si128((__m128i *)((base_ptr) + ((offset) ^ 0x30))); \
    _mm_store_si128((__m128i *)((base_ptr) + ((offset) ^ 0x10)), _mm_add_epi64(chunk3, _b1)); \
    _mm_store_si128((__m128i *)((base_ptr) + ((offset) ^ 0x20)), _mm_add_epi64(chunk1, _b)); \
    _mm_store_si128((__m128i *)((base_ptr) + ((offset) ^ 0x30)), _mm_add_epi64(chunk2, _a)); \
  } while (0)

#define VARIANT2_SHUFFLE_ADD_NEON(base_ptr, offset) \
  do if (variant == 2) \
  { \
    const uint64x2_t chunk1 = vld1q_u64(U64((base_ptr) + ((offset) ^ 0x10))); \
    const uint64x2_t chunk2 = vld1q_u64(U64((base_ptr) + ((offset) ^ 0x20))); \
    const uint64x2_t chunk3 = vld1q_u64(U64((base_ptr) + ((offset) ^ 0x30))); \
    vst1q_u64(U64((base_ptr) + ((offset) ^ 0x10)), vaddq_u64(chunk3, vreinterpretq_u64_u8(_b1))); \
    vst1q_u64(U64((base_ptr) + ((offset) ^ 0x20)), vaddq_u64(chunk1, vreinterpretq_u64_u8(_b))); \
    vst1q_u64(U64((base_ptr) + ((offset) ^ 0x30)), vaddq_u64(chunk2, vreinterpretq_u64_u8(_a))); \
  } while (0)

#define VARIANT2_PORTABLE_SHUFFLE_ADD(base_ptr, offset) \
  do if (variant == 2) \
  { \
    uint64_t* chunk1 = U64((base_ptr) + ((offset) ^ 0x10)); \
    uint64_t* chunk2 = U64((base_ptr) + ((offset) ^ 0x20)); \
    uint64_t* chunk3 = U64((base_ptr) + ((offset) ^ 0x30)); \
    \
    const uint64_t chunk1_old[2] = { chunk1[0], chunk1[1] }; \
    \
    uint64_t b1[2]; \
    memcpy(b1, b + 16, 16); \
    chunk1[0] = chunk3[0] + b1[0]; \
    chunk1[1] = chunk3[1] + b1[1]; \
    \
    uint64_t a0[2]; \
    memcpy(a0, a, 16); \
    chunk3[0] = chunk2[0] + a0[0]; \
    chunk3[1] = chunk2[1] + a0[1]; \
    \
    uint64_t b0[2]; \
    memcpy(b0, b, 16); \
    chunk2[0] = chunk1_old[0] + b0[0]; \
    chunk2[1] = chunk1_old[1] + b0[1]; \
  } while (0)

#define VARIANT2_INTEGER_MATH_DIVISION_STEP(b, ptr) \
  ((uint64_t*)(b))[0] ^= division_result ^ (sqrt_result << 32); \
  { \
    const uint64_t dividend = ((uint64_t*)(ptr))[1]; \
    const uint32_t divisor = (((uint64_t*)(ptr))[0] + (uint32_t)(sqrt_result << 1)) | 0x80000001UL; \
    division_result = ((uint32_t)(dividend / divisor)) + \
                     (((uint64_t)(dividend % divisor)) << 32); \
  } \
  const uint64_t sqrt_input = ((uint64_t*)(ptr))[0] + division_result

#define VARIANT2_INTEGER_MATH_SSE2(b, ptr) \
  do if (variant == 2) \
  { \
    VARIANT2_INTEGER_MATH_DIVISION_STEP(b, ptr); \
    VARIANT2_INTEGER_MATH_SQRT_STEP_SSE2(); \
    VARIANT2_INTEGER_MATH_SQRT_FIXUP(sqrt_result); \
  } while(0)

#if defined DBL_MANT_DIG && (DBL_MANT_DIG >= 50)
  // double precision floating point type has enough bits of precision on current platform
  #define VARIANT2_PORTABLE_INTEGER_MATH(b, ptr) \
    do if (variant == 2) \
    { \
      VARIANT2_INTEGER_MATH_DIVISION_STEP(b, ptr); \
      VARIANT2_INTEGER_MATH_SQRT_STEP_FP64(); \
      VARIANT2_INTEGER_MATH_SQRT_FIXUP(sqrt_result); \
    } while (0)
#else
  // double precision floating point type is not good enough on current platform
  // fall back to the reference code (integer only)
  #define VARIANT2_PORTABLE_INTEGER_MATH(b, ptr) \
    do if (variant == 2) \
    { \
      VARIANT2_INTEGER_MATH_DIVISION_STEP(b, ptr); \
      VARIANT2_INTEGER_MATH_SQRT_STEP_REF(); \
    } while (0)
#endif

#define VARIANT2_2_PORTABLE() \
    if (variant == 2) { \
      xor_blocks(long_state + (j ^ 0x10), d); \
      xor_blocks(d, long_state + (j ^ 0x20)); \
    }

#define VARIANT2_2() \
  do if (variant == 2) \
  { \
    *U64(hp_state + (j ^ 0x10)) ^= hi; \
    *(U64(hp_state + (j ^ 0x10)) + 1) ^= lo; \
    hi ^= *U64(hp_state + (j ^ 0x20)); \
    lo ^= *(U64(hp_state + (j ^ 0x20)) + 1); \
  } while (0)

#if !defined NO_AES && (defined(__x86_64__) || (defined(_MSC_VER) && defined(_WIN64)))
// Optimised code below, uses x86-specific intrinsics, SSE2, AES-NI
// Fall back to more portable code is down at the bottom

#include <emmintrin.h>

#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>
#define STATIC
#define INLINE __inline
#if !defined(RDATA_ALIGN16)
#define RDATA_ALIGN16 __declspec(align(16))
#endif
#elif defined(__MINGW32__)
#include <intrin.h>
#include <windows.h>
#define STATIC static
#define INLINE inline
#if !defined(RDATA_ALIGN16)
#define RDATA_ALIGN16 __attribute__ ((aligned(16)))
#endif
#else
#include <wmmintrin.h>
#include <sys/mman.h>
#define STATIC static
#define INLINE inline
#if !defined(RDATA_ALIGN16)
#define RDATA_ALIGN16 __attribute__ ((aligned(16)))
#endif
#endif

#if defined(__INTEL_COMPILER)
#define ASM __asm__
#elif !defined(_MSC_VER)
#define ASM __asm__
#else
#define ASM __asm
#endif

#define U64(x) ((uint64_t *) (x))
#define R128(x) ((__m128i *) (x))

#define state_index(x,div) (((*((uint64_t *)x) >> 4) & (TOTALBLOCKS /(div) - 1)) << 4)
#if defined(_MSC_VER)
#if !defined(_WIN64)
#define __mul() lo = mul128(c[0], b[0], &hi);
#else
#define __mul() lo = _umul128(c[0], b[0], &hi);
#endif
#else
#if defined(__x86_64__)
#define __mul() ASM("mulq %3\n\t" : "=d"(hi), "=a"(lo) : "%a" (c[0]), "rm" (b[0]) : "cc");
#else
#define __mul() lo = mul128(c[0], b[0], &hi);
#endif
#endif

#define pre_aes() \
  j = state_index(a,lightFlag); \
  _c = _mm_load_si128(R128(&hp_state[j])); \
  _a = _mm_load_si128(R128(a)); \

/*
 * An SSE-optimized implementation of the second half of CryptoNight step 3.
 * After using AES to mix a scratchpad value into _c (done by the caller),
 * this macro xors it with _b and stores the result back to the same index (j) that it
 * loaded the scratchpad value from.  It then performs a second random memory
 * read/write from the scratchpad, but this time mixes the values using a 64
 * bit multiply.
 * This code is based upon an optimized implementation by dga.
 */
#define post_aes() \
  VARIANT2_SHUFFLE_ADD_SSE2(hp_state, j); \
  _mm_store_si128(R128(c), _c); \
  _mm_store_si128(R128(&hp_state[j]), _mm_xor_si128(_b, _c)); \
  VARIANT1_1(&hp_state[j]); \
  j = state_index(c,lightFlag); \
  p = U64(&hp_state[j]); \
  b[0] = p[0]; b[1] = p[1]; \
  VARIANT2_INTEGER_MATH_SSE2(b, c); \
  __mul(); \
  VARIANT2_2(); \
  VARIANT2_SHUFFLE_ADD_SSE2(hp_state, j); \
  a[0] += hi; a[1] += lo; \
  p = U64(&hp_state[j]); \
  p[0] = a[0];  p[1] = a[1]; \
  a[0] ^= b[0]; a[1] ^= b[1]; \
  VARIANT1_2(p + 1); \
  _b1 = _b; \
  _b = _c; \

#if defined(_MSC_VER)
#define THREADV __declspec(thread)
#else
#define THREADV __thread
#endif

#pragma pack(push, 1)
union cn_turtle_hash_state
{
  union hash_state hs;
  struct
  {
    uint8_t k[64];
    uint8_t init[INIT_SIZE_BYTE];
  };
};
#pragma pack(pop)

THREADV uint8_t *hp_state = NULL;
THREADV int hp_allocated = 0;

#if defined(_MSC_VER)
#define cpuid(info,x)    __cpuidex(info,x,0)
#else
void cpuid(int CPUInfo[4], int InfoType)
{
  ASM __volatile__
  (
  "cpuid":
    "=a" (CPUInfo[0]),
    "=b" (CPUInfo[1]),
    "=c" (CPUInfo[2]),
    "=d" (CPUInfo[3]) :
        "a" (InfoType), "c" (0)
    );
}
#endif

/**
 * @brief a = (a xor b), where a and b point to 128 bit values
 */

STATIC INLINE void xor_blocks(uint8_t *a, const uint8_t *b)
{
  U64(a)[0] ^= U64(b)[0];
  U64(a)[1] ^= U64(b)[1];
}

STATIC INLINE void xor64(uint64_t *a, const uint64_t b)
{
  *a ^= b;
}

/**
 * @brief uses cpuid to determine if the CPU supports the AES instructions
 * @return true if the CPU supports AES, false otherwise
 */

STATIC INLINE int force_software_aes(void)
{
  static int use = -1;

  if (use != -1)
    return use;

  const char *env = getenv("TURTLECOIN_USE_SOFTWARE_AES");
  if (!env) {
    use = 0;
  }
  else if (!strcmp(env, "0") || !strcmp(env, "no")) {
    use = 0;
  }
  else {
    use = 1;
  }
  return use;
}

STATIC INLINE int check_aes_hw(void)
{
  int cpuid_results[4];
  static int supported = -1;

  if(supported >= 0)
    return supported;

  cpuid(cpuid_results,1);
  return supported = cpuid_results[2] & (1 << 25);
}

STATIC INLINE void aes_256_assist1(__m128i* t1, __m128i * t2)
{
  __m128i t4;
  *t2 = _mm_shuffle_epi32(*t2, 0xff);
  t4 = _mm_slli_si128(*t1, 0x04);
  *t1 = _mm_xor_si128(*t1, t4);
  t4 = _mm_slli_si128(t4, 0x04);
  *t1 = _mm_xor_si128(*t1, t4);
  t4 = _mm_slli_si128(t4, 0x04);
  *t1 = _mm_xor_si128(*t1, t4);
  *t1 = _mm_xor_si128(*t1, *t2);
}

STATIC INLINE void aes_256_assist2(__m128i* t1, __m128i * t3)
{
  __m128i t2, t4;
  t4 = _mm_aeskeygenassist_si128(*t1, 0x00);
  t2 = _mm_shuffle_epi32(t4, 0xaa);
  t4 = _mm_slli_si128(*t3, 0x04);
  *t3 = _mm_xor_si128(*t3, t4);
  t4 = _mm_slli_si128(t4, 0x04);
  *t3 = _mm_xor_si128(*t3, t4);
  t4 = _mm_slli_si128(t4, 0x04);
  *t3 = _mm_xor_si128(*t3, t4);
  *t3 = _mm_xor_si128(*t3, t2);
}

/**
 * @brief expands 'key' into a form it can be used for AES encryption.
 *
 * This is an SSE-optimized implementation of AES key schedule generation.  It
 * expands the key into multiple round keys, each of which is used in one round
 * of the AES encryption used to fill (and later, extract randomness from)
 * the large 2MB buffer.  Note that CryptoNight does not use a completely
 * standard AES encryption for its buffer expansion, so do not copy this
 * function outside of Monero without caution!  This version uses the hardware
 * AESKEYGENASSIST instruction to speed key generation, and thus requires
 * CPU AES support.
 * For more information about these functions, see page 19 of Intel's AES instructions
 * white paper:
 * https://www.intel.com/content/dam/doc/white-paper/advanced-encryption-standard-new-instructions-set-paper.pdf
 *
 * @param key the input 128 bit key
 * @param expandedKey An output buffer to hold the generated key schedule
 */

STATIC INLINE void aes_expand_key(const uint8_t *key, uint8_t *expandedKey)
{
  __m128i *ek = R128(expandedKey);
  __m128i t1, t2, t3;

  t1 = _mm_loadu_si128(R128(key));
  t3 = _mm_loadu_si128(R128(key + 16));

  ek[0] = t1;
  ek[1] = t3;

  t2 = _mm_aeskeygenassist_si128(t3, 0x01);
  aes_256_assist1(&t1, &t2);
  ek[2] = t1;
  aes_256_assist2(&t1, &t3);
  ek[3] = t3;

  t2 = _mm_aeskeygenassist_si128(t3, 0x02);
  aes_256_assist1(&t1, &t2);
  ek[4] = t1;
  aes_256_assist2(&t1, &t3);
  ek[5] = t3;

  t2 = _mm_aeskeygenassist_si128(t3, 0x04);
  aes_256_assist1(&t1, &t2);
  ek[6] = t1;
  aes_256_assist2(&t1, &t3);
  ek[7] = t3;

  t2 = _mm_aeskeygenassist_si128(t3, 0x08);
  aes_256_assist1(&t1, &t2);
  ek[8] = t1;
  aes_256_assist2(&t1, &t3);
  ek[9] = t3;

  t2 = _mm_aeskeygenassist_si128(t3, 0x10);
  aes_256_assist1(&t1, &t2);
  ek[10] = t1;
}

/**
 * @brief a "pseudo" round of AES (similar to but slightly different from normal AES encryption)
 *
 * To fill its 2MB scratch buffer, CryptoNight uses a nonstandard implementation
 * of AES encryption:  It applies 10 rounds of the basic AES encryption operation
 * to an input 128 bit chunk of data <in>.  Unlike normal AES, however, this is
 * all it does;  it does not perform the initial AddRoundKey step (this is done
 * in subsequent steps by aesenc_si128), and it does not use the simpler final round.
 * Hence, this is a "pseudo" round - though the function actually implements 10 rounds together.
 *
 * Note that unlike aesb_pseudo_round, this function works on multiple data chunks.
 *
 * @param in a pointer to nblocks * 128 bits of data to be encrypted
 * @param out a pointer to an nblocks * 128 bit buffer where the output will be stored
 * @param expandedKey the expanded AES key
 * @param nblocks the number of 128 blocks of data to be encrypted
 */

STATIC INLINE void aes_pseudo_round(const uint8_t *in, uint8_t *out,
                                    const uint8_t *expandedKey, int nblocks)
{
  __m128i *k = R128(expandedKey);
  __m128i d;
  int i;

  for(i = 0; i < nblocks; i++)
  {
    d = _mm_loadu_si128(R128(in + i * AES_BLOCK_SIZE));
    d = _mm_aesenc_si128(d, *R128(&k[0]));
    d = _mm_aesenc_si128(d, *R128(&k[1]));
    d = _mm_aesenc_si128(d, *R128(&k[2]));
    d = _mm_aesenc_si128(d, *R128(&k[3]));
    d = _mm_aesenc_si128(d, *R128(&k[4]));
    d = _mm_aesenc_si128(d, *R128(&k[5]));
    d = _mm_aesenc_si128(d, *R128(&k[6]));
    d = _mm_aesenc_si128(d, *R128(&k[7]));
    d = _mm_aesenc_si128(d, *R128(&k[8]));
    d = _mm_aesenc_si128(d, *R128(&k[9]));
    _mm_storeu_si128((R128(out + i * AES_BLOCK_SIZE)), d);
  }
}

/**
 * @brief aes_pseudo_round that loads data from *in and xors it with *xor first
 *
 * This function performs the same operations as aes_pseudo_round, but before
 * performing the encryption of each 128 bit block from <in>, it xors
 * it with the corresponding block from <xor>.
 *
 * @param in a pointer to nblocks * 128 bits of data to be encrypted
 * @param out a pointer to an nblocks * 128 bit buffer where the output will be stored
 * @param expandedKey the expanded AES key
 * @param xor a pointer to an nblocks * 128 bit buffer that is xored into in before encryption (in is left unmodified)
 * @param nblocks the number of 128 blocks of data to be encrypted
 */

STATIC INLINE void aes_pseudo_round_xor(const uint8_t *in, uint8_t *out,
                                        const uint8_t *expandedKey, const uint8_t *xor, int nblocks)
{
    __m128i *k = R128(expandedKey);
    __m128i *x = R128(xor);
    __m128i d;
    int i;

    for(i = 0; i < nblocks; i++)
    {
        d = _mm_loadu_si128(R128(in + i * AES_BLOCK_SIZE));
        d = _mm_xor_si128(d, *R128(x++));
        d = _mm_aesenc_si128(d, *R128(&k[0]));
        d = _mm_aesenc_si128(d, *R128(&k[1]));
        d = _mm_aesenc_si128(d, *R128(&k[2]));
        d = _mm_aesenc_si128(d, *R128(&k[3]));
        d = _mm_aesenc_si128(d, *R128(&k[4]));
        d = _mm_aesenc_si128(d, *R128(&k[5]));
        d = _mm_aesenc_si128(d, *R128(&k[6]));
        d = _mm_aesenc_si128(d, *R128(&k[7]));
        d = _mm_aesenc_si128(d, *R128(&k[8]));
        d = _mm_aesenc_si128(d, *R128(&k[9]));
        _mm_storeu_si128((R128(out + i * AES_BLOCK_SIZE)), d);
    }
}

#if defined(_MSC_VER) || defined(__MINGW32__)
BOOL SetLockPagesPrivilege(HANDLE hProcess, BOOL bEnable)
{
  struct
  {
      DWORD count;
      LUID_AND_ATTRIBUTES privilege[1];
  } info;

  HANDLE token;
  if(!OpenProcessToken(hProcess, TOKEN_ADJUST_PRIVILEGES, &token))
      return FALSE;

  info.count = 1;
  info.privilege[0].Attributes = bEnable ? SE_PRIVILEGE_ENABLED : 0;

  if(!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &(info.privilege[0].Luid)))
      return FALSE;

  if(!AdjustTokenPrivileges(token, FALSE, (PTOKEN_PRIVILEGES) &info, 0, NULL, NULL))
      return FALSE;

  if (GetLastError() != ERROR_SUCCESS)
      return FALSE;

  CloseHandle(token);

  return TRUE;
}
#endif

/**
 * @brief allocate the 2MB scratch buffer using OS support for huge pages, if available
 *
 * This function tries to allocate the 2MB scratch buffer using a single
 * 2MB "huge page" (instead of the usual 4KB page sizes) to reduce TLB misses
 * during the random accesses to the scratch buffer.  This is one of the
 * important speed optimizations needed to make CryptoNight faster.
 *
 * No parameters.  Updates a thread-local pointer, hp_state, to point to
 * the allocated buffer.
 */

void slow_hash_allocate_state(uint32_t page_size)
{
    if(hp_state != NULL)
        return;

#if defined(_MSC_VER) || defined(__MINGW32__)
    SetLockPagesPrivilege(GetCurrentProcess(), TRUE);
    hp_state = (uint8_t *) VirtualAlloc(hp_state, page_size, MEM_LARGE_PAGES |
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
  defined(__DragonFly__) || defined(__NetBSD__)
    hp_state = mmap(0, page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, 0, 0);
#else
    hp_state = mmap(0, page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, 0, 0);
#endif
    if(hp_state == MAP_FAILED)
        hp_state = NULL;
#endif
    hp_allocated = 1;
    if(hp_state == NULL)
    {
        hp_allocated = 0;
        hp_state = (uint8_t *) malloc(page_size);
    }
}

/**
 *@brief frees the state allocated by slow_hash_allocate_state
 */

void slow_hash_free_state(uint32_t page_size)
{
    if(hp_state == NULL)
        return;

    if(!hp_allocated)
        free(hp_state);
    else
    {
#if defined(_MSC_VER) || defined(__MINGW32__)
        VirtualFree(hp_state, 0, MEM_RELEASE);
#else
        munmap(hp_state, page_size);
#endif
    }

    hp_state = NULL;
    hp_allocated = 0;
}

/**
 * @brief the hash function implementing CryptoNight, used for the Monero proof-of-work
 *
 * Computes the hash of <data> (which consists of <length> bytes), returning the
 * hash in <hash>.  The CryptoNight hash operates by first using Keccak 1600,
 * the 1600 bit variant of the Keccak hash used in SHA-3, to create a 200 byte
 * buffer of pseudorandom data by hashing the supplied data.  It then uses this
 * random data to fill a large 2MB buffer with pseudorandom data by iteratively
 * encrypting it using 10 rounds of AES per entry.  After this initialization,
 * it executes 524,288 rounds of mixing through the random 2MB buffer using
 * AES (typically provided in hardware on modern CPUs) and a 64 bit multiply.
 * Finally, it re-mixes this large buffer back into
 * the 200 byte "text" buffer, and then hashes this buffer using one of four
 * pseudorandomly selected hash functions (Blake, Groestl, JH, or Skein)
 * to populate the output.
 *
 * The 2MB buffer and choice of functions for mixing are designed to make the
 * algorithm "CPU-friendly" (and thus, reduce the advantage of GPU, FPGA,
 * or ASIC-based implementations):  the functions used are fast on modern
 * CPUs, and the 2MB size matches the typical amount of L3 cache available per
 * core on 2013-era CPUs.  When available, this implementation will use hardware
 * AES support on x86 CPUs.
 *
 * A diagram of the inner loop of this function can be found at
 * https://www.cs.cmu.edu/~dga/crypto/xmr/cryptonight.png
 *
 * @param data the data to hash
 * @param length the length in bytes of the data
 * @param hash a pointer to a buffer in which the final 256 bit hash will be stored
 */
void cn_turtle_hash(const void *data, size_t length, char *hash, int light, int variant, int prehashed, uint32_t page_size, uint32_t scratchpad, uint32_t iterations)
{
  uint32_t TOTALBLOCKS = (page_size / AES_BLOCK_SIZE);
  uint32_t init_rounds = (scratchpad / INIT_SIZE_BYTE);
  uint32_t aes_rounds = (iterations / 2);
  size_t lightFlag = (light ? 2: 1);

  RDATA_ALIGN16 uint8_t expandedKey[240];  /* These buffers are aligned to use later with SSE functions */

  uint8_t text[INIT_SIZE_BYTE];
  RDATA_ALIGN16 uint64_t a[2];
  RDATA_ALIGN16 uint64_t b[4];
  RDATA_ALIGN16 uint64_t c[2];
  union cn_turtle_hash_state state;
  __m128i _a, _b, _b1, _c;
  uint64_t hi, lo;

  size_t i, j;
  uint64_t *p = NULL;
  oaes_ctx *aes_ctx = NULL;
  int useAes = !force_software_aes() && check_aes_hw();

  static void (*const extra_hashes[4])(const void *, size_t, char *) =
  {
      hash_extra_blake, hash_extra_groestl, hash_extra_jh, hash_extra_skein
  };

  slow_hash_allocate_state(page_size);

  /* CryptoNight Step 1:  Use Keccak1600 to initialize the 'state' (and 'text') buffers from the data. */
  if (prehashed) {
      memcpy(&state.hs, data, length);
  } else {
      hash_process(&state.hs, data, length);
  }
  memcpy(text, state.init, INIT_SIZE_BYTE);

  VARIANT1_INIT64();
  VARIANT2_INIT64();

  /* CryptoNight Step 2:  Iteratively encrypt the results from Keccak to fill
   * the 2MB large random access buffer.
   */

  if(useAes)
  {
      aes_expand_key(state.hs.b, expandedKey);
      for(i = 0; i < init_rounds; i++)
      {
          aes_pseudo_round(text, text, expandedKey, INIT_SIZE_BLK);
          memcpy(&hp_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
      }
  }
  else
  {
      aes_ctx = (oaes_ctx *) oaes_alloc();
      oaes_key_import_data(aes_ctx, state.hs.b, AES_KEY_SIZE);
      for(i = 0; i < init_rounds; i++)
      {
          for(j = 0; j < INIT_SIZE_BLK; j++)
              aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], aes_ctx->key->exp_data);

          memcpy(&hp_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
      }
  }

  U64(a)[0] = U64(&state.k[0])[0] ^ U64(&state.k[32])[0];
  U64(a)[1] = U64(&state.k[0])[1] ^ U64(&state.k[32])[1];
  U64(b)[0] = U64(&state.k[16])[0] ^ U64(&state.k[48])[0];
  U64(b)[1] = U64(&state.k[16])[1] ^ U64(&state.k[48])[1];

  /* CryptoNight Step 3:  Bounce randomly 1,048,576 times (1<<20) through the mixing buffer,
   * using 524,288 iterations of the following mixing function.  Each execution
   * performs two reads and writes from the mixing buffer.
   */

  _b = _mm_load_si128(R128(b));
  _b1 = _mm_load_si128(R128(b) + 1);
  // Two independent versions, one with AES, one without, to ensure that
  // the useAes test is only performed once, not every iteration.
  if(useAes)
  {
      for(i = 0; i < aes_rounds; i++)
      {
          pre_aes();
          _c = _mm_aesenc_si128(_c, _a);
          post_aes();
      }
  }
  else
  {
      for(i = 0; i < aes_rounds; i++)
      {
          pre_aes();
          aesb_single_round((uint8_t *) &_c, (uint8_t *) &_c, (uint8_t *) &_a);
          post_aes();
      }
  }

  /* CryptoNight Step 4:  Sequentially pass through the mixing buffer and use 10 rounds
   * of AES encryption to mix the random data back into the 'text' buffer.  'text'
   * was originally created with the output of Keccak1600. */

  memcpy(text, state.init, INIT_SIZE_BYTE);
  if(useAes)
  {
      aes_expand_key(&state.hs.b[32], expandedKey);
      for(i = 0; i < init_rounds; i++)
      {
          // add the xor to the pseudo round
          aes_pseudo_round_xor(text, text, expandedKey, &hp_state[i * INIT_SIZE_BYTE], INIT_SIZE_BLK);
      }
  }
  else
  {
      oaes_key_import_data(aes_ctx, &state.hs.b[32], AES_KEY_SIZE);
      for(i = 0; i < init_rounds; i++)
      {
          for(j = 0; j < INIT_SIZE_BLK; j++)
          {
              xor_blocks(&text[j * AES_BLOCK_SIZE], &hp_state[i * INIT_SIZE_BYTE + j * AES_BLOCK_SIZE]);
              aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], aes_ctx->key->exp_data);
          }
      }
      oaes_free((OAES_CTX **) &aes_ctx);
  }

  /* CryptoNight Step 5:  Apply Keccak to the state again, and then
   * use the resulting data to select which of four finalizer
   * hash functions to apply to the data (Blake, Groestl, JH, or Skein).
   * Use this hash to squeeze the state array down
   * to the final 256 bit hash output.
   */

  memcpy(state.init, text, INIT_SIZE_BYTE);
  hash_permutation(&state.hs);
  extra_hashes[state.hs.b[0] & 3](&state, 200, hash);
  slow_hash_free_state(page_size);
}

#elif !defined NO_AES && (defined(__arm__) || defined(__aarch64__))
void slow_hash_allocate_state(void)
{
  // Do nothing, this is just to maintain compatibility with the upgraded slow-hash.c
  return;
}

void slow_hash_free_state(void)
{
  // As above
  return;
}

#if defined(__GNUC__)
#define RDATA_ALIGN16 __attribute__ ((aligned(16)))
#define STATIC static
#define INLINE inline
#else
#define RDATA_ALIGN16
#define STATIC static
#define INLINE
#endif

#define U64(x) ((uint64_t *) (x))

STATIC INLINE void xor64(uint64_t *a, const uint64_t b)
{
    *a ^= b;
}

#pragma pack(push, 1)
union cn_turtle_hash_state
{
    union hash_state hs;
    struct
    {
        uint8_t k[64];
        uint8_t init[INIT_SIZE_BYTE];
    };
};
#pragma pack(pop)

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)

/* ARMv8-A optimized with NEON and AES instructions.
 * Copied from the x86-64 AES-NI implementation. It has much the same
 * characteristics as x86-64: there's no 64x64=128 multiplier for vectors,
 * and moving between vector and regular registers stalls the pipeline.
 */
#include <arm_neon.h>

#define state_index(x,div) (((*((uint64_t *)x) >> 4) & (TOTALBLOCKS /(div) - 1)) << 4)
#define __mul() __asm__("mul %0, %1, %2\n\t" : "=r"(lo) : "r"(c[0]), "r"(b[0]) ); \
  __asm__("umulh %0, %1, %2\n\t" : "=r"(hi) : "r"(c[0]), "r"(b[0]) );

#define pre_aes() \
  j = state_index(a,lightFlag); \
  _c = vld1q_u8(&hp_state[j]); \
  _a = vld1q_u8((const uint8_t *)a); \

#define post_aes() \
  VARIANT2_SHUFFLE_ADD_NEON(hp_state, j); \
  vst1q_u8((uint8_t *)c, _c); \
  vst1q_u8(&hp_state[j], veorq_u8(_b, _c)); \
  VARIANT1_1(&hp_state[j]); \
  j = state_index(c,lightFlag); \
  p = U64(&hp_state[j]); \
  b[0] = p[0]; b[1] = p[1]; \
  VARIANT2_PORTABLE_INTEGER_MATH(b, c); \
  __mul(); \
  VARIANT2_2(); \
  VARIANT2_SHUFFLE_ADD_NEON(hp_state, j); \
  a[0] += hi; a[1] += lo; \
  p = U64(&hp_state[j]); \
  p[0] = a[0];  p[1] = a[1]; \
  a[0] ^= b[0]; a[1] ^= b[1]; \
  VARIANT1_2(p + 1); \
  _b1 = _b; \
  _b = _c; \


/* Note: this was based on a standard 256bit key schedule but
 * it's been shortened since Cryptonight doesn't use the full
 * key schedule. Don't try to use this for vanilla AES.
*/
static void aes_expand_key(const uint8_t *key, uint8_t *expandedKey) {
static const int rcon[] = {
	0x01,0x01,0x01,0x01,
	0x0c0f0e0d,0x0c0f0e0d,0x0c0f0e0d,0x0c0f0e0d,	// rotate-n-splat
	0x1b,0x1b,0x1b,0x1b };
__asm__(
"	eor	v0.16b,v0.16b,v0.16b\n"
"	ld1	{v3.16b},[%0],#16\n"
"	ld1	{v1.4s,v2.4s},[%2],#32\n"
"	ld1	{v4.16b},[%0]\n"
"	mov	w2,#5\n"
"	st1	{v3.4s},[%1],#16\n"
"\n"
"1:\n"
"	tbl	v6.16b,{v4.16b},v2.16b\n"
"	ext	v5.16b,v0.16b,v3.16b,#12\n"
"	st1	{v4.4s},[%1],#16\n"
"	aese	v6.16b,v0.16b\n"
"	subs	w2,w2,#1\n"
"\n"
"	eor	v3.16b,v3.16b,v5.16b\n"
"	ext	v5.16b,v0.16b,v5.16b,#12\n"
"	eor	v3.16b,v3.16b,v5.16b\n"
"	ext	v5.16b,v0.16b,v5.16b,#12\n"
"	eor	v6.16b,v6.16b,v1.16b\n"
"	eor	v3.16b,v3.16b,v5.16b\n"
"	shl	v1.16b,v1.16b,#1\n"
"	eor	v3.16b,v3.16b,v6.16b\n"
"	st1	{v3.4s},[%1],#16\n"
"	b.eq	2f\n"
"\n"
"	dup	v6.4s,v3.s[3]		// just splat\n"
"	ext	v5.16b,v0.16b,v4.16b,#12\n"
"	aese	v6.16b,v0.16b\n"
"\n"
"	eor	v4.16b,v4.16b,v5.16b\n"
"	ext	v5.16b,v0.16b,v5.16b,#12\n"
"	eor	v4.16b,v4.16b,v5.16b\n"
"	ext	v5.16b,v0.16b,v5.16b,#12\n"
"	eor	v4.16b,v4.16b,v5.16b\n"
"\n"
"	eor	v4.16b,v4.16b,v6.16b\n"
"	b	1b\n"
"\n"
"2:\n" : : "r"(key), "r"(expandedKey), "r"(rcon));
}

/* An ordinary AES round is a sequence of SubBytes, ShiftRows, MixColumns, AddRoundKey. There
 * is also an InitialRound which consists solely of AddRoundKey. The ARM instructions slice
 * this sequence differently; the aese instruction performs AddRoundKey, SubBytes, ShiftRows.
 * The aesmc instruction does the MixColumns. Since the aese instruction moves the AddRoundKey
 * up front, and Cryptonight's hash skips the InitialRound step, we have to kludge it here by
 * feeding in a vector of zeros for our first step. Also we have to do our own Xor explicitly
 * at the last step, to provide the AddRoundKey that the ARM instructions omit.
 */
STATIC INLINE void aes_pseudo_round(const uint8_t *in, uint8_t *out, const uint8_t *expandedKey, int nblocks)
{
	const uint8x16_t *k = (const uint8x16_t *)expandedKey, zero = {0};
	uint8x16_t tmp;
	int i;

	for (i=0; i<nblocks; i++)
	{
		uint8x16_t tmp = vld1q_u8(in + i * AES_BLOCK_SIZE);
		tmp = vaeseq_u8(tmp, zero);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[0]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[1]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[2]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[3]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[4]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[5]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[6]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[7]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[8]);
		tmp = vaesmcq_u8(tmp);
		tmp = veorq_u8(tmp,  k[9]);
		vst1q_u8(out + i * AES_BLOCK_SIZE, tmp);
	}
}

STATIC INLINE void aes_pseudo_round_xor(const uint8_t *in, uint8_t *out, const uint8_t *expandedKey, const uint8_t *xor, int nblocks)
{
	const uint8x16_t *k = (const uint8x16_t *)expandedKey;
	const uint8x16_t *x = (const uint8x16_t *)xor;
	uint8x16_t tmp;
	int i;

	for (i=0; i<nblocks; i++)
	{
		uint8x16_t tmp = vld1q_u8(in + i * AES_BLOCK_SIZE);
		tmp = vaeseq_u8(tmp, x[i]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[0]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[1]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[2]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[3]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[4]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[5]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[6]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[7]);
		tmp = vaesmcq_u8(tmp);
		tmp = vaeseq_u8(tmp, k[8]);
		tmp = vaesmcq_u8(tmp);
		tmp = veorq_u8(tmp,  k[9]);
		vst1q_u8(out + i * AES_BLOCK_SIZE, tmp);
	}
}

#ifdef FORCE_USE_HEAP
STATIC INLINE void* aligned_malloc(size_t size, size_t align)
{
    void *result;
#ifdef _MSC_VER
    result = _aligned_malloc(size, align);
#else
    if (posix_memalign(&result, align, size)) result = NULL;
#endif
    return result;
}

STATIC INLINE void aligned_free(void *ptr)
{
#ifdef _MSC_VER
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}
#endif /* FORCE_USE_HEAP */

void cn_turtle_hash(const void *data, size_t length, char *hash, int light, int variant, int prehashed, uint32_t page_size, uint32_t scratchpad, uint32_t iterations)
{
  uint32_t TOTALBLOCKS = (page_size / AES_BLOCK_SIZE);
  uint32_t init_rounds = (scratchpad / INIT_SIZE_BYTE);
  uint32_t aes_rounds = (iterations / 2);
  size_t lightFlag = (light ? 2: 1);

  RDATA_ALIGN16 uint8_t expandedKey[240];

#ifndef FORCE_USE_HEAP
  RDATA_ALIGN16 uint8_t hp_state[page_size];
#else
#warning "ACTIVATING FORCE_USE_HEAP IN aarch64 + crypto in slow-hash.c"
  uint8_t *hp_state = (uint8_t *)aligned_malloc(page_size,16);
#endif

  uint8_t text[INIT_SIZE_BYTE];
  RDATA_ALIGN16 uint64_t a[2];
  RDATA_ALIGN16 uint64_t b[4];
  RDATA_ALIGN16 uint64_t c[2];
  union cn_turtle_hash_state state;
  uint8x16_t _a, _b, _b1, _c, zero = {0};
  uint64_t hi, lo;

  size_t i, j;
  uint64_t *p = NULL;

  static void (*const extra_hashes[4])(const void *, size_t, char *) =
  {
      hash_extra_blake, hash_extra_groestl, hash_extra_jh, hash_extra_skein
  };

  /* CryptoNight Step 1:  Use Keccak1600 to initialize the 'state' (and 'text') buffers from the data. */

  if (prehashed) {
      memcpy(&state.hs, data, length);
  } else {
      hash_process(&state.hs, data, length);
  }
  memcpy(text, state.init, INIT_SIZE_BYTE);

  VARIANT1_INIT64();
  VARIANT2_INIT64();

  /* CryptoNight Step 2:  Iteratively encrypt the results from Keccak to fill
   * the 2MB large random access buffer.
   */

  aes_expand_key(state.hs.b, expandedKey);
  for(i = 0; i < init_rounds; i++)
  {
      aes_pseudo_round(text, text, expandedKey, INIT_SIZE_BLK);
      memcpy(&hp_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
  }

  U64(a)[0] = U64(&state.k[0])[0] ^ U64(&state.k[32])[0];
  U64(a)[1] = U64(&state.k[0])[1] ^ U64(&state.k[32])[1];
  U64(b)[0] = U64(&state.k[16])[0] ^ U64(&state.k[48])[0];
  U64(b)[1] = U64(&state.k[16])[1] ^ U64(&state.k[48])[1];

  /* CryptoNight Step 3:  Bounce randomly 1,048,576 times (1<<20) through the mixing buffer,
   * using 524,288 iterations of the following mixing function.  Each execution
   * performs two reads and writes from the mixing buffer.
   */

  _b = vld1q_u8((const uint8_t *)b);
  _b1 = vld1q_u8(((const uint8_t *)b) + AES_BLOCK_SIZE);

  for(i = 0; i < aes_rounds; i++)
  {
      pre_aes();
      _c = vaeseq_u8(_c, zero);
      _c = vaesmcq_u8(_c);
      _c = veorq_u8(_c, _a);
      post_aes();
  }

  /* CryptoNight Step 4:  Sequentially pass through the mixing buffer and use 10 rounds
   * of AES encryption to mix the random data back into the 'text' buffer.  'text'
   * was originally created with the output of Keccak1600. */

  memcpy(text, state.init, INIT_SIZE_BYTE);

  aes_expand_key(&state.hs.b[32], expandedKey);
  for(i = 0; i < init_rounds; i++)
  {
      // add the xor to the pseudo round
      aes_pseudo_round_xor(text, text, expandedKey, &hp_state[i * INIT_SIZE_BYTE], INIT_SIZE_BLK);
  }

  /* CryptoNight Step 5:  Apply Keccak to the state again, and then
   * use the resulting data to select which of four finalizer
   * hash functions to apply to the data (Blake, Groestl, JH, or Skein).
   * Use this hash to squeeze the state array down
   * to the final 256 bit hash output.
   */

  memcpy(state.init, text, INIT_SIZE_BYTE);
  hash_permutation(&state.hs);
  extra_hashes[state.hs.b[0] & 3](&state, 200, hash);

#ifdef FORCE_USE_HEAP
  aligned_free(hp_state);
#endif
}
#else /* aarch64 && crypto */

// ND: Some minor optimizations for ARMv7 (raspberrry pi 2), effect seems to be ~40-50% faster.
//     Needs more work.

#ifdef NO_OPTIMIZED_MULTIPLY_ON_ARM
/* The asm corresponds to this C code */
#define SHORT uint32_t
#define LONG uint64_t

void mul(const uint8_t *ca, const uint8_t *cb, uint8_t *cres) {
  const SHORT *aa = (SHORT *)ca;
  const SHORT *bb = (SHORT *)cb;
  SHORT *res = (SHORT *)cres;
  union {
    SHORT tmp[8];
    LONG ltmp[4];
  } t;
  LONG A = aa[1];
  LONG a = aa[0];
  LONG B = bb[1];
  LONG b = bb[0];

  // Aa * Bb = ab + aB_ + Ab_ + AB__
  t.ltmp[0] = a * b;
  t.ltmp[1] = a * B;
  t.ltmp[2] = A * b;
  t.ltmp[3] = A * B;

  res[2] = t.tmp[0];
  t.ltmp[1] += t.tmp[1];
  t.ltmp[1] += t.tmp[4];
  t.ltmp[3] += t.tmp[3];
  t.ltmp[3] += t.tmp[5];
  res[3] = t.tmp[2];
  res[0] = t.tmp[6];
  res[1] = t.tmp[7];
}
#else // !NO_OPTIMIZED_MULTIPLY_ON_ARM

#ifdef __aarch64__ /* ARM64, no crypto */
#define mul(a, b, c)	cn_mul128((const uint64_t *)a, (const uint64_t *)b, (uint64_t *)c)
STATIC void cn_mul128(const uint64_t *a, const uint64_t *b, uint64_t *r)
{
  uint64_t lo, hi;
  __asm__("mul %0, %1, %2\n\t" : "=r"(lo) : "r"(a[0]), "r"(b[0]) );
  __asm__("umulh %0, %1, %2\n\t" : "=r"(hi) : "r"(a[0]), "r"(b[0]) );
  r[0] = hi;
  r[1] = lo;
}
#else /* ARM32 */
#define mul(a, b, c)	cn_mul128((const uint32_t *)a, (const uint32_t *)b, (uint32_t *)c)
STATIC void cn_mul128(const uint32_t *aa, const uint32_t *bb, uint32_t *r)
{
  uint32_t t0, t1, t2=0, t3=0;
__asm__ __volatile__(
  "umull %[t0], %[t1], %[a], %[b]\n\t"
  "str   %[t0], %[ll]\n\t"

  // accumulating with 0 can never overflow/carry
  "eor   %[t0], %[t0]\n\t"
  "umlal %[t1], %[t0], %[a], %[B]\n\t"

  "umlal %[t1], %[t2], %[A], %[b]\n\t"
  "str   %[t1], %[lh]\n\t"

  "umlal %[t0], %[t3], %[A], %[B]\n\t"

  // final add may have a carry
  "adds  %[t0], %[t0], %[t2]\n\t"
  "adc   %[t1], %[t3], #0\n\t"

  "str   %[t0], %[hl]\n\t"
  "str   %[t1], %[hh]\n\t"
  : [t0]"=&r"(t0), [t1]"=&r"(t1), [t2]"+r"(t2), [t3]"+r"(t3), [hl]"=m"(r[0]), [hh]"=m"(r[1]), [ll]"=m"(r[2]), [lh]"=m"(r[3])
  : [A]"r"(aa[1]), [a]"r"(aa[0]), [B]"r"(bb[1]), [b]"r"(bb[0])
  : "cc");
}
#endif /* !aarch64 */
#endif // NO_OPTIMIZED_MULTIPLY_ON_ARM

STATIC INLINE void copy_block(uint8_t* dst, const uint8_t* src)
{
  memcpy(dst, src, AES_BLOCK_SIZE);
}

STATIC INLINE void sum_half_blocks(uint8_t* a, const uint8_t* b)
{
  uint64_t a0, a1, b0, b1;
  a0 = U64(a)[0];
  a1 = U64(a)[1];
  b0 = U64(b)[0];
  b1 = U64(b)[1];
  a0 += b0;
  a1 += b1;
  U64(a)[0] = a0;
  U64(a)[1] = a1;
}

STATIC INLINE void swap_blocks(uint8_t *a, uint8_t *b)
{
  uint64_t t[2];
  U64(t)[0] = U64(a)[0];
  U64(t)[1] = U64(a)[1];
  U64(a)[0] = U64(b)[0];
  U64(a)[1] = U64(b)[1];
  U64(b)[0] = U64(t)[0];
  U64(b)[1] = U64(t)[1];
}

STATIC INLINE void xor_blocks(uint8_t* a, const uint8_t* b)
{
  U64(a)[0] ^= U64(b)[0];
  U64(a)[1] ^= U64(b)[1];
}

void cn_turtle_hash(const void *data, size_t length, char *hash, int light, int variant, int prehashed, uint32_t page_size, uint32_t scratchpad, uint32_t iterations)
{
  uint32_t init_rounds = (scratchpad / INIT_SIZE_BYTE);
  uint32_t aes_rounds = (iterations / 2);
  size_t lightFlag = (light ? 2: 1);

  uint8_t text[INIT_SIZE_BYTE];
  uint8_t a[AES_BLOCK_SIZE];
  uint8_t b[AES_BLOCK_SIZE * 2];
  uint8_t c[AES_BLOCK_SIZE];
  uint8_t c1[AES_BLOCK_SIZE];
  uint8_t d[AES_BLOCK_SIZE];
  uint8_t aes_key[AES_KEY_SIZE];
  RDATA_ALIGN16 uint8_t expandedKey[256];

  union cn_turtle_hash_state state;

  size_t i, j;
  uint8_t *p = NULL;
  oaes_ctx *aes_ctx;
  static void (*const extra_hashes[4])(const void *, size_t, char *) =
  {
      hash_extra_blake, hash_extra_groestl, hash_extra_jh, hash_extra_skein
  };

#ifndef FORCE_USE_HEAP
  uint8_t long_state[page_size];
#else
#warning "ACTIVATING FORCE_USE_HEAP IN aarch64 && !crypto in slow-hash.c"
  uint8_t *long_state = (uint8_t *)malloc(page_size);
#endif

  if (prehashed) {
      memcpy(&state.hs, data, length);
  } else {
      hash_process(&state.hs, data, length);
  }
  memcpy(text, state.init, INIT_SIZE_BYTE);

  aes_ctx = (oaes_ctx *) oaes_alloc();
  oaes_key_import_data(aes_ctx, state.hs.b, AES_KEY_SIZE);

  VARIANT1_INIT64();
  VARIANT2_INIT64();

  // use aligned data
  memcpy(expandedKey, aes_ctx->key->exp_data, aes_ctx->key->exp_data_len);
  for(i = 0; i < init_rounds; i++)
  {
      for(j = 0; j < INIT_SIZE_BLK; j++)
          aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], expandedKey);
      memcpy(&long_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
  }

  U64(a)[0] = U64(&state.k[0])[0] ^ U64(&state.k[32])[0];
  U64(a)[1] = U64(&state.k[0])[1] ^ U64(&state.k[32])[1];
  U64(b)[0] = U64(&state.k[16])[0] ^ U64(&state.k[48])[0];
  U64(b)[1] = U64(&state.k[16])[1] ^ U64(&state.k[48])[1];

  for(i = 0; i < aes_rounds; i++)
  {
    #define MASK(div) ((uint32_t)(((page_size / AES_BLOCK_SIZE) / (div) - 1) << 4))
    #define state_index(x,div) ((*(uint32_t *) x) & MASK(div))

    // Iteration 1
    j = state_index(a,lightFlag);
    p = &long_state[j];
    aesb_single_round(p, p, a);
    copy_block(c1, p);

    VARIANT2_PORTABLE_SHUFFLE_ADD(long_state, j);
    xor_blocks(p, b);
    VARIANT1_1(p);

    // Iteration 2
    j = state_index(c1,lightFlag);
    p = &long_state[j];
    copy_block(c, p);

    VARIANT2_PORTABLE_INTEGER_MATH(c, c1);
    mul(c1, c, d);
    VARIANT2_2_PORTABLE();
    VARIANT2_PORTABLE_SHUFFLE_ADD(long_state, j);
    sum_half_blocks(a, d);
    swap_blocks(a, c);
    xor_blocks(a, c);
    VARIANT1_2(U64(c) + 1);
    copy_block(p, c);

    if (variant >= 2) {
      copy_block(b + AES_BLOCK_SIZE, b);
    }
    copy_block(b, c1);
  }

  memcpy(text, state.init, INIT_SIZE_BYTE);
  oaes_key_import_data(aes_ctx, &state.hs.b[32], AES_KEY_SIZE);
  memcpy(expandedKey, aes_ctx->key->exp_data, aes_ctx->key->exp_data_len);
  for(i = 0; i < init_rounds; i++)
  {
      for(j = 0; j < INIT_SIZE_BLK; j++)
      {
          xor_blocks(&text[j * AES_BLOCK_SIZE], &long_state[i * INIT_SIZE_BYTE + j * AES_BLOCK_SIZE]);
          aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], expandedKey);
      }
  }

  oaes_free((OAES_CTX **) &aes_ctx);
  memcpy(state.init, text, INIT_SIZE_BYTE);
  hash_permutation(&state.hs);
  extra_hashes[state.hs.b[0] & 3](&state, 200, hash);
#ifdef FORCE_USE_HEAP
  free(long_state);
#endif
}
#endif /* !aarch64 || !crypto */

#else
// Portable implementation as a fallback

void slow_hash_allocate_state(void)
{
  // Do nothing, this is just to maintain compatibility with the upgraded slow-hash.c
  return;
}

void slow_hash_free_state(void)
{
  // As above
  return;
}

static void (*const extra_hashes[4])(const void *, size_t, char *) = {
  hash_extra_blake, hash_extra_groestl, hash_extra_jh, hash_extra_skein
};

extern int aesb_single_round(const uint8_t *in, uint8_t*out, const uint8_t *expandedKey);
extern int aesb_pseudo_round(const uint8_t *in, uint8_t *out, const uint8_t *expandedKey);

static size_t e2i(const uint8_t* a, size_t count) { return (*((uint64_t*)a) / AES_BLOCK_SIZE) & (count - 1); }

static void mul(const uint8_t* a, const uint8_t* b, uint8_t* res) {
  uint64_t a0, b0;
  uint64_t hi, lo;

  a0 = SWAP64LE(((uint64_t*)a)[0]);
  b0 = SWAP64LE(((uint64_t*)b)[0]);
  lo = mul128(a0, b0, &hi);
  ((uint64_t*)res)[0] = SWAP64LE(hi);
  ((uint64_t*)res)[1] = SWAP64LE(lo);
}

static void sum_half_blocks(uint8_t* a, const uint8_t* b) {
  uint64_t a0, a1, b0, b1;

  a0 = SWAP64LE(((uint64_t*)a)[0]);
  a1 = SWAP64LE(((uint64_t*)a)[1]);
  b0 = SWAP64LE(((uint64_t*)b)[0]);
  b1 = SWAP64LE(((uint64_t*)b)[1]);
  a0 += b0;
  a1 += b1;
  ((uint64_t*)a)[0] = SWAP64LE(a0);
  ((uint64_t*)a)[1] = SWAP64LE(a1);
}
#define U64(x) ((uint64_t *) (x))

static void copy_block(uint8_t* dst, const uint8_t* src) {
  memcpy(dst, src, AES_BLOCK_SIZE);
}

static void swap_blocks(uint8_t *a, uint8_t *b){
  uint64_t t[2];
  U64(t)[0] = U64(a)[0];
  U64(t)[1] = U64(a)[1];
  U64(a)[0] = U64(b)[0];
  U64(a)[1] = U64(b)[1];
  U64(b)[0] = U64(t)[0];
  U64(b)[1] = U64(t)[1];
}

static void xor_blocks(uint8_t* a, const uint8_t* b) {
  size_t i;
  for (i = 0; i < AES_BLOCK_SIZE; i++) {
    a[i] ^= b[i];
  }
}

static void xor64(uint8_t* left, const uint8_t* right)
{
  size_t i;
  for (i = 0; i < 8; ++i)
  {
    left[i] ^= right[i];
  }
}

#pragma pack(push, 1)
union cn_turtle_hash_state {
  union hash_state hs;
  struct {
    uint8_t k[64];
    uint8_t init[INIT_SIZE_BYTE];
  };
};
#pragma pack(pop)

void cn_turtle_hash(const void *data, size_t length, char *hash, int light, int variant, int prehashed, uint32_t page_size, uint32_t scratchpad, uint32_t iterations)
{
  uint32_t init_rounds = (scratchpad / INIT_SIZE_BYTE);
  uint32_t aes_rounds = (iterations / 2);
  size_t aes_init = (page_size / AES_BLOCK_SIZE);

#ifndef FORCE_USE_HEAP
  uint8_t long_state[page_size];
#else
#warning "ACTIVATING FORCE_USE_HEAP IN portable slow-hash.c"
  uint8_t *long_state = (uint8_t *)malloc(page_size);
#endif

  union cn_turtle_hash_state state;
  uint8_t text[INIT_SIZE_BYTE];
  uint8_t a[AES_BLOCK_SIZE];
  uint8_t b[AES_BLOCK_SIZE * 2];
  uint8_t c1[AES_BLOCK_SIZE];
  uint8_t c2[AES_BLOCK_SIZE];
  uint8_t d[AES_BLOCK_SIZE];
  size_t i, j;
  uint8_t aes_key[AES_KEY_SIZE];
  oaes_ctx *aes_ctx;

  if (prehashed) {
    memcpy(&state.hs, data, length);
  } else {
    hash_process(&state.hs, data, length);
  }
  memcpy(text, state.init, INIT_SIZE_BYTE);
  memcpy(aes_key, state.hs.b, AES_KEY_SIZE);
  aes_ctx = (oaes_ctx *) oaes_alloc();

  VARIANT1_PORTABLE_INIT();
  VARIANT2_PORTABLE_INIT();

  oaes_key_import_data(aes_ctx, aes_key, AES_KEY_SIZE);
  for (i = 0; i < init_rounds; i++) {
    for (j = 0; j < INIT_SIZE_BLK; j++) {
      aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], aes_ctx->key->exp_data);
    }
    memcpy(&long_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
  }

  for (i = 0; i < AES_BLOCK_SIZE; i++) {
    a[i] = state.k[     i] ^ state.k[AES_BLOCK_SIZE * 2 + i];
    b[i] = state.k[AES_BLOCK_SIZE + i] ^ state.k[AES_BLOCK_SIZE * 3 + i];
  }

  for (i = 0; i < aes_rounds; i++) {
    /* Dependency chain: address -> read value ------+
     * written value <-+ hard function (AES or MUL) <+
     * next address  <-+
     */
    /* Iteration 1 */
    j = e2i(a, aes_init);
    copy_block(c1, &long_state[j]);
    aesb_single_round(c1, c1, a);
    VARIANT2_PORTABLE_SHUFFLE_ADD(long_state, j);
    copy_block(&long_state[j], c1);
    xor_blocks(&long_state[j], b);
    assert(j == e2i(a, aes_init));
    VARIANT1_1(&long_state[j]);
    /* Iteration 2 */
    j = e2i(c1, aes_init);
    copy_block(c2, &long_state[j]);
    VARIANT2_PORTABLE_INTEGER_MATH(c2, c1);
    mul(c1, c2, d);
    VARIANT2_2_PORTABLE();
    VARIANT2_PORTABLE_SHUFFLE_ADD(long_state, j);
    swap_blocks(a, c1);
    sum_half_blocks(c1, d);
    swap_blocks(c1, c2);
    xor_blocks(c1, c2);
    VARIANT1_2(c2 + 8);
    copy_block(&long_state[j], c2);
    assert(j == e2i(a, aes_init));
    if (variant == 2) {
      copy_block(b + AES_BLOCK_SIZE, b);
    }
    copy_block(b, a);
    copy_block(a, c1);
  }

  memcpy(text, state.init, INIT_SIZE_BYTE);
  oaes_key_import_data(aes_ctx, &state.hs.b[32], AES_KEY_SIZE);
  for (i = 0; i < init_rounds; i++) {
    for (j = 0; j < INIT_SIZE_BLK; j++) {
      xor_blocks(&text[j * AES_BLOCK_SIZE], &long_state[i * INIT_SIZE_BYTE + j * AES_BLOCK_SIZE]);
      aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], aes_ctx->key->exp_data);
    }
  }
  memcpy(state.init, text, INIT_SIZE_BYTE);
  hash_permutation(&state.hs);
  /*memcpy(hash, &state, 32);*/
  extra_hashes[state.hs.b[0] & 3](&state, 200, hash);
  oaes_free((OAES_CTX **) &aes_ctx);

#ifdef FORCE_USE_HEAP
  free(long_state);
#endif
}

#endif
