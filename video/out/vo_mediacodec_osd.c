/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <string.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "common/common.h"
#include "common/msg.h"
#include "misc/jni.h"
#include "mpv_talloc.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "sub/draw_bmp.h"
#include "sub/osd.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "vo.h"

// Like vo_mediacodec_embed, MediaCodec decodes straight into the application's
// video Surface (--wid) and we only tell it when to show a buffer. On top of
// that, this VO draws the OSD with the CPU into a *second*, separate Surface
// the application stacks above the video. vo_dmabuf_wayland does the same
// thing on Wayland: the video goes its own way and the OSD is a CPU-drawn
// overlay on a surface of its own.
//
// The VO never learns (or cares) whether that second Surface is backed by a
// SurfaceView or by a texture; it only ever sees one jobject.

struct osd_opts {
    int64_t surface;
    struct m_geometry video_rect;
};

#define OPT_BASE_STRUCT struct osd_opts

static const struct m_sub_options vo_mediacodec_osd_conf = {
    .opts = (const struct m_option[]) {
        {"vo-mediacodec-osd-surface", OPT_INT64(surface)},
        {"vo-mediacodec-osd-video-rect", OPT_RECT(video_rect)},
        // Became the generic --sub-keepout (sub/osd.c); the name stays for
        // applications written against the VO-specific option.
        {"vo-mediacodec-osd-sub-keepout", OPT_ALIAS("sub-keepout")},
        {0},
    },
    .size = sizeof(struct osd_opts),
};

struct priv {
    struct osd_opts *opts;
    struct m_config_cache *opts_cache;

    // Video path. Identical to vo_mediacodec_embed.
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;

    // OSD path.
    ANativeWindow *osd_win;
    int64_t cur_surface;        // handle the current osd_win was made from
    int win_w, win_h;           // OSD buffer size, in buffer pixels
    struct mp_osd_res osd_res;
    struct mp_draw_sub_cache *osd_cache;
    double last_pts;

    bool locked;                // a lock is outstanding (draw_frame->flip_page)
    bool force_full;            // next update must repaint the whole buffer
    bool lock_failed;           // already complained about a failing lock
    bool bad_rect;              // already complained about the video rect
};

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_ERR(vo, "No video surface given; use --wid=(intptr_t)Surface.\n");
        av_buffer_unref(&device_ref);
        return NULL;
    }
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

// Drop the OSD surface. Always safe, even if nothing was ever attached.
static void osd_detach(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (p->osd_win) {
        // Never leave the window locked: nothing else can dequeue a buffer
        // while we hold one, and the app cannot recover from that.
        if (p->locked)
            ANativeWindow_unlockAndPost(p->osd_win);
        ANativeWindow_release(p->osd_win);
    }
    // The android.view.Surface global ref belongs to the application. We did
    // not create it, so we must not delete it.
    p->osd_win = NULL;
    p->locked = false;
    p->win_w = p->win_h = 0;
    p->osd_res = (struct mp_osd_res){0};
    TA_FREEP(&p->osd_cache);
}

// Pick up the buffer size the app configured with Surface.setFixedSize(), and
// ask for buffers of exactly that size in a format we can fill with the CPU.
// The app may deliberately use a buffer smaller than the screen; the
// compositor scales it up for us.
static void update_osd_geometry(struct vo *vo)
{
    struct priv *p = vo->priv;

    // Clearing our own requested size makes ANativeWindow_getWidth/Height
    // report the producer's default size again, which is what the app set.
    ANativeWindow_setBuffersGeometry(p->osd_win, 0, 0, WINDOW_FORMAT_RGBA_8888);

    int w = ANativeWindow_getWidth(p->osd_win);
    int h = ANativeWindow_getHeight(p->osd_win);
    if (w <= 0 || h <= 0) {
        MP_ERR(vo, "OSD surface reports an invalid size (%dx%d), disabling "
                   "the OSD.\n", w, h);
        osd_detach(vo);
        return;
    }

    if (ANativeWindow_setBuffersGeometry(p->osd_win, w, h,
                                         WINDOW_FORMAT_RGBA_8888) < 0)
    {
        MP_ERR(vo, "Could not configure the OSD surface for CPU rendering, "
                   "disabling the OSD.\n");
        osd_detach(vo);
        return;
    }

    if (w != p->win_w || h != p->win_h) {
        MP_VERBOSE(vo, "OSD surface buffer size: %dx%d\n", w, h);
        p->win_w = w;
        p->win_h = h;
        p->force_full = true;
    }
}

static void osd_attach(struct vo *vo)
{
    struct priv *p = vo->priv;

    osd_detach(vo);

    if (!p->cur_surface) {
        MP_VERBOSE(vo, "No OSD surface, behaving like mediacodec_embed.\n");
        return;
    }

    JNIEnv *env = MP_JNI_GET_ENV(vo);
    if (!env) {
        MP_ERR(vo, "Could not attach to the Java VM, disabling the OSD.\n");
        return;
    }

    p->osd_win = ANativeWindow_fromSurface(env, (jobject)(intptr_t)p->cur_surface);
    if (!p->osd_win) {
        MP_ERR(vo, "Could not get an ANativeWindow for the OSD surface, "
                   "disabling the OSD.\n");
        return;
    }

    p->force_full = true;
    p->lock_failed = false;
    p->bad_rect = false;
    update_osd_geometry(vo);
}

// Where the video picture sits inside the OSD buffer. Everything outside of it
// is letterbox area; the OSD margins must match it so that PGS bitmaps and
// subtitles placed in the black bars land in the right place.
static void update_osd_res(struct vo *vo)
{
    struct priv *p = vo->priv;

    struct mp_rect vid = {0, 0, p->win_w, p->win_h};
    struct m_geometry *gm = &p->opts->video_rect;
    if (gm->xy_valid || gm->wh_valid) {
        m_rect_apply(&vid, p->win_w, p->win_h, gm);

        vid.x0 = MPCLAMP(vid.x0, 0, p->win_w);
        vid.y0 = MPCLAMP(vid.y0, 0, p->win_h);
        vid.x1 = MPCLAMP(vid.x1, vid.x0, p->win_w);
        vid.y1 = MPCLAMP(vid.y1, vid.y0, p->win_h);

        if (mp_rect_w(vid) < 1 || mp_rect_h(vid) < 1) {
            if (!p->bad_rect) {
                p->bad_rect = true;
                MP_WARN(vo, "Empty video rect, using the whole OSD surface.\n");
            }
            vid = (struct mp_rect){0, 0, p->win_w, p->win_h};
        } else {
            p->bad_rect = false;
        }
    }

    struct mp_osd_res res = {
        .w = p->win_w,
        .h = p->win_h,
        .ml = vid.x0,
        .mt = vid.y0,
        .mr = p->win_w - vid.x1,
        .mb = p->win_h - vid.y1,
        .display_par = vo->monitor_par > 0 ? vo->monitor_par : 1.0,
    };

    if (osd_res_equals(res, p->osd_res))
        return;

    p->osd_res = res;
    // The overlay cache tracks changes against the old geometry, so its idea
    // of what is already on screen is worthless now. Start over and repaint
    // everything, or the old subtitle position would stay on the surface.
    TA_FREEP(&p->osd_cache);
    p->force_full = true;
}

// mp_draw_sub_overlay() hands out IMGFMT_BGRA with premultiplied alpha, i.e.
// the bytes in memory are B,G,R,A. WINDOW_FORMAT_RGBA_8888 is R,G,B,A and
// SurfaceFlinger treats it as premultiplied as well, so this is a plain
// per-pixel R/B swap - no un-premultiplication anywhere.
//
// rc must be inside the buffer; it may reach outside the overlay, in which
// case the remainder is cleared to transparent.
static void copy_overlay(ANativeWindow_Buffer *buf, struct mp_rect *rc,
                         struct mp_image *ov)
{
    // buf->stride is in pixels, and is regularly larger than buf->width.
    size_t dst_stride = (size_t)buf->stride * 4;
    int rc_w = mp_rect_w(*rc);

    for (int y = rc->y0; y < rc->y1; y++) {
        uint8_t *dst = (uint8_t *)buf->bits + (size_t)y * dst_stride +
                       (size_t)rc->x0 * 4;
        const uint8_t *src = NULL;
        int copy_w = 0;

        if (y < ov->h && rc->x0 < ov->w) {
            copy_w = MPMIN(rc_w, ov->w - rc->x0);
            src = (const uint8_t *)ov->planes[0] +
                  (ptrdiff_t)y * ov->stride[0] + (size_t)rc->x0 * 4;
        }

        for (int x = 0; x < copy_w; x++) {
            // memcpy-style access: the rows are not guaranteed to be aligned,
            // and this must not depend on type punning. Android is always
            // little endian, so the loaded word is A<<24|R<<16|G<<8|B and the
            // swap of the B and R bytes is a swap of those two byte lanes.
            uint32_t v;
            memcpy(&v, src + (size_t)x * 4, 4);
            v = (v & 0xFF00FF00u) | ((v >> 16) & 0x000000FFu) |
                ((v & 0x000000FFu) << 16);
            memcpy(dst + (size_t)x * 4, &v, 4);
        }

        // The overlay does not reach here, but the system still considers
        // these pixels dirty, so they have to be written: transparent.
        if (rc_w > copy_w)
            memset(dst + (size_t)copy_w * 4, 0, (size_t)(rc_w - copy_w) * 4);
    }
}

static void draw_osd(struct vo *vo, double pts)
{
    struct priv *p = vo->priv;

    if (!p->osd_win)
        return;

    update_osd_res(vo);

    struct sub_bitmap_list *sbs =
        osd_render(vo->osd, p->osd_res, pts, 0, mp_draw_sub_formats);
    if (!sbs)
        return;

    // --sub-keepout has already been applied by osd_render(); a change of the
    // lift comes with a new change_id, which marks both the old and the new
    // place of the subtitles as modified.
    if (!p->osd_cache)
        p->osd_cache = mp_draw_sub_alloc(p, vo->global);

    struct mp_rect act_rc[1], mod_rc[64];
    int num_act_rc = 0, num_mod_rc = 0;
    struct mp_image *overlay =
        mp_draw_sub_overlay(p->osd_cache, sbs, act_rc, MP_ARRAY_SIZE(act_rc),
                            &num_act_rc, mod_rc, MP_ARRAY_SIZE(mod_rc),
                            &num_mod_rc);
    talloc_free(sbs);

    if (!overlay) {
        MP_WARN(vo, "Could not render the OSD overlay.\n");
        p->force_full = true;
        return;
    }

    // Nothing changed since the last call, so the surface already shows the
    // right thing. A subtitle standing still must not cost a buffer per video
    // frame. Going from "has content" to "empty" is reported as a modified
    // region too, so that transition posts exactly one transparent update and
    // then falls back into this branch.
    if (!num_mod_rc && !p->force_full)
        return;

    struct mp_rect full = {0, 0, p->win_w, p->win_h};
    struct mp_rect dirty = full;
    if (!p->force_full) {
        dirty = mod_rc[0];
        for (int n = 1; n < num_mod_rc; n++)
            mp_rect_union(&dirty, &mod_rc[n]);
        if (!mp_rect_intersection(&dirty, &full))
            return;
    }

    // NEVER lock the video surface (--wid): the first ANativeWindow_lock()
    // connects a Surface as a CPU producer for good, and MediaCodec (or EGL)
    // can never attach to it again. Only the OSD surface is ever locked.
    ARect bounds = {
        .left = dirty.x0, .top = dirty.y0,
        .right = dirty.x1, .bottom = dirty.y1,
    };
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(p->osd_win, &buf, &bounds) < 0) {
        if (!p->lock_failed) {
            p->lock_failed = true;
            MP_ERR(vo, "Could not lock the OSD surface, dropping the OSD "
                       "update.\n");
        }
        // Whatever is on screen no longer matches our cache.
        p->force_full = true;
        return;
    }
    p->locked = true;
    p->lock_failed = false;
    p->force_full = false;

    // Should not happen, but writing into this would corrupt memory. Leave
    // p->locked set: flip_page() has to give the buffer back either way.
    if (!buf.bits || buf.width <= 0 || buf.height <= 0 || buf.stride < buf.width) {
        MP_ERR(vo, "OSD surface handed out an unusable buffer.\n");
        p->force_full = true;
        return;
    }

    // ANativeWindow_lock() may have enlarged the dirty region - it does so
    // whenever it cannot copy the previous front buffer back, e.g. on the
    // first frame, after a size change or when the buffers rotate. Every
    // pixel it reports must be (re)written, and the overlay holds the full
    // current OSD, so it can serve any region; where it has nothing,
    // copy_overlay() writes transparent pixels.
    struct mp_rect rc = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    struct mp_rect buf_rc = {0, 0, buf.width, buf.height};
    if (mp_rect_intersection(&rc, &buf_rc))
        copy_overlay(&buf, &rc, overlay);

    if (buf.width != p->win_w || buf.height != p->win_h) {
        MP_WARN(vo, "OSD surface gave a %dx%d buffer, expected %dx%d; "
                    "adapting.\n", buf.width, buf.height, p->win_w, p->win_h);
        p->win_w = buf.width;
        p->win_h = buf.height;
        p->force_full = true;
    }
}

static bool update_opts(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (!m_config_cache_update(p->opts_cache))
        return false;

    if (p->opts->surface != p->cur_surface) {
        p->cur_surface = p->opts->surface;
        osd_attach(vo); // also re-reads the geometry
        return true;
    }

    // The app updates the video rect when its layout changes, which is also
    // the moment the OSD surface itself may have been resized. The new
    // mp_osd_res is picked up by update_osd_res() during the next draw.
    if (p->osd_win)
        update_osd_geometry(vo);
    return true;
}

// Called from whichever thread sets the options; may only wake us up.
static void opts_wakeup(void *ctx)
{
    vo_wakeup(ctx);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;

    p->opts_cache = m_config_cache_alloc(vo, vo->global, &vo_mediacodec_osd_conf);
    p->opts = p->opts_cache->opts;
    p->last_pts = MP_NOPTS_VALUE;

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_osd",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        return -1; // uninit() is not called after a failed preinit()
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);

    // Only from here on is uninit() guaranteed to run and unregister this.
    m_config_cache_set_wakeup_cb(p->opts_cache, opts_wakeup, vo);

    // A broken OSD surface must never keep the video from playing.
    p->cur_surface = p->opts->surface;
    osd_attach(vo);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;

    // Post the OSD first and release the video buffer right after, so that
    // both reach the compositor for the same vsync.
    if (p->osd_win && p->locked) {
        p->locked = false;
        if (ANativeWindow_unlockAndPost(p->osd_win) < 0) {
            MP_WARN(vo, "Could not post the OSD surface.\n");
            p->force_full = true;
        }
    }

    if (!p->next_image)
        return;

    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
    av_mediacodec_release_buffer(buffer, 1);
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    update_opts(vo);

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;

    // On an OSD-only redraw (VO_CAP_OSD_ONLY_REDRAW) there is no frame to take
    // a timestamp from, because VO_CAP_NORETAIN means none was kept.
    if (frame->current)
        p->last_pts = frame->current->pts;

    // Rendering here, rather than in flip_page(), is what gets to use the
    // headroom the core gives us by waking the VO up early.
    draw_osd(vo, p->last_pts);
    // The video is always "rendered": MediaCodec shows it in flip_page().
    return true;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    if (request == VOCTRL_CHECK_EVENTS) {
        // Runs on the VO thread once per iteration, and opts_wakeup() makes
        // sure there is an iteration soon after the app changes an option.
        // Asking for a redraw is what gets a new video rect onto the screen
        // while playback is paused.
        if (update_opts(vo))
            vo->want_redraw = true;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    osd_detach(vo);
    mp_image_unrefp(&p->next_image);

    // Drop the cache (and with it the wakeup callback) here, while the VO
    // still exists: it is what opts_wakeup() is handed.
    p->opts = NULL;
    TA_FREEP(&p->opts_cache);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
}

const struct vo_driver video_out_mediacodec_osd = {
    .description = "Android (Embedded MediaCodec Surface with CPU OSD)",
    .name = "mediacodec_osd",
    .caps = VO_CAP_NORETAIN | VO_CAP_OSD_ONLY_REDRAW,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .global_opts = &vo_mediacodec_osd_conf,
};
