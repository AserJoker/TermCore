#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <termcore/tc_text.h>

namespace {

struct Decoded {
    tc_utf8_err err;
    uint32_t    cp;
    size_t      consumed;   /* bytes the input pointer advanced */
};

/* Feeds exactly len bytes (copied into a scratch buffer so the end pointer is
 * precise) and reports what tc_text_utf8_next does with them. */
Decoded decode_bytes(const unsigned char* bytes, size_t len) {
    char buf[16];
    std::memcpy(buf, bytes, len);

    const char* p   = buf;
    const char* end = buf + len;
    uint32_t    cp  = 0;
    tc_utf8_err e   = tc_text_utf8_next(&p, end, &cp);

    Decoded d;
    d.err      = e;
    d.cp       = cp;
    d.consumed = static_cast<size_t>(p - buf);
    return d;
}

}  // namespace

/* --------------------------------------------------------------------------
 * tc_text_utf8_next — valid sequences
 * ------------------------------------------------------------------------ */

TEST(Utf8Next, AsciiBoundaries) {
    auto n   = decode_bytes(reinterpret_cast<const unsigned char*>("\x00"), 1);
    auto hi  = decode_bytes(reinterpret_cast<const unsigned char*>("\x7F"), 1);
    EXPECT_EQ(n.err, TC_UTF8_OK);
    EXPECT_EQ(n.cp, 0x00u);
    EXPECT_EQ(n.consumed, 1u);
    EXPECT_EQ(hi.cp, 0x7Fu);
}

TEST(Utf8Next, TwoByteBoundaries) {
    const unsigned char lo[] = {0xC2, 0x80};   /* U+0080 */
    const unsigned char hi[] = {0xDF, 0xBF};   /* U+07FF */
    EXPECT_EQ(decode_bytes(lo, 2).cp, 0x0080u);
    EXPECT_EQ(decode_bytes(hi, 2).cp, 0x07FFu);
    EXPECT_EQ(decode_bytes(lo, 2).err, TC_UTF8_OK);
    EXPECT_EQ(decode_bytes(lo, 2).consumed, 2u);
}

TEST(Utf8Next, ThreeByteBoundaries) {
    const unsigned char b0[] = {0xE0, 0xA0, 0x80};   /* U+0800 */
    const unsigned char b1[] = {0xED, 0x9F, 0xBF};   /* U+D7FF (below surrogates) */
    const unsigned char b2[] = {0xEE, 0x80, 0x80};   /* U+E000 (above surrogates) */
    const unsigned char b3[] = {0xEF, 0xBF, 0xBF};   /* U+FFFF */

    auto r0 = decode_bytes(b0, 3);
    EXPECT_EQ(r0.err, TC_UTF8_OK);
    EXPECT_EQ(r0.cp, 0x0800u);
    EXPECT_EQ(r0.consumed, 3u);

    EXPECT_EQ(decode_bytes(b1, 3).cp, 0xD7FFu);
    EXPECT_EQ(decode_bytes(b2, 3).cp, 0xE000u);
    EXPECT_EQ(decode_bytes(b3, 3).cp, 0xFFFFu);
}

TEST(Utf8Next, FourByteBoundaries) {
    const unsigned char lo[] = {0xF0, 0x90, 0x80, 0x80};   /* U+10000 */
    const unsigned char hi[] = {0xF4, 0x8F, 0xBF, 0xBF};   /* U+10FFFF */

    auto r = decode_bytes(lo, 4);
    EXPECT_EQ(r.err, TC_UTF8_OK);
    EXPECT_EQ(r.cp, 0x10000u);
    EXPECT_EQ(r.consumed, 4u);

    EXPECT_EQ(decode_bytes(hi, 4).cp, 0x10FFFFu);
}

TEST(Utf8Next, CjkSampleRoundTrip) {
    /* 你 = U+4F60 = E4 BD A0; 中 = U+4E2D = E4 B8 AD */
    const unsigned char ni[] = {0xE4, 0xBD, 0xA0};
    const unsigned char zh[] = {0xE4, 0xB8, 0xAD};
    EXPECT_EQ(decode_bytes(ni, 3).cp, 0x4F60u);
    EXPECT_EQ(decode_bytes(zh, 3).cp, 0x4E2Du);
}

/* --------------------------------------------------------------------------
 * tc_text_utf8_next — invalid sequences
 * ------------------------------------------------------------------------ */

TEST(Utf8Next, OverlongHeadsAreInvalid) {
    const unsigned char c0[]  = {0xC0, 0x80};   /* encodes NUL, forbidden */
    const unsigned char c1[]  = {0xC1, 0xBF};   /* encodes U+7F, forbidden */
    const unsigned char e0[]  = {0xE0, 0x80, 0x80};   /* overlong 3-byte */
    const unsigned char e09[] = {0xE0, 0x9F, 0xBF};   /* E0 followed by <A0 */
    const unsigned char f0[]  = {0xF0, 0x80, 0x80, 0x80};  /* overlong 4-byte */
    const unsigned char f08[] = {0xF0, 0x8F, 0xBF, 0xBF};  /* F0 followed by <90 */

    for (const auto* seq : {c0, c1, e0, e09, f0, f08}) {
        /* Length is implicit from the first byte pattern; pass the full array
         * and expect a single-byte-consumed INVALID. */
        size_t len = (seq[0] == 0xC0 || seq[0] == 0xC1) ? 2
                   : (seq[0] == 0xE0)                  ? 3
                                                       : 4;
        auto r = decode_bytes(seq, len);
        EXPECT_EQ(r.err, TC_UTF8_INVALID) << "head byte " << static_cast<int>(seq[0]);
        EXPECT_EQ(r.cp, 0xFFFDu);
        EXPECT_EQ(r.consumed, 1u);
    }
}

TEST(Utf8Next, SurrogatesAreInvalid) {
    const unsigned char lo[] = {0xED, 0xA0, 0x80};   /* U+D800 */
    const unsigned char hi[] = {0xED, 0xBF, 0xBF};   /* U+DFFF */

    auto r = decode_bytes(lo, 3);
    EXPECT_EQ(r.err, TC_UTF8_INVALID);
    EXPECT_EQ(r.cp, 0xFFFDu);
    EXPECT_EQ(r.consumed, 1u);

    r = decode_bytes(hi, 3);
    EXPECT_EQ(r.err, TC_UTF8_INVALID);
    EXPECT_EQ(r.cp, 0xFFFDu);
    EXPECT_EQ(r.consumed, 1u);
}

TEST(Utf8Next, OutOfRangeHeadsAreInvalid) {
    const unsigned char f4[] = {0xF4, 0x90, 0x80, 0x80};   /* U+110000, > U+10FFFF */
    const unsigned char f5[] = {0xF5, 0x80, 0x80, 0x80};
    const unsigned char ff[] = {0xFF};

    for (const auto* seq : {f4, f5}) {
        auto r = decode_bytes(seq, 4);
        EXPECT_EQ(r.err, TC_UTF8_INVALID);
        EXPECT_EQ(r.cp, 0xFFFDu);
        EXPECT_EQ(r.consumed, 1u);
    }

    auto r = decode_bytes(ff, 1);
    EXPECT_EQ(r.err, TC_UTF8_INVALID);
    EXPECT_EQ(r.cp, 0xFFFDu);
    EXPECT_EQ(r.consumed, 1u);
}

TEST(Utf8Next, LoneContinuationBytesAreInvalid) {
    const unsigned char lo[] = {0x80};
    const unsigned char hi[] = {0xBF};

    for (const auto* seq : {lo, hi}) {
        auto r = decode_bytes(seq, 1);
        EXPECT_EQ(r.err, TC_UTF8_INVALID);
        EXPECT_EQ(r.cp, 0xFFFDu);
        EXPECT_EQ(r.consumed, 1u);
    }
}

TEST(Utf8Next, BadContinuationInsideSequenceConsumesOneByte) {
    const unsigned char c2[] = {0xC2, 0x41};   /* 'A' is not a continuation */
    const unsigned char e0[] = {0xE0, 0xA0, 0x41};
    const unsigned char f0[] = {0xF0, 0x90, 0x80, 0x41};

    for (const auto* seq : {c2, e0, f0}) {
        size_t len = (seq[0] == 0xC2) ? 2 : (seq[0] == 0xE0) ? 3 : 4;
        auto r = decode_bytes(seq, len);
        EXPECT_EQ(r.err, TC_UTF8_INVALID);
        EXPECT_EQ(r.cp, 0xFFFDu);
        EXPECT_EQ(r.consumed, 1u);
    }
}

TEST(Utf8Next, InvalidConsumesOneByteThenResyncs) {
    /* The invalid head byte is eaten, the following byte is re-examined
     * (no swallowing), per the docs/05 §4 error-recovery contract. */
    char seq[] = {'\xC2', 'A'};
    const char* p   = seq;
    const char* end = seq + sizeof(seq);
    uint32_t    cp  = 0;

    ASSERT_EQ(tc_text_utf8_next(&p, end, &cp), TC_UTF8_INVALID);
    EXPECT_EQ(cp, 0xFFFDu);
    EXPECT_EQ(p, seq + 1);

    EXPECT_EQ(tc_text_utf8_next(&p, end, &cp), TC_UTF8_OK);
    EXPECT_EQ(cp, 0x41u);
    EXPECT_EQ(p, end);
}

/* --------------------------------------------------------------------------
 * tc_text_utf8_next — incomplete sequences (do NOT advance the pointer)
 * ------------------------------------------------------------------------ */

TEST(Utf8Next, TruncatedSequencesReportIncomplete) {
    const unsigned char c2[]    = {0xC2};              /* 2-byte head only */
    const unsigned char e0a0[]  = {0xE0, 0xA0};        /* 3-byte head + 1 cont */
    const unsigned char f09080[] = {0xF0, 0x90, 0x80}; /* 4-byte head + 2 cont */
    const unsigned char ascii[] = {'x'};

    auto r = decode_bytes(c2, 1);
    EXPECT_EQ(r.err, TC_UTF8_INCOMPLETE);
    EXPECT_EQ(r.consumed, 0u);

    r = decode_bytes(e0a0, 2);
    EXPECT_EQ(r.err, TC_UTF8_INCOMPLETE);
    EXPECT_EQ(r.consumed, 0u);

    r = decode_bytes(f09080, 3);
    EXPECT_EQ(r.err, TC_UTF8_INCOMPLETE);
    EXPECT_EQ(r.consumed, 0u);

    /* A lone ASCII byte is complete even when it is the last byte. */
    r = decode_bytes(ascii, 1);
    EXPECT_EQ(r.err, TC_UTF8_OK);
    EXPECT_EQ(r.consumed, 1u);

    /* Empty input is incomplete with no advance. */
    r = decode_bytes(reinterpret_cast<const unsigned char*>(""), 0);
    EXPECT_EQ(r.err, TC_UTF8_INCOMPLETE);
    EXPECT_EQ(r.consumed, 0u);
}

TEST(Utf8Next, StreamedFeedingOfCjk) {
    /* 你 = E4 BD A0, fed one byte at a time across chunk boundaries. The
     * caller keeps the residual bytes and retries — the pointer must not
     * advance until the sequence is complete. */
    const unsigned char you[] = {0xE4, 0xBD, 0xA0};

    auto r1 = decode_bytes(you, 1);
    EXPECT_EQ(r1.err, TC_UTF8_INCOMPLETE);
    EXPECT_EQ(r1.consumed, 0u);

    auto r2 = decode_bytes(you, 2);
    EXPECT_EQ(r2.err, TC_UTF8_INCOMPLETE);
    EXPECT_EQ(r2.consumed, 0u);

    auto r3 = decode_bytes(you, 3);
    EXPECT_EQ(r3.err, TC_UTF8_OK);
    EXPECT_EQ(r3.cp, 0x4F60u);
    EXPECT_EQ(r3.consumed, 3u);
}

TEST(Utf8Next, NullArgumentsAreInvalid) {
    const char* p = "x";
    uint32_t    cp = 0;

    EXPECT_EQ(tc_text_utf8_next(nullptr, p + 1, &cp), TC_UTF8_INVALID);
    EXPECT_EQ(tc_text_utf8_next(&p, nullptr, &cp), TC_UTF8_INVALID);
    EXPECT_EQ(tc_text_utf8_next(&p, p + 1, nullptr), TC_UTF8_INVALID);
}

/* --------------------------------------------------------------------------
 * tc_text_utf8_encode
 * ------------------------------------------------------------------------ */

TEST(Utf8Encode, BoundaryLengths) {
    struct Case { uint32_t cp; size_t len; unsigned char out[4]; };
    const Case cases[] = {
        {0x000000, 1, {0x00}},
        {0x00007F, 1, {0x7F}},
        {0x000080, 2, {0xC2, 0x80}},
        {0x0007FF, 2, {0xDF, 0xBF}},
        {0x000800, 3, {0xE0, 0xA0, 0x80}},
        {0x00D7FF, 3, {0xED, 0x9F, 0xBF}},
        {0x00E000, 3, {0xEE, 0x80, 0x80}},
        {0x00FFFF, 3, {0xEF, 0xBF, 0xBF}},
        {0x010000, 4, {0xF0, 0x90, 0x80, 0x80}},
        {0x10FFFF, 4, {0xF4, 0x8F, 0xBF, 0xBF}},
    };

    for (const auto& c : cases) {
        char buf[4];
        std::memset(buf, 0xAB, sizeof(buf));
        size_t n = tc_text_utf8_encode(c.cp, buf);
        EXPECT_EQ(n, c.len) << "cp " << static_cast<unsigned int>(c.cp);
        EXPECT_EQ(std::memcmp(buf, c.out, c.len), 0) << "cp " << static_cast<unsigned int>(c.cp);
    }
}

TEST(Utf8Encode, UnencodableReturnsZero) {
    char buf[4];
    EXPECT_EQ(tc_text_utf8_encode(0xD800u, buf), 0u);   /* surrogate */
    EXPECT_EQ(tc_text_utf8_encode(0xDFFFu, buf), 0u);   /* surrogate */
    EXPECT_EQ(tc_text_utf8_encode(0x110000u, buf), 0u); /* beyond U+10FFFF */
    EXPECT_EQ(tc_text_utf8_encode(0xFFFFFFFFu, buf), 0u);
}

TEST(Utf8Encode, NullOutIsZero) {
    EXPECT_EQ(tc_text_utf8_encode(0x41u, nullptr), 0u);
}

TEST(Utf8Encode, RoundTrips) {
    const uint32_t samples[] = {
        0x0000u, 0x0041u, 0x007Fu, 0x0080u, 0x07FFu,
        0x0800u, 0x4F60u, 0xFFFFu, 0x10000u, 0x10FFFFu,
    };
    for (uint32_t cp : samples) {
        char buf[4];
        size_t n = tc_text_utf8_encode(cp, buf);
        ASSERT_GT(n, 0u);

        const char* p = buf;
        const char* end = buf + n;
        uint32_t back = 0;
        ASSERT_EQ(tc_text_utf8_next(&p, end, &back), TC_UTF8_OK);
        EXPECT_EQ(back, cp);
        EXPECT_EQ(p, end);
    }
}

/* --------------------------------------------------------------------------
 * tc_text_utf8_validate
 * ------------------------------------------------------------------------ */

TEST(Utf8Validate, ValidStringsPass) {
    const char* ascii = "hello";
    const char* cjk   = "\xE4\xBD\xA0\xE4\xB8\xAD";   /* 你中 */
    const char* mixed = "a\xC2\xA2z\xF0\x9F\x98\x80";  /* a ¢ z 😀 */
    const char* empty = "";

    size_t bad = 123;
    EXPECT_EQ(tc_text_utf8_validate(ascii, ascii + 5, &bad), TC_UTF8_OK);
    EXPECT_EQ(tc_text_utf8_validate(cjk, cjk + 6, &bad), TC_UTF8_OK);
    EXPECT_EQ(tc_text_utf8_validate(mixed, mixed + 8, &bad), TC_UTF8_OK);
    EXPECT_EQ(tc_text_utf8_validate(empty, empty, &bad), TC_UTF8_OK);
}

TEST(Utf8Validate, InvalidReportsFirstOffset) {
    const char* lone   = "\xC2";
    const char* mid    = "abc\xFF";              /* bad byte at offset 3 */
    const char* broken = "\xE4\xBD";             /* truncated 你 */
    const char* surf   = "x\xED\xA0\x80";        /* surrogate at offset 1 */

    size_t bad = 0;
    EXPECT_EQ(tc_text_utf8_validate(lone, lone + 1, &bad), TC_UTF8_INVALID);
    EXPECT_EQ(bad, 0u);

    EXPECT_EQ(tc_text_utf8_validate(mid, mid + 4, &bad), TC_UTF8_INVALID);
    EXPECT_EQ(bad, 3u);

    EXPECT_EQ(tc_text_utf8_validate(broken, broken + 2, &bad), TC_UTF8_INVALID);
    EXPECT_EQ(bad, 0u);

    EXPECT_EQ(tc_text_utf8_validate(surf, surf + 4, &bad), TC_UTF8_INVALID);
    EXPECT_EQ(bad, 1u);
}

TEST(Utf8Validate, NullArguments) {
    const char* s = "x";
    size_t bad = 0;
    EXPECT_EQ(tc_text_utf8_validate(nullptr, s + 1, &bad), TC_UTF8_INVALID);
    EXPECT_EQ(tc_text_utf8_validate(s, nullptr, &bad), TC_UTF8_INVALID);
}
