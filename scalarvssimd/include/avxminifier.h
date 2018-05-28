#include <stdint.h>
#include <x86intrin.h>

#include "simdprune_tables.h"

#ifndef __clang__
static inline __m256i _mm256_loadu2_m128i(__m128i const *__addr_hi,
                                          __m128i const *__addr_lo) {
  __m256i __v256 = _mm256_castsi128_si256(_mm_loadu_si128(__addr_lo));
  return _mm256_insertf128_si256(__v256, _mm_loadu_si128(__addr_hi), 1);
}

static inline void _mm256_storeu2_m128i(__m128i *__addr_hi, __m128i *__addr_lo,
                                        __m256i __a) {
  __m128i __v128;

  __v128 = _mm256_castsi256_si128(__a);
  _mm_storeu_si128(__addr_lo, __v128);
  __v128 = _mm256_extractf128_si256(__a, 1);
  _mm_storeu_si128(__addr_hi, __v128);
}
#endif

// a straightforward comparison of a mask against input.
static uint64_t cmp_mask_against_input_mini(__m256i input_lo, __m256i input_hi,
                                            __m256i mask) {
  __m256i cmp_res_0 = _mm256_cmpeq_epi8(input_lo, mask);
  uint64_t res_0 = (uint32_t)_mm256_movemask_epi8(cmp_res_0);
  __m256i cmp_res_1 = _mm256_cmpeq_epi8(input_hi, mask);
  uint64_t res_1 = _mm256_movemask_epi8(cmp_res_1);
  return res_0 | (res_1 << 32);
}

// take input from buf and remove useless whitespace, input and output can be
// the same
static inline size_t copy_without_useless_spaces(const uint8_t *buf, size_t len,
                                                 uint8_t *out) {
  // Useful constant masks
  const uint64_t even_bits = 0x5555555555555555ULL;
  const uint64_t odd_bits = ~even_bits;
  uint8_t *initout(out);
  uint64_t prev_iter_ends_odd_backslash =
      0ULL;                               // either 0 or 1, but a 64-bit value
  uint64_t prev_iter_inside_quote = 0ULL; // either all zeros or all ones
  size_t idx = 0;
  if (len >= 64) {
    size_t avxlen = len - 63;

    for (; idx < avxlen; idx += 64) {
      __m256i input_lo = _mm256_loadu_si256((const __m256i *)(buf + idx + 0));
      __m256i input_hi = _mm256_loadu_si256((const __m256i *)(buf + idx + 32));
      uint64_t bs_bits = cmp_mask_against_input_mini(input_lo, input_hi,
                                                     _mm256_set1_epi8('\\'));
      uint64_t start_edges = bs_bits & ~(bs_bits << 1);
      uint64_t even_start_mask = even_bits ^ prev_iter_ends_odd_backslash;
      uint64_t even_starts = start_edges & even_start_mask;
      uint64_t odd_starts = start_edges & ~even_start_mask;
      uint64_t even_carries = bs_bits + even_starts;
      uint64_t odd_carries;
      bool iter_ends_odd_backslash = __builtin_uaddll_overflow(
          bs_bits, odd_starts, (unsigned long long *)&odd_carries);
      odd_carries |= prev_iter_ends_odd_backslash;
      prev_iter_ends_odd_backslash = iter_ends_odd_backslash ? 0x1ULL : 0x0ULL;
      uint64_t even_carry_ends = even_carries & ~bs_bits;
      uint64_t odd_carry_ends = odd_carries & ~bs_bits;
      uint64_t even_start_odd_end = even_carry_ends & odd_bits;
      uint64_t odd_start_even_end = odd_carry_ends & even_bits;
      uint64_t odd_ends = even_start_odd_end | odd_start_even_end;
      uint64_t quote_bits = cmp_mask_against_input_mini(input_lo, input_hi,
                                                        _mm256_set1_epi8('"'));
      quote_bits = quote_bits & ~odd_ends;
      uint64_t quote_mask = _mm_cvtsi128_si64(_mm_clmulepi64_si128(
          _mm_set_epi64x(0ULL, quote_bits), _mm_set1_epi8(0xFF), 0));
      quote_mask ^= prev_iter_inside_quote;
      prev_iter_inside_quote = (uint64_t)((s64)quote_mask >> 63);
      const __m256i low_nibble_mask = _mm256_setr_epi8(
          //  0                           9  a   b  c  d
          16, 0, 0, 0, 0, 0, 0, 0, 0, 8, 12, 1, 2, 9, 0, 0, 16, 0, 0, 0, 0, 0,
          0, 0, 0, 8, 12, 1, 2, 9, 0, 0);
      const __m256i high_nibble_mask = _mm256_setr_epi8(
          //  0     2   3     5     7
          8, 0, 18, 4, 0, 1, 0, 1, 0, 0, 0, 3, 2, 1, 0, 0, 8, 0, 18, 4, 0, 1, 0,
          1, 0, 0, 0, 3, 2, 1, 0, 0);
      __m256i whitespace_shufti_mask = _mm256_set1_epi8(0x18);
      __m256i v_lo = _mm256_and_si256(
          _mm256_shuffle_epi8(low_nibble_mask, input_lo),
          _mm256_shuffle_epi8(high_nibble_mask,
                              _mm256_and_si256(_mm256_srli_epi32(input_lo, 4),
                                               _mm256_set1_epi8(0x7f))));

      __m256i v_hi = _mm256_and_si256(
          _mm256_shuffle_epi8(low_nibble_mask, input_hi),
          _mm256_shuffle_epi8(high_nibble_mask,
                              _mm256_and_si256(_mm256_srli_epi32(input_hi, 4),
                                               _mm256_set1_epi8(0x7f))));
      __m256i tmp_ws_lo = _mm256_cmpeq_epi8(
          _mm256_and_si256(v_lo, whitespace_shufti_mask), _mm256_set1_epi8(0));
      __m256i tmp_ws_hi = _mm256_cmpeq_epi8(
          _mm256_and_si256(v_hi, whitespace_shufti_mask), _mm256_set1_epi8(0));

      uint64_t ws_res_0 = (uint32_t)_mm256_movemask_epi8(tmp_ws_lo);
      uint64_t ws_res_1 = _mm256_movemask_epi8(tmp_ws_hi);
      uint64_t whitespace = ~(ws_res_0 | (ws_res_1 << 32));
      whitespace &= ~quote_mask;
      // surprisingly unhelpful:
      // if(whitespace == 0) {
      //    _mm256_storeu_si256((__m256i *)out, input_lo);
      //    _mm256_storeu_si256((__m256i *)(out + 32), input_hi);
      //    out += 64;
      // } else {
      int mask1 = whitespace & 0xFFFF;
      int mask2 = (whitespace >> 16) & 0xFFFF;
      int mask3 = (whitespace >> 32) & 0xFFFF;
      int mask4 = (whitespace >> 48) & 0xFFFF;
      int pop1 = _popcnt64((~whitespace) & 0xFFFF);
      int pop2 = _popcnt64((~whitespace) & UINT64_C(0xFFFFFFFF));
      int pop3 = _popcnt64((~whitespace) & UINT64_C(0xFFFFFFFFFFFF));
      int pop4 = _popcnt64((~whitespace));
      __m256i vmask1 =
          _mm256_loadu2_m128i((const __m128i *)mask128_epi8 + mask2,
                              (const __m128i *)mask128_epi8 + mask1);
      __m256i vmask2 =
          _mm256_loadu2_m128i((const __m128i *)mask128_epi8 + mask4,
                              (const __m128i *)mask128_epi8 + mask3);
      __m256i result1 = _mm256_shuffle_epi8(input_lo, vmask1);
      __m256i result2 = _mm256_shuffle_epi8(input_hi, vmask2);
      _mm256_storeu2_m128i((__m128i *)(out + pop1), (__m128i *)out, result1);
      _mm256_storeu2_m128i((__m128i *)(out + pop3), (__m128i *)(out + pop2),
                           result2);
      out += pop4;
      //}
    }
  }
  // we finish off the job... copying and pasting the code is not ideal here,
  // but it gets the job done.
  if (idx < len) {
    uint8_t buffer[64];
    memset(buffer, 0, 64);
    memcpy(buffer, buf + idx, len - idx);
    __m256i input_lo = _mm256_loadu_si256((const __m256i *)(buffer));
    __m256i input_hi = _mm256_loadu_si256((const __m256i *)(buffer + 32));
    uint64_t bs_bits =
        cmp_mask_against_input_mini(input_lo, input_hi, _mm256_set1_epi8('\\'));
    uint64_t start_edges = bs_bits & ~(bs_bits << 1);
    uint64_t even_start_mask = even_bits ^ prev_iter_ends_odd_backslash;
    uint64_t even_starts = start_edges & even_start_mask;
    uint64_t odd_starts = start_edges & ~even_start_mask;
    uint64_t even_carries = bs_bits + even_starts;
    uint64_t odd_carries;
    bool iter_ends_odd_backslash = __builtin_uaddll_overflow(
        bs_bits, odd_starts, (unsigned long long *)&odd_carries);
    odd_carries |= prev_iter_ends_odd_backslash;
    prev_iter_ends_odd_backslash = iter_ends_odd_backslash ? 0x1ULL : 0x0ULL;
    uint64_t even_carry_ends = even_carries & ~bs_bits;
    uint64_t odd_carry_ends = odd_carries & ~bs_bits;
    uint64_t even_start_odd_end = even_carry_ends & odd_bits;
    uint64_t odd_start_even_end = odd_carry_ends & even_bits;
    uint64_t odd_ends = even_start_odd_end | odd_start_even_end;
    uint64_t quote_bits =
        cmp_mask_against_input_mini(input_lo, input_hi, _mm256_set1_epi8('"'));
    quote_bits = quote_bits & ~odd_ends;
    uint64_t quote_mask = _mm_cvtsi128_si64(_mm_clmulepi64_si128(
        _mm_set_epi64x(0ULL, quote_bits), _mm_set1_epi8(0xFF), 0));
    quote_mask ^= prev_iter_inside_quote;
    prev_iter_inside_quote = (uint64_t)((s64)quote_mask >> 63);
    const __m256i low_nibble_mask = _mm256_setr_epi8(
        //  0                           9  a   b  c  d
        16, 0, 0, 0, 0, 0, 0, 0, 0, 8, 12, 1, 2, 9, 0, 0, 16, 0, 0, 0, 0, 0, 0,
        0, 0, 8, 12, 1, 2, 9, 0, 0);
    const __m256i high_nibble_mask = _mm256_setr_epi8(
        //  0     2   3     5     7
        8, 0, 18, 4, 0, 1, 0, 1, 0, 0, 0, 3, 2, 1, 0, 0, 8, 0, 18, 4, 0, 1, 0,
        1, 0, 0, 0, 3, 2, 1, 0, 0);
    __m256i whitespace_shufti_mask = _mm256_set1_epi8(0x18);
    __m256i v_lo = _mm256_and_si256(
        _mm256_shuffle_epi8(low_nibble_mask, input_lo),
        _mm256_shuffle_epi8(high_nibble_mask,
                            _mm256_and_si256(_mm256_srli_epi32(input_lo, 4),
                                             _mm256_set1_epi8(0x7f))));

    __m256i v_hi = _mm256_and_si256(
        _mm256_shuffle_epi8(low_nibble_mask, input_hi),
        _mm256_shuffle_epi8(high_nibble_mask,
                            _mm256_and_si256(_mm256_srli_epi32(input_hi, 4),
                                             _mm256_set1_epi8(0x7f))));
    __m256i tmp_ws_lo = _mm256_cmpeq_epi8(
        _mm256_and_si256(v_lo, whitespace_shufti_mask), _mm256_set1_epi8(0));
    __m256i tmp_ws_hi = _mm256_cmpeq_epi8(
        _mm256_and_si256(v_hi, whitespace_shufti_mask), _mm256_set1_epi8(0));

    uint64_t ws_res_0 = (uint32_t)_mm256_movemask_epi8(tmp_ws_lo);
    uint64_t ws_res_1 = _mm256_movemask_epi8(tmp_ws_hi);
    uint64_t whitespace = ~(ws_res_0 | (ws_res_1 << 32));
    whitespace &= ~quote_mask;

    //
    if (len - idx < 64) {
      whitespace |= UINT64_C(0xFFFFFFFFFFFFFFFF) << (len - idx);
    }
    int mask1 = whitespace & 0xFFFF;
    int mask2 = (whitespace >> 16) & 0xFFFF;
    int mask3 = (whitespace >> 32) & 0xFFFF;
    int mask4 = (whitespace >> 48) & 0xFFFF;
    //    dumpbits(whitespace,"whitespace");
    int pop1 = _popcnt64((~whitespace) & 0xFFFF);
    int pop2 = _popcnt64((~whitespace) & UINT64_C(0xFFFFFFFF));
    int pop3 = _popcnt64((~whitespace) & UINT64_C(0xFFFFFFFFFFFF));
    int pop4 = _popcnt64((~whitespace));
    __m256i vmask1 = _mm256_loadu2_m128i((const __m128i *)mask128_epi8 + mask2,
                                         (const __m128i *)mask128_epi8 + mask1);
    __m256i vmask2 = _mm256_loadu2_m128i((const __m128i *)mask128_epi8 + mask4,
                                         (const __m128i *)mask128_epi8 + mask3);
    __m256i result1 = _mm256_shuffle_epi8(input_lo, vmask1);
    __m256i result2 = _mm256_shuffle_epi8(input_hi, vmask2);
    _mm256_storeu2_m128i((__m128i *)(buffer + pop1), (__m128i *)buffer,
                         result1);
    _mm256_storeu2_m128i((__m128i *)(buffer + pop3), (__m128i *)(buffer + pop2),
                         result2);
    memcpy(out, buffer, pop4);
    out += pop4;
  }
  return out - initout;
}
