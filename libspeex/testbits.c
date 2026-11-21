/* Copyright (C) 2026 Xiph.Org Foundation
   File: testbits.c

   Boundary tests for the SpeexBits byte-oriented API, in particular
   speex_bits_read_whole_bytes() with owned and non-owned buffers
   (regression tests for xiph/speex issues #25 and #42).

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <speex/speex_bits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__); \
      fputc('\n', stderr); \
      failures++; \
   } \
} while (0)

/* Read the whole stream back as a sequence of bytes and compare it with
   "expected". Returns the number of bytes read. */
static int stream_bytes(const SpeexBits *bits_in, char *out, int max_out)
{
   SpeexBits bits = *bits_in;
   int i, n = speex_bits_nbytes(&bits);
   if (n > max_out)
      return -1;
   speex_bits_rewind(&bits);
   for (i = 0; i < n; i++)
      out[i] = (char)speex_bits_unpack_unsigned(&bits, 8);
   return n;
}

static void fill_pattern(char *buf, int len, int seed)
{
   int i;
   for (i = 0; i < len; i++)
      buf[i] = (char)((seed + i*7) & 0x7f);
}

/* Issue #42 follow-up: a full non-owned buffer used to make the truncated
   length -1, which shifted a negative value and dropped queued data. */
static void test_issue42_poc(void)
{
   char a[4] = {0x54, 0x72, (char)0xd4, (char)0xb9}, b[16];
   /* Heap allocation with the exact size so ASan guards the end. */
   char *buf = (char*)calloc(1, 16);
   SpeexBits bits;
   int i;
   for (i = 0; i < 16; i++)
      b[i] = (char)(0x10 + i);

   speex_bits_init_buffer(&bits, buf, 16);
   speex_bits_read_whole_bytes(&bits, b, 16); /* fills the buffer */
   speex_bits_advance(&bits, 56);             /* 9 unread bytes */
   speex_bits_read_whole_bytes(&bits, a, 4);  /* must not truncate to -1 */

   CHECK(speex_bits_nbytes(&bits) == 13,
         "issue42 PoC: expected 13 queued bytes, got %d",
         speex_bits_nbytes(&bits));
   speex_bits_destroy(&bits);
   free(buf);
}

/* Exhaustive matrix over non-owned buffers: every fill level, every number
   of consumed bytes and several append sizes. The backing allocation always
   has exactly buf_size bytes, so any out-of-bounds write is caught by ASan. */
static void test_nonowned_matrix(void)
{
   static const int sizes[] = {1, 2, 3, 4, 8, 16, 32};
   int si;
   for (si = 0; si < (int)(sizeof(sizes)/sizeof(*sizes)); si++) {
      int n = sizes[si];
      int fill;
      for (fill = 0; fill <= n; fill++) {
         int consumed;
         for (consumed = 0; consumed <= fill; consumed++) {
            int append;
            for (append = 0; append <= n + 3; append++) {
               char *backing = (char*)malloc((size_t)n);
               char initial[40];
               char input[40];
               char expected[40];
               char got[40];
               int unread, room, taken, exp_nbytes, got_nbytes;
               SpeexBits bits;

               fill_pattern(backing, n, 1);
               fill_pattern(initial, fill, 1);
               fill_pattern(input, append, 100);
               /* Bytes surviving the flush are the unread prefix of the
                  original stream. */
               memcpy(expected, initial + consumed, (size_t)(fill - consumed));

               speex_bits_init_buffer(&bits, backing, n);
               if (fill > 0)
                  speex_bits_read_whole_bytes(&bits, initial, fill);
               if (consumed > 0)
                  speex_bits_advance(&bits, consumed*8);
               speex_bits_read_whole_bytes(&bits, input, append);

               unread = fill - consumed;
               /* Model the implementation: truncation only kicks in when
                  the append would overflow; in that case one byte of
                  headroom is kept (same expression as the owned-buffer
                  realloc fallback), clamped at zero. The flush above has
                  already reclaimed the space of the consumed bytes. */
               if (unread + append > n)
                  room = n - unread - 1;
               else
                  room = append;
               if (room < 0)
                  room = 0;
               taken = append < room ? append : room;
               exp_nbytes = unread + taken;
               memcpy(expected + unread, input, (size_t)taken);

               got_nbytes = stream_bytes(&bits, got, (int)sizeof(got));
               CHECK(got_nbytes == exp_nbytes,
                     "n=%d fill=%d consumed=%d append=%d: nbytes %d != %d",
                     n, fill, consumed, append, got_nbytes, exp_nbytes);
               CHECK(bits.nbBits == exp_nbytes*8,
                     "n=%d fill=%d consumed=%d append=%d: nbBits %d != %d",
                     n, fill, consumed, append, bits.nbBits, exp_nbytes*8);
               if (got_nbytes == exp_nbytes)
                  CHECK(memcmp(got, expected, (size_t)exp_nbytes) == 0,
                        "n=%d fill=%d consumed=%d append=%d: content mismatch",
                        n, fill, consumed, append);
               speex_bits_destroy(&bits);
               free(backing);
            }
         }
      }
   }
}

/* A single exact-fit append must not trigger any truncation. */
static void test_nonowned_exact_fit(void)
{
   enum { N = 16 };
   char *backing = (char*)malloc(N);
   char input[N];
   char got[N];
   SpeexBits bits;
   fill_pattern(input, N, 37);
   speex_bits_init_buffer(&bits, backing, N);
   speex_bits_read_whole_bytes(&bits, input, N);
   CHECK(speex_bits_nbytes(&bits) == N, "exact fit nbytes");
   CHECK(stream_bytes(&bits, got, N) == N && memcmp(got, input, N) == 0,
         "exact fit content");
   speex_bits_destroy(&bits);
   free(backing);
}

/* Owned buffers grow on demand and keep every appended byte. */
static void test_owned_grows(void)
{
   SpeexBits bits;
   char input[2100];
   char got[2100];
   int i;
   fill_pattern(input, (int)sizeof(input), 9);
   speex_bits_init(&bits);
   speex_bits_read_whole_bytes(&bits, input, (int)sizeof(input));
   CHECK(speex_bits_nbytes(&bits) == (int)sizeof(input),
         "owned oversize nbytes %d", speex_bits_nbytes(&bits));
   CHECK(stream_bytes(&bits, got, (int)sizeof(got)) == (int)sizeof(input)
         && memcmp(got, input, sizeof(input)) == 0,
         "owned oversize content");

   /* Sequential appends interleaved with reads (exercises the flush):
      each of the 50 rounds adds 50 bytes and consumes 25, so the first
      round leaves 25 bytes queued and every later round adds 25 more. */
   speex_bits_reset(&bits);
   for (i = 0; i < 50; i++) {
      char chunk[50];
      fill_pattern(chunk, 50, i);
      speex_bits_read_whole_bytes(&bits, chunk, 50);
      speex_bits_advance(&bits, 25*8);
   }
   CHECK(speex_bits_nbytes(&bits) == 50*50 - 49*25,
         "owned sequential nbytes %d", speex_bits_nbytes(&bits));
   speex_bits_destroy(&bits);
}

/* Issue #25: speex_bits_unpack_signed() used to left-shift a negative
   signed value (undefined behavior). Check two's-complement results for
   negative decodings across all widths. */
static void test_issue25_signed(void)
{
   SpeexBits bits;
   char storage[256];
   int width;
   speex_bits_init_buffer(&bits, storage, (int)sizeof(storage));
   for (width = 1; width <= 31; width++) {
      unsigned int u;
      int expected, got;
      /* Value with the sign bit set and some varying low bits. */
      u = (1u << (width - 1)) | (unsigned)(width & 1);
      speex_bits_reset(&bits);
      speex_bits_pack(&bits, (int)u, width);
      speex_bits_rewind(&bits);
      got = speex_bits_unpack_signed(&bits, width);
      expected = (int)(u - (1u << width));
      CHECK(got == expected, "width %d: signed %d != %d", width, got, expected);
   }
   /* Positive values must decode unchanged as well. */
   for (width = 2; width <= 31; width++) {
      unsigned int u = (unsigned)(width*3);
      int got;
      if (u >= (1u << (width - 1)))
         continue;
      speex_bits_reset(&bits);
      speex_bits_pack(&bits, (int)u, width);
      speex_bits_rewind(&bits);
      got = speex_bits_unpack_signed(&bits, width);
      CHECK(got == (int)u, "width %d: positive %d != %u", width, got, u);
   }
   speex_bits_destroy(&bits);
}

/* The byte-oriented API operates on byte-aligned streams: appending after
   some bytes have been consumed must flush the unread prefix and place the
   new bytes right after it without corrupting anything. */
static void test_interleave(void)
{
   SpeexBits bits;
   char storage[128];
   char head[2] = {(char)0xa5, 0x11};
   char chunk[4] = {0x22, 0x33, 0x44, 0x5a};
   char more[2] = {0x66, 0x77};

   speex_bits_init_buffer(&bits, storage, (int)sizeof(storage));
   speex_bits_read_whole_bytes(&bits, head, 2);
   speex_bits_read_whole_bytes(&bits, chunk, 4);

   /* Consume the first two bytes, then append more: the flush moves the
      remaining bytes to the start and the new data follows them. */
   speex_bits_rewind(&bits);
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0xa5, "interleave byte 0");
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x11, "interleave byte 1");
   speex_bits_read_whole_bytes(&bits, more, 2);
   CHECK(speex_bits_nbytes(&bits) == 6, "interleave nbytes after flush+append");

   speex_bits_rewind(&bits);
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x22, "interleave byte 2");
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x33, "interleave byte 3");
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x44, "interleave byte 4");
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x5a, "interleave byte 5");
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x66, "interleave byte 6");
   CHECK(speex_bits_unpack_unsigned(&bits, 8) == 0x77, "interleave byte 7");
   speex_bits_destroy(&bits);
}

int main(void)
{
   test_issue42_poc();
   test_nonowned_exact_fit();
   test_nonowned_matrix();
   test_owned_grows();
   test_issue25_signed();
   test_interleave();
   if (failures) {
      fprintf(stderr, "testbits: %d failure(s)\n", failures);
      return 1;
   }
   printf("testbits: all tests passed\n");
   return 0;
}
