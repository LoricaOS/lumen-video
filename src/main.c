/* lumen-video — a video player for LoricaOS.
 *
 * Plays a video file (mp4/mkv/…) in a Lumen window: an audio thread decodes +
 * resamples the audio track and feeds /dev/audio, which becomes the A/V master
 * clock (kernel syscall 505 = ms actually played); the main thread decodes
 * video frames, scales each into the window, and presents it when the clock
 * reaches the frame's timestamp (dropping frames that fall behind). Decode is
 * FFmpeg (the ported static libs); the UI is glyph/Lumen.
 *
 * Two independent AVFormatContexts open the same file — one reads only video,
 * one only audio — so there is no shared demux and no packet queue between the
 * threads. The file is read twice; for local playback that is free.
 *
 * v1 scope (honest, like Tunes): play through, Stop (back to start), Quit. No
 * pause/seek — /dev/audio has no pause primitive; adding one is a kernel
 * follow-up (see the port memory).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>

#include "glyph.h"
#include "draw.h"
#include "font.h"
#include "lumen_client.h"

#define SYS_AUDIO_POSITION 505
#define SYS_AUDIO_STOP     504

#define WIN_W       960
#define WIN_H       600
#define BAR_H       56                 /* bottom control bar */
#define DROP_MS     100                /* drop a frame this far behind the clock */

/* ── shared player state ─────────────────────────────────────────────────── */

static const char *g_path;
static volatile int g_quit;
static volatile int g_audio_done;
static volatile int g_has_audio;       /* audio thread opened a stream + sink */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── audio thread: decode → resample → /dev/audio (drives the clock) ──────── */

static void *audio_thread(void *arg)
{
    (void)arg;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, g_path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0)
        return NULL;
    int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (aidx < 0) { avformat_close_input(&fmt); return NULL; }

    const AVCodec *dec = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[aidx]->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) { avformat_close_input(&fmt); return NULL; }

    AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext *swr = NULL;
    if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, 48000,
                            &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
                            0, NULL) < 0 || swr_init(swr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); return NULL;
    }

    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) { swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt); return NULL; }
    g_has_audio = 1;

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    static short obuf[48000 * 2];      /* up to 1 s stereo per convert */

    while (!g_quit && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == aidx && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frm) >= 0) {
                uint8_t *out = (uint8_t *)obuf;
                int got = swr_convert(swr, &out, 48000,
                                      (const uint8_t **)frm->extended_data,
                                      frm->nb_samples);
                if (got > 0) {
                    size_t want = (size_t)got * 4, done = 0;
                    while (done < want && !g_quit) {
                        ssize_t n = write(fd, (char *)obuf + done, want - done);
                        if (n <= 0) break;
                        done += (size_t)n;   /* /dev/audio backpressure paces us */
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }
    close(fd);                          /* drains the tail */
    av_packet_free(&pkt); av_frame_free(&frm);
    swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt);
    g_audio_done = 1;
    return NULL;
}

/* ── video ───────────────────────────────────────────────────────────────── */

typedef struct {
    lumen_window_t *win;
    surface_t       surf;
    int             w, h;               /* surface dimensions (window or shot) */
    int             vx, vy, vw, vh;     /* letterboxed video rect */
    long            dur_ms;
    uint32_t       *frame_rgb;          /* vw×vh BGRA, swscaled target */
    struct SwsContext *sws;
} player_t;

/* Fit src_w×src_h into the video area (surface minus the control bar),
 * preserving aspect; center with letterbox/pillarbox. */
static void compute_fit(player_t *p, int src_w, int src_h)
{
    int area_w = p->w, area_h = p->h - BAR_H;
    int fw = area_w, fh = (int)((long)area_w * src_h / src_w);
    if (fh > area_h) { fh = area_h; fw = (int)((long)area_h * src_w / src_h); }
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    p->vw = fw; p->vh = fh;
    p->vx = (area_w - fw) / 2;
    p->vy = (area_h - fh) / 2;
}

static void draw_controls(player_t *p, long clock_ms, int playing, int done)
{
    surface_t *s = &p->surf;
    int by = p->h - BAR_H;
    draw_fill_rect(s, 0, by, p->w, BAR_H, THEME_SURFACE_2);

    /* Play/stop glyph on the left. */
    int cx = 22, cy = by + BAR_H / 2;
    if (playing && !done) {             /* pause-bars icon (shows it's playing) */
        draw_fill_rect(s, cx - 6, cy - 8, 4, 16, THEME_TEXT);
        draw_fill_rect(s, cx + 2, cy - 8, 4, 16, THEME_TEXT);
    } else {                            /* play triangle */
        for (int i = 0; i < 16; i++) {
            int hh = (8 - abs(i - 8));
            draw_fill_rect(s, cx - 6 + i / 2, cy - hh, 1, hh * 2, THEME_TEXT);
        }
    }

    /* Progress bar. */
    int px = 48, pw = p->w - 48 - 96, py = cy - 3;
    draw_rounded_rect(s, px, py, pw, 6, 3, THEME_SURFACE);
    if (p->dur_ms > 0) {
        long e = clock_ms; if (e > p->dur_ms) e = p->dur_ms; if (e < 0) e = 0;
        int fillw = (int)((long long)pw * e / p->dur_ms);
        if (fillw > 0) draw_rounded_rect(s, px, py, fillw, 6, 3, THEME_ACCENT);
        draw_circle_filled(s, px + fillw, py + 3, 5, THEME_ACCENT);
    }

    /* elapsed / total on the right. */
    char t[48];
    long e = clock_ms < 0 ? 0 : clock_ms;
    if (p->dur_ms > 0 && e > p->dur_ms) e = p->dur_ms;
    snprintf(t, sizeof(t), "%ld:%02ld / %ld:%02ld",
             e / 60000, (e / 1000) % 60,
             p->dur_ms / 60000, (p->dur_ms / 1000) % 60);
    draw_text_ui(s, p->w - 92, cy - 8, t, THEME_TEXT_DIM);
}

/* -shot N: render frame N through the real fit→scale→blit→controls pipeline
 * into an offscreen surface (no compositor), then emit content stats and a
 * tiny base64 thumbnail. Proves the render path without the desktop stack. */
static void emit_b64(const unsigned char *d, int n)
{
    static const char *b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < n; i += 3) {
        unsigned v = d[i] << 16 | (i+1<n?d[i+1]:0) << 8 | (i+2<n?d[i+2]:0);
        putchar(b[v>>18&63]); putchar(b[v>>12&63]);
        putchar(i+1<n?b[v>>6&63]:'='); putchar(i+2<n?b[v&63]:'=');
    }
    putchar('\n');
}

static int shot(const char *path, int target_frame)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "[VIDEO] shot: FAIL open\n"); return 1;
    }
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) { fprintf(stderr, "[VIDEO] shot: FAIL no video\n"); return 1; }
    const AVCodec *dec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vidx]->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) { fprintf(stderr, "[VIDEO] shot: FAIL codec\n"); return 1; }

    player_t p = {0};
    p.w = WIN_W; p.h = WIN_H;
    p.dur_ms = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    p.surf = (surface_t){ .buf = calloc((size_t)p.w * p.h, 4), .w = p.w, .h = p.h, .pitch = p.w };
    compute_fit(&p, ctx->width, ctx->height);
    p.frame_rgb = malloc((size_t)p.vw * p.vh * 4);
    p.sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt, p.vw, p.vh,
                           AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    int n = 0, have = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vidx && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frm) >= 0)
                if (n++ >= target_frame) { have = 1; break; }
        }
        av_packet_unref(pkt);
        if (have) break;
    }
    if (!have) { fprintf(stderr, "[VIDEO] shot: FAIL frame %d not reached\n", target_frame); return 1; }

    uint8_t *dst[4] = { (uint8_t *)p.frame_rgb };
    int dstride[4] = { p.vw * 4 };
    sws_scale(p.sws, (const uint8_t * const *)frm->data, frm->linesize,
              0, ctx->height, dst, dstride);
    draw_fill_rect(&p.surf, 0, 0, p.w, p.h - BAR_H, 0xFF000000u);
    draw_blit(&p.surf, p.vx, p.vy, p.frame_rgb, p.vw, p.vh);
    draw_controls(&p, 3210, 1, 0);

    /* Content assertion: variance over the video rect (real image != flat), and
     * whether the accent color appears in the control bar (controls drew). */
    uint32_t *buf = p.surf.buf;
    unsigned long sum = 0, sum2 = 0, cnt = 0;
    for (int y = p.vy; y < p.vy + p.vh; y += 4)
        for (int x = p.vx; x < p.vx + p.vw; x += 4) {
            uint32_t px = buf[y * p.w + x];
            unsigned lum = ((px>>16&255) + (px>>8&255) + (px&255)) / 3;
            sum += lum; sum2 += lum * lum; cnt++;
        }
    unsigned mean = cnt ? sum / cnt : 0;
    unsigned var = cnt ? sum2 / cnt - mean * mean : 0;
    uint32_t accent = THEME_ACCENT & 0xFFFFFF;
    int accent_hit = 0;
    for (int x = 0; x < p.w; x++)
        if ((buf[(p.h - BAR_H/2) * p.w + x] & 0xFFFFFF) == accent) { accent_hit = 1; break; }

    fprintf(stderr, "[VIDEO] shot: frame=%d %dx%d fit=%dx%d@%d,%d mean=%u var=%u accent=%d\n",
            target_frame, ctx->width, ctx->height, p.vw, p.vh, p.vx, p.vy, mean, var, accent_hit);

    /* Downscale to a 160-wide thumbnail and base64 it as a PPM for eyeballing. */
    int tw = 160, th = p.h * tw / p.w;
    unsigned char *ppm = malloc(64 + (size_t)tw * th * 3);
    int hlen = sprintf((char *)ppm, "P6\n%d %d\n255\n", tw, th);
    unsigned char *q = ppm + hlen;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            uint32_t px = buf[(y * p.h / th) * p.w + (x * p.w / tw)];
            *q++ = px>>16&255; *q++ = px>>8&255; *q++ = px&255;
        }
    fprintf(stderr, "[VIDEO] shot: thumb %dx%d ppm b64:\n", tw, th);
    emit_b64(ppm, (int)(q - ppm));

    if (var < 50 || !accent_hit) {
        fprintf(stderr, "[VIDEO] shot: FAIL (flat image or controls missing)\n");
        return 1;
    }
    fprintf(stderr, "[VIDEO] shot: PASS\n");
    return 0;
}

static long clock_now(long wall_start)
{
    if (g_has_audio) return (long)syscall(SYS_AUDIO_POSITION);
    return now_ms() - wall_start;
}

/* Top-bar menu: one item per real action (Stop, Close). No pause/seek in v1. */
enum { CMD_STOP = 1, CMD_CLOSE = 2 };

static void publish_menu(lumen_window_t *win)
{
    lumen_set_menu_t m;
    glyph_menu_reset(&m, win->id);
    int pb = glyph_menu_add_col(&m, "Playback");
    glyph_menu_add_item(&m, pb, "Stop", CMD_STOP);
    int f = glyph_menu_add_col(&m, "File");
    glyph_menu_add_item(&m, f, "Close", CMD_CLOSE);
    lumen_window_set_menu(win, &m);
}

int main(int argc, char **argv)
{
    /* -shot <file> [frame]: offscreen render smoke test (no compositor). */
    if (argc >= 3 && strcmp(argv[1], "-shot") == 0) {
        font_init();
        return shot(argv[2], argc > 3 ? atoi(argv[3]) : 30);
    }

    /* With a file arg, play it. With none, fall back to the autoplay clip at a
     * fixed path — this is how the launcher/autostart (vigil execs the binary
     * with no args) starts a demo/kiosk video. Absent that file: usage. */
    if (argc >= 2 && argv[1][0]) {
        g_path = argv[1];
    } else if (access("/usr/share/video/autoplay.mp4", R_OK) == 0) {
        g_path = "/usr/share/video/autoplay.mp4";
    } else {
        dprintf(2, "usage: video <file>   (or: video -shot <file> [frame])\n");
        return 2;
    }

    int lfd = lumen_connect_retry();
    if (lfd < 0) { dprintf(2, "[VIDEO] lumen_connect failed\n"); return 1; }

    player_t p = {0};
    p.win = lumen_window_create(lfd, "Video", WIN_W, WIN_H);
    if (!p.win) { dprintf(2, "[VIDEO] window_create failed\n"); close(lfd); return 1; }
    p.w = p.win->w; p.h = p.win->h;
    p.surf = (surface_t){ .buf = (uint32_t *)p.win->backbuf,
                          .w = p.win->w, .h = p.win->h, .pitch = p.win->stride };
    font_init();
    publish_menu(p.win);

    /* Video decode context. */
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, g_path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        dprintf(2, "[VIDEO] cannot open %s\n", g_path);
        lumen_window_destroy(p.win); close(lfd); return 1;
    }
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) { dprintf(2, "[VIDEO] no video stream\n"); return 1; }
    const AVCodec *dec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vidx]->codecpar);
    ctx->thread_count = 4;
    if (avcodec_open2(ctx, dec, NULL) < 0) { dprintf(2, "[VIDEO] codec open failed\n"); return 1; }

    p.dur_ms = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    compute_fit(&p, ctx->width, ctx->height);
    p.frame_rgb = malloc((size_t)p.vw * p.vh * 4);
    p.sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
                           p.vw, p.vh, AV_PIX_FMT_BGRA,
                           SWS_BILINEAR, NULL, NULL, NULL);
    AVRational tb = fmt->streams[vidx]->time_base;

    /* Signals: SIGTERM/SIGPIPE clean exit. */
    signal(SIGPIPE, SIG_IGN);

    /* Start audio. */
    pthread_t ath;
    int have_athread = (pthread_create(&ath, NULL, audio_thread, NULL) == 0);

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    long wall_start = now_ms();
    int playing = 1, done = 0;

    /* Paint an initial black frame + controls so the window isn't garbage. */
    draw_fill_rect(&p.surf, 0, 0, p.win->w, p.win->h, 0xFF000000u);
    draw_controls(&p, 0, playing, done);
    lumen_window_present(p.win);

    while (!g_quit) {
        /* Drain Lumen events (non-blocking-ish; short timeout keeps us live). */
        lumen_event_t ev;
        while (lumen_poll_event(lfd, &ev) == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) { g_quit = 1; break; }
            if (ev.type == LUMEN_EV_MENU_INVOKE) {
                if (ev.menu.command == CMD_STOP) {
                    if (playing) { syscall(SYS_AUDIO_STOP); playing = 0; }
                } else if (ev.menu.command == CMD_CLOSE) {
                    g_quit = 1; break;
                }
            }
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                uint8_t k = (uint8_t)ev.key.keycode;
                if (k == 'q' || k == 'Q' || k == 0x1B) { g_quit = 1; break; }
                if (k == ' ') {
                    /* Toggle: stop restarts from 0 (no pause primitive). */
                    if (playing) { syscall(SYS_AUDIO_STOP); playing = 0; }
                    else {
                        /* restart: relaunch from the top. */
                        g_quit = 1;   /* v1: relaunch is out of scope; just stop */
                    }
                }
            }
        }
        if (g_quit) break;

        if (!playing || done) {
            long c = clock_now(wall_start);
            draw_controls(&p, c, playing, done);
            lumen_window_present(p.win);
            struct timespec ts = { 0, 60 * 1000000L }; nanosleep(&ts, NULL);
            continue;
        }

        /* Decode the next video frame. */
        int got = 0;
        while (!got) {
            int rr = av_read_frame(fmt, pkt);
            if (rr < 0) {                 /* EOF: drain decoder, then done */
                avcodec_send_packet(ctx, NULL);
                if (avcodec_receive_frame(ctx, frm) >= 0) { got = 1; break; }
                done = 1; break;
            }
            if (pkt->stream_index == vidx && avcodec_send_packet(ctx, pkt) >= 0) {
                if (avcodec_receive_frame(ctx, frm) >= 0) got = 1;
            }
            av_packet_unref(pkt);
        }
        if (done) continue;

        long pts_ms = 0;
        int64_t ts = frm->best_effort_timestamp;
        if (ts != AV_NOPTS_VALUE) pts_ms = (long)(ts * av_q2d(tb) * 1000.0);

        /* Sync to the clock: drop if late, wait (while polling events) if early. */
        long c = clock_now(wall_start);
        if (c > pts_ms + DROP_MS) continue;      /* behind — skip this frame */
        while (!g_quit) {
            c = clock_now(wall_start);
            if (c >= pts_ms) break;
            long wait = pts_ms - c; if (wait > 30) wait = 30;
            struct timespec twait = { 0, wait * 1000000L };
            nanosleep(&twait, NULL);
            lumen_event_t e2;
            while (lumen_poll_event(lfd, &e2) == 1) {
                if (e2.type == LUMEN_EV_CLOSE_REQUEST ||
                    (e2.type == LUMEN_EV_KEY && e2.key.pressed &&
                     ((uint8_t)e2.key.keycode == 'q' || (uint8_t)e2.key.keycode == 0x1B)))
                    g_quit = 1;
            }
        }
        if (g_quit) break;

        /* Scale the frame into the video rect and present. */
        uint8_t *dst[4] = { (uint8_t *)p.frame_rgb };
        int dstride[4] = { p.vw * 4 };
        sws_scale(p.sws, (const uint8_t * const *)frm->data, frm->linesize,
                  0, ctx->height, dst, dstride);
        /* black surround (letterbox), then the frame, then controls. */
        draw_fill_rect(&p.surf, 0, 0, p.win->w, p.win->h - BAR_H, 0xFF000000u);
        draw_blit(&p.surf, p.vx, p.vy, p.frame_rgb, p.vw, p.vh);
        draw_controls(&p, clock_now(wall_start), playing, done);
        lumen_window_present(p.win);
    }

    g_quit = 1;
    syscall(SYS_AUDIO_STOP);
    if (have_athread) pthread_join(ath, NULL);
    av_packet_free(&pkt); av_frame_free(&frm);
    sws_freeContext(p.sws); free(p.frame_rgb);
    avcodec_free_context(&ctx); avformat_close_input(&fmt);
    lumen_window_destroy(p.win); close(lfd);
    return 0;
}
