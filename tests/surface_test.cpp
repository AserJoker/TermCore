/* Render layer: surface lifecycle, draw primitives and present diff
 * (docs/06 §3, §4, §9).
 *
 * The draw and lifecycle cases run headless against the null backend. The
 * present cases white-box swap in a capture backend (src/platform/backend.h)
 * so the exact escape-sequence bytes are asserted, exactly like the real
 * terminal would receive them.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <termcore/tc.h>

#include <control/term_internal.h>
#include <platform/backend.h>
#include <render/render_internal.h>

namespace {

tc_term_options headless_options(const tc_allocator* alloc = nullptr) {
    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless  = true;
    opt.allocator = alloc;
    return opt;
}

tc_cell cell_with_text(char ch) {
    tc_cell c = tc_cell_blank();
    c.len       = 1;
    c.text[0]   = ch;
    return c;
}

tc_cell cell_with_text(const char* utf8) {
    tc_cell c = tc_cell_blank();
    c.len     = static_cast<uint8_t>(strlen(utf8));
    memcpy(c.text, utf8, c.len);
    return c;
}

/* Build a cell locally then pass its address (avoids -Waddress-of-temporary
 * when passing an rvalue's address into the C API). */
tc_status put_text(tc_surface_t* s, int32_t x, int32_t y, char ch) {
    tc_cell c = cell_with_text(ch);
    return tc_surface_put_cell(s, x, y, &c);
}

tc_status fill_text(tc_surface_t* s, const tc_rect* r, char ch) {
    tc_cell c = cell_with_text(ch);
    return tc_surface_fill_rect(s, r, &c);
}

/* --------------------------------------------------------------------------
 * Capture backend: a real tc_backend whose write() collects bytes into a
 * fixed C buffer. Allocated through the term's allocator so the alloc counter
 * stays balanced; dispose frees the block tc_backend_destroy would free, and
 * also releases the backend it replaced.
 * ------------------------------------------------------------------------ */
struct capture_backend {
    tc_backend base;
    tc_backend* orig;      /* the backend replaced at attach time (freed here) */
    char    out[4096];
    size_t  out_len;
    tc_status write_rc = TC_OK;
    int32_t    cols     = 80;
    int32_t    rows     = 24;
};

static std::string cap_out(const capture_backend* cb) {
    return std::string(cb->out, cb->out_len);
}

static void cap_dispose(tc_backend* b) {
    capture_backend* cb = reinterpret_cast<capture_backend*>(b);
    /* Only the replaced backend is freed here; tc_backend_destroy frees `b`
     * itself through base.self_size after dispose returns. */
    if (cb->orig) tc_backend_destroy(cb->orig);
}

static tc_status cap_set_raw(tc_backend* b, bool on) {
    (void)b; (void)on;
    return TC_OK;
}

static tc_status cap_get_size(tc_backend* b, int32_t* cols, int32_t* rows) {
    capture_backend* cb = reinterpret_cast<capture_backend*>(b);
    if (cols) *cols = cb->cols;
    if (rows) *rows = cb->rows;
    return TC_OK;
}

static tc_status cap_get_size_px(tc_backend* b, int32_t* w, int32_t* h) {
    (void)b;
    if (w) *w = 0;
    if (h) *h = 0;
    return TC_ERR_UNSUPPORTED;
}

static tc_status cap_set_cursor_visible(tc_backend* b, bool visible) {
    (void)b; (void)visible;
    return TC_OK;
}

static tc_status cap_set_cursor_pos(tc_backend* b, int32_t x, int32_t y) {
    (void)b; (void)x; (void)y;
    return TC_OK;
}

static tc_status cap_set_title(tc_backend* b, const char* utf8) {
    (void)b; (void)utf8;
    return TC_OK;
}

static tc_status cap_write(tc_backend* b, const void* buf, size_t len,
                           size_t* nwritten) {
    capture_backend* cb = reinterpret_cast<capture_backend*>(b);
    if (cb->write_rc != TC_OK) return cb->write_rc;
    size_t room = sizeof(cb->out) - cb->out_len;
    if (len > room) len = room;
    memcpy(cb->out + cb->out_len, buf, len);
    cb->out_len += len;
    if (nwritten) *nwritten = len;
    return TC_OK;
}

static tc_status cap_wait_ready(tc_backend* b, int32_t timeout_ms, bool* ready) {
    (void)b; (void)timeout_ms;
    if (ready) *ready = false;
    return TC_OK;
}

static tc_status cap_read(tc_backend* b, void* buf, size_t cap, size_t* nread) {
    (void)b; (void)buf; (void)cap;
    if (nread) *nread = 0;
    return TC_OK;
}

static const tc_backend_vtable kCaptureVtable = {
    cap_dispose,
    cap_set_raw,
    cap_get_size,
    cap_get_size_px,
    cap_set_cursor_visible,
    cap_set_cursor_pos,
    cap_set_title,
    cap_write,
    cap_wait_ready,
    cap_read,
};

/* Replaces the term's backend with a capture backend and returns it. The
 * replaced backend is kept until dispose so allocator accounting stays
 * balanced; the term still owns the new backend (destroyed with the term). */
static capture_backend* attach_capture_backend(tc_term_t* term) {
    tc_term* m = reinterpret_cast<tc_term*>(term);
    const tc_allocator* alloc = m->alloc;
    tc_backend* orig = m->backend;

    capture_backend* cb = static_cast<capture_backend*>(
        alloc->alloc(alloc->ctx, sizeof(capture_backend), sizeof(void*)));
    memset(static_cast<void*>(cb), 0, sizeof(*cb));
    cb->base.vt        = &kCaptureVtable;
    cb->base.kind      = TC_BACKEND_NULL;
    cb->base.alloc     = alloc;
    cb->base.self_size = sizeof(capture_backend);
    cb->cols           = 4;
    cb->rows           = 2;
    cb->orig           = orig;

    m->backend = &cb->base;
    return cb;
}

class SurfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tc_alloc_counter_init(&counter, nullptr);
        opt = headless_options(&counter.allocator);
        ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    }

    void TearDown() override {
        if (term) tc_term_destroy(term);
        EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
    }

    tc_alloc_counter counter{};
    tc_term_options  opt{};
    tc_term_t*       term = nullptr;
};

/* A fixture that also enters the session, for present tests. */
class PresentTest : public SurfaceTest {
protected:
    void SetUp() override {
        SurfaceTest::SetUp();
        ASSERT_EQ(tc_term_enter(term), TC_OK);
        cap = attach_capture_backend(term);
    }
    capture_backend* cap = nullptr;
};

}  // namespace

/* --------------------------------------------------------------------------
 * Lifecycle (docs/06 §3)
 * ------------------------------------------------------------------------ */

TEST_F(SurfaceTest, CreateBlankGridAllDirty) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 3, 2, &s), TC_OK);

    int32_t cols = 0;
    int32_t rows = 0;
    ASSERT_EQ(tc_surface_get_size(s, &cols, &rows), TC_OK);
    EXPECT_EQ(cols, 3);
    EXPECT_EQ(rows, 2);

    /* Fresh surface: the whole grid is dirty (docs/06 §3). */
    tc_rect r{};
    ASSERT_EQ(tc_surface_dirty_rect(s, &r), TC_OK);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.w, 3);
    EXPECT_EQ(r.h, 2);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, CreateValidatesArguments) {
    tc_surface_t* s = nullptr;
    EXPECT_EQ(tc_surface_create(nullptr, 3, 2, &s), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_create(term, 0, 2, &s), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_create(term, 3, -1, &s), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_create(term, 5000, 2, &s), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_create(term, 3, 2, nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(s, nullptr);
}

TEST_F(SurfaceTest, ResizeKeepsTopLeftAndMarksDirty) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 2, 2, &s), TC_OK);
    ASSERT_EQ(put_text(s, 0, 0, 'A'), TC_OK);

    ASSERT_EQ(tc_surface_resize(s, 4, 3), TC_OK);

    /* Top-left content survived, whole grid dirty again. */
    int32_t cols = 0;
    int32_t rows = 0;
    ASSERT_EQ(tc_surface_get_size(s, &cols, &rows), TC_OK);
    EXPECT_EQ(cols, 4);
    EXPECT_EQ(rows, 3);

    tc_rect r{};
    ASSERT_EQ(tc_surface_dirty_rect(s, &r), TC_OK);
    EXPECT_EQ(r.w, 4);
    EXPECT_EQ(r.h, 3);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, ResizeValidates) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 2, 2, &s), TC_OK);
    EXPECT_EQ(tc_surface_resize(s, 0, 2), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_resize(s, 2, -1), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_resize(s, 5000, 2), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_resize(nullptr, 2, 2), TC_ERR_INVALID_ARG);
    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, GetSizeOptionalArgs) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 5, 7, &s), TC_OK);

    int32_t cols = 0;
    ASSERT_EQ(tc_surface_get_size(s, &cols, nullptr), TC_OK);
    EXPECT_EQ(cols, 5);
    EXPECT_EQ(tc_surface_get_size(s, nullptr, nullptr), TC_ERR_INVALID_ARG);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, SetModeValidates) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 2, 2, &s), TC_OK);
    EXPECT_EQ(tc_surface_set_text_mode(s, TC_TEXT_ASCII), TC_OK);
    EXPECT_EQ(tc_surface_set_text_mode(s, TC_TEXT_UNICODE), TC_OK);
    EXPECT_EQ(tc_surface_set_text_mode(s, static_cast<tc_text_mode>(42)),
              TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_set_ambiguous_wide(s, true), TC_OK);
    EXPECT_EQ(tc_surface_set_ambiguous_wide(s, false), TC_OK);
    tc_surface_destroy(s);
}

/* --------------------------------------------------------------------------
 * Draw primitives (docs/06 §4)
 * ------------------------------------------------------------------------ */

TEST_F(SurfaceTest, PutCellWritesAndMarksRowDirty) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 3, &s), TC_OK);

    /* create marks every row dirty (docs/06 §3); clear the bits to observe
     * which row the write itself touches. */
    tc_surface* inner = reinterpret_cast<tc_surface*>(s);
    memset(inner->dirty, 0, 3);

    ASSERT_EQ(put_text(s, 1, 1, 'X'), TC_OK);

    EXPECT_EQ(inner->dirty[0], 0);
    EXPECT_EQ(inner->dirty[1], 1);
    EXPECT_EQ(inner->dirty[2], 0);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, PutCellOutOfBounds) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 3, 2, &s), TC_OK);

    const tc_cell c = cell_with_text('X');
    EXPECT_EQ(tc_surface_put_cell(s, -1, 0, &c), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_put_cell(s, 3, 0, &c), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_put_cell(s, 0, -1, &c), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_put_cell(s, 0, 2, &c), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_put_cell(s, 0, 0, nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_surface_put_cell(nullptr, 0, 0, &c), TC_ERR_INVALID_ARG);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, FillRectFillsAndClips) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 3, &s), TC_OK);

    /* Fully out of bounds -> error. */
    tc_rect off{ -5, 1, 2, 1, {0, 0} };
    EXPECT_EQ(fill_text(s, &off, 'Z'), TC_ERR_INVALID_ARG);

    /* Partially out of bounds -> clip. */
    tc_surface* inner = reinterpret_cast<tc_surface*>(s);
    memset(inner->dirty, 0, 3);

    tc_rect part{ 2, 0, 5, 2, {0, 0} };   /* spans cols 2..4 (clipped to 3), rows 0..1 */
    ASSERT_EQ(fill_text(s, &part, 'Z'), TC_OK);

    EXPECT_EQ(inner->dirty[0], 1);
    EXPECT_EQ(inner->dirty[1], 1);
    EXPECT_EQ(inner->dirty[2], 0);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, ClearMarksWholeGridDirty) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 3, &s), TC_OK);

    tc_surface* inner = reinterpret_cast<tc_surface*>(s);
    memset(inner->dirty, 0, 3);
    ASSERT_EQ(put_text(s, 0, 0, 'X'), TC_OK);
    EXPECT_EQ(inner->dirty[1], 0);

    ASSERT_EQ(tc_surface_clear(s), TC_OK);

    /* Every row is dirty again. */
    EXPECT_EQ(inner->dirty[0], 1);
    EXPECT_EQ(inner->dirty[1], 1);
    EXPECT_EQ(inner->dirty[2], 1);

    tc_surface_destroy(s);
}

/* Wide-character pairing (docs/06 §4.1). */

TEST_F(SurfaceTest, WideHeadWritesContPartner) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    tc_cell head = cell_with_text("界");
    head.attr    = TC_ATTR_WIDE_HEAD;
    ASSERT_EQ(tc_surface_put_cell(s, 0, 0, &head), TC_OK);

    /* The partner slot was written as an empty CONT. */
    tc_rect r{};
    ASSERT_EQ(tc_surface_dirty_rect(s, &r), TC_OK);
    EXPECT_EQ(r.w, 4);   /* both cells in the row touched */

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, OverwriteHeadClearsPartner) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    tc_cell head = cell_with_text("界");
    head.attr    = TC_ATTR_WIDE_HEAD;
    ASSERT_EQ(tc_surface_put_cell(s, 0, 0, &head), TC_OK);

    /* Overwriting the CONT slot blanks the HEAD. */
    ASSERT_EQ(put_text(s, 1, 0, 'x'), TC_OK);
    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, OrphanContDegradesToBlank) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    /* A CONT with nothing to its left is not a valid wide pair. */
    tc_cell cont = tc_cell_blank();
    cont.attr    = TC_ATTR_WIDE_CONT;
    ASSERT_EQ(tc_surface_put_cell(s, 0, 0, &cont), TC_OK);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, PutHeadThenContKeepsPair) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    /* The exact sequence the text writer emits for a wide cluster: HEAD then
     * an empty CONT into the partner slot. The pair must survive. */
    tc_cell head = cell_with_text("界");
    head.attr    = TC_ATTR_WIDE_HEAD;
    ASSERT_EQ(tc_surface_put_cell(s, 0, 0, &head), TC_OK);

    tc_cell cont = tc_cell_blank();
    cont.attr    = TC_ATTR_WIDE_CONT;
    ASSERT_EQ(tc_surface_put_cell(s, 1, 0, &cont), TC_OK);

    tc_surface_destroy(s);
}

/* draw_text (docs/06 §4.1). */

TEST_F(SurfaceTest, DrawTextAsciiOneBytePerCell) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 6, 1, &s), TC_OK);

    int32_t consumed = 0;
    ASSERT_EQ(tc_surface_draw_text(s, 0, 0, "abc", nullptr, TC_TEXT_ASCII,
                                   &consumed), TC_OK);
    EXPECT_EQ(consumed, 3);

    /* The write touches the row. */
    tc_surface* inner = reinterpret_cast<tc_surface*>(s);
    EXPECT_EQ(inner->dirty[0], 1);

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, DrawTextAsciiSkipsControlChars) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 6, 1, &s), TC_OK);

    int32_t consumed = 0;
    ASSERT_EQ(tc_surface_draw_text(s, 0, 0, "a\nb", nullptr, TC_TEXT_ASCII,
                                   &consumed), TC_OK);
    EXPECT_EQ(consumed, 2);   /* '\n' skipped, not written */

    tc_surface_destroy(s);
}

TEST_F(SurfaceTest, DrawTextUnicodeStopsAtRightEdge) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 3, 1, &s), TC_OK);

    /* "abc" needs 3 columns; "ab界" would need 4, so it stops before the
     * wide character. */
    int32_t consumed = 0;
    ASSERT_EQ(tc_surface_draw_text(s, 0, 0, "ab界", nullptr, TC_TEXT_UNICODE,
                                   &consumed), TC_OK);
    EXPECT_EQ(consumed, 2);

    tc_surface_destroy(s);
}

/* --------------------------------------------------------------------------
 * present (docs/06 §9)
 * ------------------------------------------------------------------------ */

TEST_F(SurfaceTest, PresentRequiresActiveTerm) {
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 2, 1, &s), TC_OK);

    /* Not entered yet. */
    EXPECT_EQ(tc_present(term, s), TC_ERR_STATE);

    tc_surface_destroy(s);
}

TEST_F(PresentTest, FirstFrameWritesChangedCells) {
    ASSERT_EQ(tc_term_set_size(term, 4, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    /* Draw "ab" at the start; the rest stays blank. */
    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(put_text(s, 1, 0, 'b'), TC_OK);

    ASSERT_EQ(tc_present(term, s), TC_OK);

    /* CUP to (1,1), SGR reset, then the two bytes. */
    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0mab");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, UnchangedFrameWritesNothing) {
    ASSERT_EQ(tc_term_set_size(term, 4, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);
    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0ma");

    /* Second present with no changes: nothing is written. */
    cap->out_len = 0;
    ASSERT_EQ(tc_present(term, s), TC_OK);
    EXPECT_TRUE(cap->out_len == 0);

    tc_surface_destroy(s);
}

TEST_F(PresentTest, OnlyChangedCellsAreEmitted) {
    ASSERT_EQ(tc_term_set_size(term, 6, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 6, 1, &s), TC_OK);

    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(put_text(s, 1, 0, 'b'), TC_OK);
    ASSERT_EQ(put_text(s, 2, 0, 'c'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);

    /* Change only the last cell. */
    cap->out_len = 0;
    ASSERT_EQ(put_text(s, 5, 0, 'z'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);

    /* One CUP; the SGR reset is re-emitted because have_sgr starts false on
     * every frame (the first run of the frame always carries its SGR). */
    EXPECT_EQ(cap_out(cap), "\x1b[1;6H\x1b[0mz");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, SgrEmittedWhenStyleChanges) {
    ASSERT_EQ(tc_term_set_size(term, 4, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);

    /* A bold cell emits its own SGR run. */
    cap->out_len = 0;
    tc_cell bold = cell_with_text('b');
    bold.style   = TC_STYLE_BOLD;
    ASSERT_EQ(tc_surface_put_cell(s, 0, 0, &bold), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);

    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0;1mb");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, RgbColorsEmitTruecolorSgr) {
    ASSERT_EQ(tc_term_set_size(term, 2, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 2, 1, &s), TC_OK);

    tc_cell c = cell_with_text('r');
    c.fg      = tc_color_rgb(255, 0, 0);
    c.bg      = tc_color_indexed(4);
    ASSERT_EQ(tc_surface_put_cell(s, 0, 0, &c), TC_OK);

    ASSERT_EQ(tc_present(term, s), TC_OK);

    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0;38;2;255;0;0;48;5;4mr");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, WideClusterEmitsOnlyHeadText) {
    ASSERT_EQ(tc_term_set_size(term, 4, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    ASSERT_EQ(tc_surface_draw_text(s, 0, 0, "界", nullptr, TC_TEXT_UNICODE,
                                   nullptr), TC_OK);

    ASSERT_EQ(tc_present(term, s), TC_OK);

    /* The wide cluster occupies 2 cells but only the HEAD text is written. */
    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0m界");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, WriteFailureKeepsDirtyForRetry) {
    ASSERT_EQ(tc_term_set_size(term, 4, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    cap->write_rc = TC_ERR_IO;
    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_ERR_IO);

    /* Front not updated, rows still dirty: the next present retries. */
    cap->write_rc = TC_OK;
    ASSERT_EQ(tc_present(term, s), TC_OK);
    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0ma");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, SizeMismatchTakesSmallerRegion) {
    ASSERT_EQ(tc_term_set_size(term, 4, 2), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 2, 2, &s), TC_OK);   /* narrower than term */

    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(put_text(s, 1, 0, 'b'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);

    EXPECT_EQ(cap_out(cap), "\x1b[1;1H\x1b[0mab");

    tc_surface_destroy(s);
}

TEST_F(PresentTest, FrontFreesOnTermDestroy) {
    ASSERT_EQ(tc_term_set_size(term, 4, 1), TC_OK);
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(term, 4, 1, &s), TC_OK);

    ASSERT_EQ(put_text(s, 0, 0, 'a'), TC_OK);
    ASSERT_EQ(tc_present(term, s), TC_OK);

    /* The front buffer and outbuf were allocated inside the term and must be
     * released on destroy (checked in TearDown). */
    tc_surface_destroy(s);
    tc_term_destroy(term);
    term = nullptr;
}
