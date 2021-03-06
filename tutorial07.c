// tutorial07.c
// A pedagogical video player that really works! Now with seeking features.
//
// This tutorial was written by Stephen Dranger (dranger@gmail.com).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
//
// Use the Makefile to build all the samples.
//
// Run using
// tutorial07 myvideofile.mpg
//
// to play the video.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libswresample/swresample.h>

#include <SDL.h>
#include <SDL_thread.h>
#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif
#include <stdio.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER
#define MAX_AUDIO_FRAME_SIZE 192000
#define SUBPICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;
typedef struct VideoPicture {
	SDL_Overlay *bmp;
	int width, height; /* source height & width */
	int allocated;
	double pts;
} VideoPicture;

typedef struct AudioParams{
	int freq;
	int channels;
	int64_t channel_layout;
	enum AVSampleFormat fmt;
}AudioParams;

typedef struct SubPicture {
    double pts; /* presentation time stamp for this picture */
    AVSubtitle sub;
} SubPicture;


typedef struct VideoState {
	AVFormatContext *pFormatCtx;
	int             videoStream, audioStream, subtitleStream;

	int             av_sync_type;
	double          external_clock; /* external clock base */
	int64_t         external_clock_time;
	int             seek_req;
	int             seek_flags;
	int64_t         seek_pos;

	double          audio_clock;
	AVStream        *audio_st;
	PacketQueue     audioq;
	AVFrame         audio_frame;
	//uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE* 3) / 2];
	uint8_t         *audio_buf;
	uint8_t         *audio_buf1;
	unsigned int    audio_buf_size;
	unsigned int    audio_buf1_size;
	unsigned int    audio_buf_index;
	AVPacket        audio_pkt;
	uint8_t         *audio_pkt_data;
	int             audio_pkt_size;
	int             audio_hw_buf_size;  
	double          audio_diff_cum; /* used for AV difference average computation */
	double          audio_diff_avg_coef;
	double          audio_diff_threshold;
	int             audio_diff_avg_count;
	AudioParams     audio_src;
	AudioParams     audio_tgt;
	struct SwrContext *swr_ctx;
	double          frame_timer;
	double          frame_last_pts;
	double          frame_last_delay;
	double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
	double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
	int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts
	AVStream        *video_st;
	PacketQueue     videoq;
	VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int             pictq_size, pictq_rindex, pictq_windex;
	//subtitle
	PacketQueue     subtitleq;
	AVStream        *subtitle_st;
    SubPicture subpq[SUBPICTURE_QUEUE_SIZE];
	int subpq_size, subpq_rindex, subpq_windex;
	SDL_mutex *subpq_mutex;
	SDL_cond *subpq_cond;

	SDL_mutex       *pictq_mutex;
	SDL_cond        *pictq_cond;
	SDL_Thread      *parse_tid;
	SDL_Thread      *video_tid;
	SDL_Thread      *subtitle_tid;

	char            filename[1024];
	int             quit;

	AVIOContext     *io_context;
	struct SwsContext *sws_ctx;
} VideoState;

enum {
	AV_SYNC_AUDIO_MASTER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_MASTER,
};

SDL_Surface     *screen;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;
AVPacket flush_pkt;

#define ALPHA_BLEND(a, oldp, newp, s)\
((((oldp << s) * (255 - (a))) + (newp * (a))) / (255 << s))

#define RGBA_IN(r, g, b, a, s)\
{\
    unsigned int v = ((const uint32_t *)(s))[0];\
    a = (v >> 24) & 0xff;\
    r = (v >> 16) & 0xff;\
    g = (v >> 8) & 0xff;\
    b = v & 0xff;\
}

#define YUVA_IN(y, u, v, a, s, pal)\
{\
    unsigned int val = ((const uint32_t *)(pal))[*(const uint8_t*)(s)];\
    a = (val >> 24) & 0xff;\
    y = (val >> 16) & 0xff;\
    u = (val >> 8) & 0xff;\
    v = val & 0xff;\
}

#define YUVA_OUT(d, y, u, v, a)\
{\
    ((uint32_t *)(d))[0] = (a << 24) | (y << 16) | (u << 8) | v;\
}

#define BPP 1

static void blend_subrect(AVPicture *dst, const AVSubtitleRect *rect, int imgw, int imgh)
{
    int wrap, wrap3, width2, skip2;
    int y, u, v, a, u1, v1, a1, w, h;
    uint8_t *lum, *cb, *cr;
    const uint8_t *p;
    const uint32_t *pal;
    int dstx, dsty, dstw, dsth;

    dstw = av_clip(rect->w, 0, imgw);
    dsth = av_clip(rect->h, 0, imgh);
    dstx = av_clip(rect->x, 0, imgw - dstw);
    dsty = av_clip(rect->y, 0, imgh - dsth);
    lum = dst->data[0] + dsty * dst->linesize[0];
    cb  = dst->data[1] + (dsty >> 1) * dst->linesize[1];
    cr  = dst->data[2] + (dsty >> 1) * dst->linesize[2];

    width2 = ((dstw + 1) >> 1) + (dstx & ~dstw & 1);
    skip2 = dstx >> 1;
    wrap = dst->linesize[0];
    wrap3 = rect->pict.linesize[0];
    p = rect->pict.data[0];
    pal = (const uint32_t *)rect->pict.data[1];  /* Now in YCrCb! */

    if (dsty & 1) {
        lum += dstx;
        cb += skip2;
        cr += skip2;

        if (dstx & 1) {
            YUVA_IN(y, u, v, a, p, pal);
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            cb[0] = ALPHA_BLEND(a >> 2, cb[0], u, 0);
            cr[0] = ALPHA_BLEND(a >> 2, cr[0], v, 0);
            cb++;
            cr++;
            lum++;
            p += BPP;
        }
        for (w = dstw - (dstx & 1); w >= 2; w -= 2) {
            YUVA_IN(y, u, v, a, p, pal);
            u1 = u;
            v1 = v;
            a1 = a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);

            YUVA_IN(y, u, v, a, p + BPP, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[1] = ALPHA_BLEND(a, lum[1], y, 0);
            cb[0] = ALPHA_BLEND(a1 >> 2, cb[0], u1, 1);
            cr[0] = ALPHA_BLEND(a1 >> 2, cr[0], v1, 1);
            cb++;
            cr++;
            p += 2 * BPP;
            lum += 2;
        }
        if (w) {
            YUVA_IN(y, u, v, a, p, pal);
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            cb[0] = ALPHA_BLEND(a >> 2, cb[0], u, 0);
            cr[0] = ALPHA_BLEND(a >> 2, cr[0], v, 0);
            p++;
            lum++;
        }
        p += wrap3 - dstw * BPP;
        lum += wrap - dstw - dstx;
        cb += dst->linesize[1] - width2 - skip2;
        cr += dst->linesize[2] - width2 - skip2;
    }
    for (h = dsth - (dsty & 1); h >= 2; h -= 2) {
        lum += dstx;
        cb += skip2;
        cr += skip2;

        if (dstx & 1) {
            YUVA_IN(y, u, v, a, p, pal);
            u1 = u;
            v1 = v;
            a1 = a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            p += wrap3;
            lum += wrap;
            YUVA_IN(y, u, v, a, p, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            cb[0] = ALPHA_BLEND(a1 >> 2, cb[0], u1, 1);
            cr[0] = ALPHA_BLEND(a1 >> 2, cr[0], v1, 1);
            cb++;
            cr++;
            p += -wrap3 + BPP;
            lum += -wrap + 1;
        }
        for (w = dstw - (dstx & 1); w >= 2; w -= 2) {
            YUVA_IN(y, u, v, a, p, pal);
            u1 = u;
            v1 = v;
            a1 = a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);

            YUVA_IN(y, u, v, a, p + BPP, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[1] = ALPHA_BLEND(a, lum[1], y, 0);
            p += wrap3;
            lum += wrap;

            YUVA_IN(y, u, v, a, p, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);

            YUVA_IN(y, u, v, a, p + BPP, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[1] = ALPHA_BLEND(a, lum[1], y, 0);

            cb[0] = ALPHA_BLEND(a1 >> 2, cb[0], u1, 2);
            cr[0] = ALPHA_BLEND(a1 >> 2, cr[0], v1, 2);

            cb++;
            cr++;
            p += -wrap3 + 2 * BPP;
            lum += -wrap + 2;
        }
        if (w) {
            YUVA_IN(y, u, v, a, p, pal);
            u1 = u;
            v1 = v;
            a1 = a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            p += wrap3;
            lum += wrap;
            YUVA_IN(y, u, v, a, p, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            cb[0] = ALPHA_BLEND(a1 >> 2, cb[0], u1, 1);
            cr[0] = ALPHA_BLEND(a1 >> 2, cr[0], v1, 1);
            cb++;
            cr++;
            p += -wrap3 + BPP;
            lum += -wrap + 1;
        }
        p += wrap3 + (wrap3 - dstw * BPP);
        lum += wrap + (wrap - dstw - dstx);
        cb += dst->linesize[1] - width2 - skip2;
        cr += dst->linesize[2] - width2 - skip2;
    }
    /* handle odd height */
    if (h) {
        lum += dstx;
        cb += skip2;
        cr += skip2;

        if (dstx & 1) {
            YUVA_IN(y, u, v, a, p, pal);
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            cb[0] = ALPHA_BLEND(a >> 2, cb[0], u, 0);
            cr[0] = ALPHA_BLEND(a >> 2, cr[0], v, 0);
            cb++;
            cr++;
            lum++;
            p += BPP;
        }
        for (w = dstw - (dstx & 1); w >= 2; w -= 2) {
            YUVA_IN(y, u, v, a, p, pal);
            u1 = u;
            v1 = v;
            a1 = a;
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);

            YUVA_IN(y, u, v, a, p + BPP, pal);
            u1 += u;
            v1 += v;
            a1 += a;
            lum[1] = ALPHA_BLEND(a, lum[1], y, 0);
            cb[0] = ALPHA_BLEND(a1 >> 2, cb[0], u, 1);
            cr[0] = ALPHA_BLEND(a1 >> 2, cr[0], v, 1);
            cb++;
            cr++;
            p += 2 * BPP;
            lum += 2;
        }
        if (w) {
            YUVA_IN(y, u, v, a, p, pal);
            lum[0] = ALPHA_BLEND(a, lum[0], y, 0);
            cb[0] = ALPHA_BLEND(a >> 2, cb[0], u, 0);
            cr[0] = ALPHA_BLEND(a >> 2, cr[0], v, 0);
        }
    }
}

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

	AVPacketList *pkt1;
	if(pkt != &flush_pkt && av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for(;;) {

		if(global_video_state->quit) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		} else if (!block) {
			ret = 0;
			break;
		} else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}
static void packet_queue_flush(PacketQueue *q) {
	AVPacketList *pkt, *pkt1;

	SDL_LockMutex(q->mutex);
	for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
		pkt1 = pkt->next;
		av_free_packet(&pkt->pkt);
		av_freep(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
	packet_queue_flush(q);
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);
}

double get_audio_clock(VideoState *is) {
	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = is->audio_clock; /* maintained in the audio thread */
	hw_buf_size = is->audio_buf_size - is->audio_buf_index;
	bytes_per_sec = 0;
	n = is->audio_st->codec->channels * 2;
	if(is->audio_st) {
		bytes_per_sec = is->audio_st->codec->sample_rate * n;
	}
	if(bytes_per_sec) {
		pts -= (double)hw_buf_size / bytes_per_sec;
	}
	return pts;
}
double get_video_clock(VideoState *is) {
	double delta;

	delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
	return is->video_current_pts + delta;
}
double get_external_clock(VideoState *is) {
	return av_gettime() / 1000000.0;
}
double get_master_clock(VideoState *is) {
	if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		return get_video_clock(is);
	} else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		return get_audio_clock(is);
	} else {
		return get_external_clock(is);
	}
}
/* Add or subtract samples to get a better sync, return new
   audio buffer size */
int synchronize_audio(VideoState *is, short *samples,
		int samples_size, double pts) {
	int n;
	double ref_clock;

	n = 2 * is->audio_st->codec->channels;

	if(is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int wanted_size, min_size, max_size /*, nb_samples */;

		ref_clock = get_master_clock(is);
		diff = get_audio_clock(is) - ref_clock;

		if(diff < AV_NOSYNC_THRESHOLD) {
			// accumulate the diffs
			is->audio_diff_cum = diff + is->audio_diff_avg_coef
				* is->audio_diff_cum;
			if(is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				is->audio_diff_avg_count++;
			} else {
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if(fabs(avg_diff) >= is->audio_diff_threshold) {
					wanted_size = samples_size + ((int)(diff * is->audio_st->codec->sample_rate) * n);
					min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					if(wanted_size < min_size) {
						wanted_size = min_size;
					} else if (wanted_size > max_size) {
						wanted_size = max_size;
					}
					if(wanted_size < samples_size) {
						/* remove samples */
						samples_size = wanted_size;
					} else if(wanted_size > samples_size) {
						uint8_t *samples_end, *q;
						int nb;

						/* add samples by copying final sample*/
						nb = (samples_size - wanted_size);
						samples_end = (uint8_t *)samples + samples_size - n;
						q = samples_end + n;
						while(nb > 0) {
							memcpy(q, samples_end, n);
							q += n;
							nb -= n;
						}
						samples_size = wanted_size;
					}
				}
			}
		} else {
			/* difference is TOO big; reset diff stuff */
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	return samples_size;
}

int audio_decode_frame(VideoState *is, double *pts_ptr) {

	int len1,len2, data_size = 0, n, resampled_data_size;
	AVPacket *pkt = &is->audio_pkt;
	double pts;
	int64_t dec_channel_layout;
	AVRational tb;

	for(;;) 
	{
		while(is->audio_pkt_size > 0) 
		{
			int got_frame = 0;
			len1 = avcodec_decode_audio4(is->audio_st->codec, &is->audio_frame, &got_frame, pkt);
			if(len1 < 0) {
				/* if error, skip frame */
				is->audio_pkt_size = 0;
				break;
			}

			is->audio_pkt_data += len1;
			is->audio_pkt_size -= len1;

			if (got_frame)
			{
				tb = (AVRational){1, is->audio_frame.sample_rate};
				if(is->audio_frame.pts != AV_NOPTS_VALUE)
					is->audio_frame.pts = av_rescale_q(is->audio_frame.pts, is->audio_st->codec->time_base, tb);
				if(is->audio_frame.pts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
					is->audio_frame.pts = av_rescale_q(pkt->pts, is->audio_st->time_base, tb);
				if(pkt->pts != AV_NOPTS_VALUE)
					pkt->pts += (double)is->audio_frame.nb_samples / is->audio_frame.sample_rate / av_q2d(is->audio_st->time_base);

				data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(&(is->audio_frame)),
						is->audio_frame.nb_samples,
						is->audio_frame.format, 1);
				dec_channel_layout =
					(is->audio_frame.channel_layout && av_frame_get_channels(&(is->audio_frame)) == av_get_channel_layout_nb_channels(is->audio_frame.channel_layout)) ? 
					is->audio_frame.channel_layout : av_get_default_channel_layout(av_frame_get_channels(&(is->audio_frame)));

				if(is->audio_frame.format != is->audio_src.fmt
						|| dec_channel_layout != is->audio_src.channel_layout
						|| is->audio_frame.sample_rate != is->audio_src.freq
						|| is->swr_ctx == NULL)
				{
					if(is->swr_ctx)
						swr_free(is->swr_ctx);

					is->swr_ctx = swr_alloc_set_opts(NULL,
							is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
							dec_channel_layout, is->audio_frame.format, is->audio_frame.sample_rate,
							0, NULL);
					if(is->swr_ctx == NULL || swr_init(is->swr_ctx) < 0)
					{
						printf("create swr_ctx error\n");
						break;
					}

					is->audio_src.channel_layout = dec_channel_layout;
					is->audio_src.channels = av_frame_get_channels(&(is->audio_frame));
					is->audio_src.freq = is->audio_frame.sample_rate;
					is->audio_src.fmt = is->audio_frame.format;
				}

				if(is->swr_ctx)
				{
					const uint8_t **in = (const uint8_t **)is->audio_frame.extended_data;
					uint8_t **out = &is->audio_buf1;
					int out_count = (int64_t)is->audio_frame.nb_samples * is->audio_tgt.freq / is->audio_frame.sample_rate + 256;
					int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
					if(out_size < 0)
					{
						printf("av_samples_get_buffer_size error\n");
						break;
					}

					av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
					if(!is->audio_buf1)
						return AVERROR(ENOMEM);
					len2 = swr_convert(is->swr_ctx, out, out_count, in, is->audio_frame.nb_samples);
					if(len2 < 0)
					{
						printf("swr_convert error\n");
						break;
					}
					if(len2 == out_count)
					{
						printf("warning: audio buffer is probably too small\n");
						swr_init(is->swr_ctx);
					}

					is->audio_buf = is->audio_buf1;
					resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);

				}
				else
				{
					is->audio_buf = is->audio_frame.data[0];
					resampled_data_size = data_size;
				}

				pts = is->audio_clock;
				if(is->audio_frame.pts != AV_NOPTS_VALUE)
				{
					is->audio_clock = is->audio_frame.pts * av_q2d(tb) + (double)is->audio_frame.nb_samples / is->audio_frame.sample_rate;
				}
				*pts_ptr = pts;

				return resampled_data_size;
				// printf("channels: %u, nb_samples: %d, data_size: %d\n\n", is->audio_st->codec->channels,
				//		  													is->audio_frame.nb_samples,
				//															data_size);
				//	printf("audio format: %d\n", is->audio_frame.format);
				//   memcpy(is->audio_buf, is->audio_frame.data[0], data_size);
			}
		}

		if(pkt->data)
			av_free_packet(pkt);

		if(is->quit) {
			return -1;
		}

		/* next packet */
		if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
			return -1;
		}
		if(pkt->data == flush_pkt.data) {
			avcodec_flush_buffers(is->audio_st->codec);
			continue;
		}
		is->audio_pkt_data = pkt->data;
		is->audio_pkt_size = pkt->size;
		/* if update, update the audio clock w/pts */
		if(pkt->pts != AV_NOPTS_VALUE) {
			is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
		}
	}
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
	VideoState *is = (VideoState *)userdata;
	int len1, audio_size;
	double pts;

	while(len > 0) {
		if(is->audio_buf_index >= is->audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(is, &pts);
			if(audio_size < 0) {
				/* If error, output silence */
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			} else {
				audio_size = synchronize_audio(is, (int16_t *)is->audio_buf,
						audio_size, pts);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if(len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {

	SDL_Rect rect;
	VideoPicture *vp;
    SubPicture *sp;
    AVPicture pict;
	//AVPicture pict;
	float aspect_ratio;
	int w, h, x, y;
	//int i;
    int i;

	vp = &is->pictq[is->pictq_rindex];
	if(vp->bmp) {
        if (is->subtitle_st) 
		{
            if (is->subpq_size > 0) {
                sp = &is->subpq[is->subpq_rindex];

                if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                    SDL_LockYUVOverlay (vp->bmp);

                    pict.data[0] = vp->bmp->pixels[0];
                    pict.data[1] = vp->bmp->pixels[2];
                    pict.data[2] = vp->bmp->pixels[1];

                    pict.linesize[0] = vp->bmp->pitches[0];
                    pict.linesize[1] = vp->bmp->pitches[2];
                    pict.linesize[2] = vp->bmp->pitches[1];

                    for (i = 0; i < sp->sub.num_rects; i++)
                        blend_subrect(&pict, sp->sub.rects[i],
                                      vp->bmp->w, vp->bmp->h);

                    SDL_UnlockYUVOverlay (vp->bmp);
                }
            }
        }
		
		if(is->video_st->codec->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		} else {
			aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) *
				is->video_st->codec->width / is->video_st->codec->height;
		}
		if(aspect_ratio <= 0.0) {
			aspect_ratio = (float)is->video_st->codec->width /
				(float)is->video_st->codec->height;
		}
		h = screen->h;
		w = ((int)rint(h * aspect_ratio)) & -3;

		if(w > screen->w) {
			w = screen->w;
			h = ((int)rint(w / aspect_ratio)) & -3;
		}
		x = (screen->w - w) / 2;
		y = (screen->h - h) / 2;

		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;
		SDL_DisplayYUVOverlay(vp->bmp, &rect);
	}
}

void video_refresh_timer(void *userdata) {

	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;
    SubPicture *sp, *sp2;

	if(is->video_st) {
		if(is->pictq_size == 0) {
			schedule_refresh(is, 1);
		} else {
			vp = &is->pictq[is->pictq_rindex];

			is->video_current_pts = vp->pts;
			is->video_current_pts_time = av_gettime();

			delay = vp->pts - is->frame_last_pts; /* the pts from last time */
			if(delay <= 0 || delay >= 1.0) {
				/* if incorrect delay, use previous one */
				delay = is->frame_last_delay;
			}
			/* save for next time */
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;

			/* update delay to sync to audio if not master source */
			if(is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
				ref_clock = get_master_clock(is);
				diff = vp->pts - ref_clock;

				/* Skip or repeat the frame. Take delay into account
				   FFPlay still doesn't "know if this is the best guess." */
				sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
				if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
					if(diff <= -sync_threshold) {
						delay = 0;
					} else if(diff >= sync_threshold) {
						delay = 2 * delay;
					}
				}
			}

			is->frame_timer += delay;
			/* computer the REAL delay */
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if(actual_delay < 0.010) {
				/* Really it should skip the picture instead */
				actual_delay = 0.010;
			}
			schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));

			if (is->subpq_size > 0) {
				sp = &is->subpq[is->subpq_rindex];

				if (is->subpq_size > 1)
					sp2 = &is->subpq[(is->subpq_rindex + 1) % SUBPICTURE_QUEUE_SIZE];
				else
					sp2 = NULL;

				if ((is->video_current_pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
						|| (sp2 && is->video_current_pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
				{
					avsubtitle_free(&sp->sub);

					/* update queue size and signal for next picture */
					if (++is->subpq_rindex == SUBPICTURE_QUEUE_SIZE)
						is->subpq_rindex = 0;

					SDL_LockMutex(is->subpq_mutex);
					is->subpq_size--;
					SDL_CondSignal(is->subpq_cond);
					SDL_UnlockMutex(is->subpq_mutex);
				}
			}

			/* show the picture! */
			video_display(is);

			/* update queue for next picture! */
			if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
				is->pictq_rindex = 0;
			}
			SDL_LockMutex(is->pictq_mutex);
			is->pictq_size--;
			SDL_CondSignal(is->pictq_cond);
			SDL_UnlockMutex(is->pictq_mutex);
		}
	} else {
		schedule_refresh(is, 100);
	}
}

void alloc_picture(void *userdata) {

	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;

	vp = &is->pictq[is->pictq_windex];
	if(vp->bmp) {
		// we already have one make another, bigger/smaller
		SDL_FreeYUVOverlay(vp->bmp);
	}
	// Allocate a place to put our YUV image on that screen
	vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,
			is->video_st->codec->height,
			SDL_YV12_OVERLAY,
			screen);
	vp->width = is->video_st->codec->width;
	vp->height = is->video_st->codec->height;

	SDL_LockMutex(is->pictq_mutex);
	vp->allocated = 1;
	SDL_CondSignal(is->pictq_cond);
	SDL_UnlockMutex(is->pictq_mutex);

}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts) {

	VideoPicture *vp;
	//int dst_pix_fmt;
	AVPicture pict;

	/* wait until we have space for a new pic */
	SDL_LockMutex(is->pictq_mutex);
	while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
			!is->quit) {
		SDL_CondWait(is->pictq_cond, is->pictq_mutex);
	}
	SDL_UnlockMutex(is->pictq_mutex);

	if(is->quit)
		return -1;

	// windex is set to 0 initially
	vp = &is->pictq[is->pictq_windex];

	/* allocate or resize the buffer! */
	if(!vp->bmp ||
			vp->width != is->video_st->codec->width ||
			vp->height != is->video_st->codec->height) {
		SDL_Event event;

		vp->allocated = 0;
		/* we have to do it in the main thread */
		event.type = FF_ALLOC_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);

		/* wait until we have a picture allocated */
		SDL_LockMutex(is->pictq_mutex);
		while(!vp->allocated && !is->quit) {
			SDL_CondWait(is->pictq_cond, is->pictq_mutex);
		}
		SDL_UnlockMutex(is->pictq_mutex);
		if(is->quit) {
			return -1;
		}
	}
	/* We have a place to put our picture on the queue */
	/* If we are skipping a frame, do we set this to null 
	   but still return vp->allocated = 1? */


	if(vp->bmp) {

		SDL_LockYUVOverlay(vp->bmp);

		//dst_pix_fmt = PIX_FMT_YUV420P;
		/* point pict at the queue */

		pict.data[0] = vp->bmp->pixels[0];
		pict.data[1] = vp->bmp->pixels[2];
		pict.data[2] = vp->bmp->pixels[1];

		pict.linesize[0] = vp->bmp->pitches[0];
		pict.linesize[1] = vp->bmp->pitches[2];
		pict.linesize[2] = vp->bmp->pitches[1];

		// Convert the image into YUV format that SDL uses
		sws_scale
			(
			 is->sws_ctx,
			 (uint8_t const * const *)pFrame->data,
			 pFrame->linesize,
			 0, 
			 is->video_st->codec->height, 
			 pict.data, 
			 pict.linesize
			);

		SDL_UnlockYUVOverlay(vp->bmp);
		vp->pts = pts;

		/* now we inform our display thread that we have a pic ready */
		if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
			is->pictq_windex = 0;
		}
		SDL_LockMutex(is->pictq_mutex);
		is->pictq_size++;
		SDL_UnlockMutex(is->pictq_mutex);
	}
	return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {

	double frame_delay;

	if(pts != 0) {
		/* if we have pts, set video clock to it */
		is->video_clock = pts;
	} else {
		/* if we aren't given a pts, set it to the clock */
		pts = is->video_clock;
	}
	/* update the video clock */
	frame_delay = av_q2d(is->video_st->codec->time_base);
	/* if we are repeating a frame, adjust clock accordingly */
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	is->video_clock += frame_delay;
	return pts;
}

uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;

/* These are called whenever we allocate a frame
 * buffer. We use this to store the global_pts in
 * a frame at the time it is allocated.
 */
int our_get_buffer(struct AVCodecContext *c, AVFrame *pic) {
	int ret = avcodec_default_get_buffer(c, pic);
	uint64_t *pts = av_malloc(sizeof(uint64_t));
	*pts = global_video_pkt_pts;
	pic->opaque = pts;
	return ret;
}
void our_release_buffer(struct AVCodecContext *c, AVFrame *pic) {
	if(pic) av_freep(&pic->opaque);
	avcodec_default_release_buffer(c, pic);
}

int subtitle_thread(void *arg)
{
	VideoState *is = (VideoState *)arg;
    SubPicture *sp;
	double pts;
	int ret, got_subtitle;
	AVPacket pkt1, *pkt = &pkt1;
    int i, j;
    int r, g, b, y, u, v, a;

	while(1)
	{
		ret = packet_queue_get(&is->subtitleq, pkt, 1);
		if(ret < 0)
			break;

		if(pkt->data == flush_pkt.data)
		{
			avcodec_flush_buffers(is->subtitle_st->codec);
			continue;
		}
		
        SDL_LockMutex(is->subpq_mutex);
        while (is->subpq_size >= SUBPICTURE_QUEUE_SIZE &&
               !is->quit) {
            SDL_CondWait(is->subpq_cond, is->subpq_mutex);
        }
        SDL_UnlockMutex(is->subpq_mutex);

		if(is->quit)
			return 0;

		sp = &is->subpq[is->subpq_windex];
		pts = 0;
		if(pkt->pts != AV_NOPTS_VALUE)
			pts = av_q2d(is->subtitle_st->time_base) * pkt->pts;

		avcodec_decode_subtitle2(is->subtitle_st->codec, &sp->sub, &got_subtitle, pkt);
		if(got_subtitle && sp->sub.format == 0)
		{
			if(sp->sub.pts != AV_NOPTS_VALUE)
				pts = sp->sub.pts / (double)AV_TIME_BASE;
			sp->pts = pts;


            for (i = 0; i < sp->sub.num_rects; i++)
            {
                for (j = 0; j < sp->sub.rects[i]->nb_colors; j++)
                {
                    RGBA_IN(r, g, b, a, (uint32_t*)sp->sub.rects[i]->pict.data[1] + j);
                    y = RGB_TO_Y_CCIR(r, g, b);
                    u = RGB_TO_U_CCIR(r, g, b, 0);
                    v = RGB_TO_V_CCIR(r, g, b, 0);
                    YUVA_OUT((uint32_t*)sp->sub.rects[i]->pict.data[1] + j, y, u, v, a);
                }
            }
			
            /* now we can update the picture count */
            if (++is->subpq_windex == SUBPICTURE_QUEUE_SIZE)
                is->subpq_windex = 0;
            SDL_LockMutex(is->subpq_mutex);
            is->subpq_size++;
            SDL_UnlockMutex(is->subpq_mutex);

		}
		av_free_packet(pkt);
	}

	return 0;
}

int video_thread(void *arg) {
	VideoState *is = (VideoState *)arg;
	AVPacket pkt1, *packet = &pkt1;
	int frameFinished;
	AVFrame *pFrame;
	double pts;

	pFrame = avcodec_alloc_frame();

	for(;;) {
		if(packet_queue_get(&is->videoq, packet, 1) < 0) {
			// means we quit getting packets
			break;
		}
		if(packet->data == flush_pkt.data) {
			avcodec_flush_buffers(is->video_st->codec);
			continue;
		}
		pts = 0;

		// Save global pts to be stored in pFrame in first call
		global_video_pkt_pts = packet->pts;
		// Decode video frame
		avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished, 
				packet);
		if(packet->dts == AV_NOPTS_VALUE 
				&& pFrame->opaque && *(uint64_t*)pFrame->opaque != AV_NOPTS_VALUE) {
			pts = *(uint64_t *)pFrame->opaque;
		} else if(packet->dts != AV_NOPTS_VALUE) {
			pts = packet->dts;
		} else {
			pts = 0;
		}
		pts *= av_q2d(is->video_st->time_base);

		// Did we get a video frame?
		if(frameFinished) {
			pts = synchronize_video(is, pFrame, pts);
			if(queue_picture(is, pFrame, pts) < 0) {
				break;
			}
		}
		av_free_packet(packet);
	}
	av_free(pFrame);
	return 0;
}

int stream_component_close(VideoState *is, int stream_index) 
{
	AVCodecContext *codecCtx = NULL;
	AVFormatContext *pFormatCtx = is->pFormatCtx;

	if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
		return -1;
	}

	codecCtx = pFormatCtx->streams[stream_index]->codec;
	switch(codecCtx->codec_type)
	{
		case AVMEDIA_TYPE_AUDIO:
			SDL_CloseAudio();

			packet_queue_flush(&is->audioq);
			av_free_packet(&is->audio_pkt);
			swr_free(&is->swr_ctx);
			av_freep(&is->audio_buf1);
			is->audio_buf1_size = 0;
			is->audio_buf = NULL;
			break;

		case AVMEDIA_TYPE_VIDEO:

			/* note: we also signal this mutex to make sure we deblock the
			   video thread in all cases */
			SDL_LockMutex(is->pictq_mutex);
			SDL_CondSignal(is->pictq_cond);
			SDL_UnlockMutex(is->pictq_mutex);
			packet_queue_flush(&is->videoq);

			SDL_WaitThread(is->video_tid, NULL);
			break;

		case AVMEDIA_TYPE_SUBTITLE:
			packet_queue_flush(&is->subtitleq);
			SDL_WaitThread(is->subtitle_tid, NULL);
		default:
			break;
	}



}

int stream_component_open(VideoState *is, int stream_index) {

	AVFormatContext *pFormatCtx = is->pFormatCtx;
	AVCodecContext *codecCtx = NULL;
	AVCodec *codec = NULL;
	AVDictionary *optionsDict = NULL;
	SDL_AudioSpec wanted_spec, spec;
	int64_t wanted_channel_layout;
	int wanted_nb_channels;
	const char *env;

	if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
		return -1;
	}

	// Get a pointer to the codec context for the video stream
	codecCtx = pFormatCtx->streams[stream_index]->codec;

	if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
		// Set audio settings from codec info
		wanted_spec.freq = codecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = codecCtx->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}
		is->audio_hw_buf_size = spec.size;

		wanted_channel_layout = codecCtx->channel_layout;
		wanted_nb_channels = codecCtx->channels;
		env = SDL_getenv("SDL_AUDIO_CHANNELS");
		if (env) {
			wanted_nb_channels = atoi(env);
			wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		}
		if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
			wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
			wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
		}

		is->audio_tgt.fmt = AV_SAMPLE_FMT_S16;
		is->audio_tgt.freq = spec.freq;
		is->audio_tgt.channel_layout = wanted_channel_layout;
		is->audio_tgt.channels = spec.channels;

		is->audio_src = is->audio_tgt;
		printf("samples: %u, bytes: %u\n", spec.samples, spec.size);
		printf("codec id: %u, VORBIS = %d\n", codecCtx->codec_id, AV_CODEC_ID_VORBIS);
		printf("channel_layout: %d\n", codecCtx->channel_layout);
	}

	codec = avcodec_find_decoder(codecCtx->codec_id);
	if(!codec || (avcodec_open2(codecCtx, codec, &optionsDict) < 0)) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	switch(codecCtx->codec_type) {
		case AVMEDIA_TYPE_AUDIO:
			is->audioStream = stream_index;
			is->audio_st = pFormatCtx->streams[stream_index];
			is->audio_buf_size = 0;
			is->audio_buf_index = 0;

			/* averaging filter for audio sync */
			is->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
			is->audio_diff_avg_count = 0;
			/* Correct audio only if larger error than this */
			is->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE / codecCtx->sample_rate;

			memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
			packet_queue_init(&is->audioq);
			SDL_PauseAudio(0);
			break;
		case AVMEDIA_TYPE_VIDEO:
			is->videoStream = stream_index;
			is->video_st = pFormatCtx->streams[stream_index];

			is->frame_timer = (double)av_gettime() / 1000000.0;
			is->frame_last_delay = 40e-3;
			is->video_current_pts_time = av_gettime();

			packet_queue_init(&is->videoq);
			is->video_tid = SDL_CreateThread(video_thread, is);
			is->sws_ctx =
				sws_getContext
				(
				 is->video_st->codec->width,
				 is->video_st->codec->height,
				 is->video_st->codec->pix_fmt,
				 is->video_st->codec->width,
				 is->video_st->codec->height,
				 PIX_FMT_YUV420P, 
				 SWS_BILINEAR, 
				 NULL, 
				 NULL, 
				 NULL
				);
			codecCtx->get_buffer = our_get_buffer;
			codecCtx->release_buffer = our_release_buffer;
			break;
		case AVMEDIA_TYPE_SUBTITLE:
			is->subtitleStream = stream_index;
			is->subtitle_st = pformatCtx->streams[stream_index];
			packet_queue_init(&is->subtitleq);
			is->subtitle_tid = SDL_CreateThread(subtitle_thread, is);
			break;
		default:
			break;
	}

	return 0;
}

int decode_interrupt_cb(void *opaque) {
	return (global_video_state && global_video_state->quit);
}
int decode_thread(void *arg) {

	VideoState *is = (VideoState *)arg;
	AVFormatContext *pFormatCtx = NULL;
	AVPacket pkt1, *packet = &pkt1;

	AVDictionary *io_dict = NULL;
	AVIOInterruptCB callback;

	int video_index = -1;
	int audio_index = -1;
	int subtitle_index = -1;
	int i, fail_flag = 0;

	is->videoStream=-1;
	is->audioStream=-1;

	global_video_state = is;
	// will interrupt blocking functions if we quit!
	callback.callback = decode_interrupt_cb;
	callback.opaque = is;
	if (avio_open2(&is->io_context, is->filename, 0, &callback, &io_dict))
	{
		fprintf(stderr, "Unable to open I/O for %s\n", is->filename);
		return -1;
	}

	// Open video file
	pFormatCtx = avformat_alloc_context();
	if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0)
	{
		printf("avformat_open_input: %s\n", is->filename);
		goto READ_RET;
	}

	is->pFormatCtx = pFormatCtx;

	// Retrieve stream information
	if(avformat_find_stream_info(pFormatCtx, NULL)<0)
	{
		printf("avformat_find_stream_info\n");
		goto READ_RET;
	}
	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, is->filename, 0);

	// Find the first video stream
	for(i=0; i<pFormatCtx->nb_streams; i++) {
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO &&
				video_index < 0) {
			video_index=i;
		}
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO &&
				audio_index < 0) {
			audio_index=i;
		}
		if(pFormatCtx->streams[i]->codec->code_type == AVMEDIA_TYPE_SUBTITLE &&
				subtitle_index < 0)
			subtitle_index = i;
	}
	if(audio_index >= 0) {
		stream_component_open(is, audio_index);
	}
	if(video_index >= 0) {
		stream_component_open(is, video_index);
	}   
	if(subtitle_index >= 0){
		stream_component_open(is, subtitle_index);
	}

	if(is->videoStream < 0 || is->audioStream < 0) {
		fprintf(stderr, "%s: could not open codecs\n", is->filename);
		goto READ_RET;
	}

	// main decode loop

	for(;;) {
		if(is->quit) {
			break;
		}
		// seek stuff goes here
		if(is->seek_req) {
			int stream_index= -1;
			int64_t seek_target = is->seek_pos;

			if     (is->videoStream >= 0) stream_index = is->videoStream;
			else if(is->audioStream >= 0) stream_index = is->audioStream;

			if(stream_index>=0){
				seek_target= av_rescale_q(seek_target, AV_TIME_BASE_Q, pFormatCtx->streams[stream_index]->time_base);
			}
			if(av_seek_frame(is->pFormatCtx, stream_index, seek_target, is->seek_flags) < 0) {
				fprintf(stderr, "%s: error while seeking\n", is->pFormatCtx->filename);
			} else {
				if(is->audioStream >= 0) {
					packet_queue_flush(&is->audioq);
					packet_queue_put(&is->audioq, &flush_pkt);
				}
				if(is->videoStream >= 0) {
					packet_queue_flush(&is->videoq);
					packet_queue_put(&is->videoq, &flush_pkt);
				}
			}
			is->seek_req = 0;
		}

		if(is->audioq.size > MAX_AUDIOQ_SIZE ||
				is->videoq.size > MAX_VIDEOQ_SIZE) {
			SDL_Delay(10);
			continue;
		}
		if(av_read_frame(is->pFormatCtx, packet) < 0) {
			if(is->pFormatCtx->pb->error == 0) {
				SDL_Delay(100); /* no error; wait for user input */
				continue;
			} else {
				fail_flag = 1;
				break;
			}
		}
		// Is this a packet from the video stream?
		if(packet->stream_index == is->videoStream) {
			packet_queue_put(&is->videoq, packet);
		} else if(packet->stream_index == is->audioStream) {
			packet_queue_put(&is->audioq, packet);
		} else if(packet->stream_index == is->subtitleStream)
		{
			packet_queue_put(&is->subtitleq, packet);
		}	
		else {
			av_free_packet(packet);
		}
	}
	/* all done - wait for it */
	while(!is->quit) {
		SDL_Delay(100);
	}

READ_RET:
	{
		if (is->audioStream >= 0)
			stream_component_close(is, is->audioStream);
		if (is->videoStream >= 0)
			stream_component_close(is, is->videoStream);
		if (is->subtitleStream >= 0)
			stream_component_close(is, is->subtitleStream);
		if(is->pFormatCtx)
		{
			avformat_close_input(&is->pFormatCtx);
		}

		if(fail_flag)
		{
			SDL_Event event;
			event.type = FF_QUIT_EVENT;
			event.user.data1 = is;
			SDL_PushEvent(&event);
		}
	}
	return 0;
}

void stream_seek(VideoState *is, int64_t pos, int rel) {

	if(!is->seek_req) {
		is->seek_pos = pos;
		is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
		is->seek_req = 1;
	}
}

int do_exit(VideoState *is)
{
	VideoPicture *vp;
	int i;

	printf("quit player\n");
	is->quit = 1;
	SDL_WaitThread(is->parse_tid, NULL);

	SDL_DestroyMutex(is->pictq_mutex);
	SDL_DestroyMutex(is->pictq_cond);


	/*
	 * If the video has finished playing, then both the picture and
	 * audio queues are waiting for more data.  Make them stop
	 * waiting and terminate normally.
	 */
	SDL_CondSignal(is->audioq.cond);
	SDL_CondSignal(is->videoq.cond);

	packet_queue_destroy(&is->videoq);
	packet_queue_destroy(&is->audioq);

	for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++) 
	{
		vp = &is->pictq[i];
		if (vp->bmp) 
		{
			SDL_FreeYUVOverlay(vp->bmp);
			vp->bmp = NULL;
		}
	}
	av_freep(&is);

	SDL_Quit();
	exit(0);
}

int quit_main(VideoState *is)
{
	if(is)
	{
		SDL_DestroyMutex(is->pictq_mutex);
		SDL_DestroyCond(is->pictq_cond);

		SDL_DestroyMutex(is->subpq_mutex);
		SDL_DestroyCond(is->subpq_mutex);
		av_freep(&is);
	}

	SDL_Quit();
	exit(-1);
}

int main(int argc, char *argv[]) {

	SDL_Event       event;
	VideoState      *is = NULL;

	if(argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		exit(-1);
	}

	// Register all formats and codecs
	av_register_all();

	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(-1);
	}

	// Make a screen to put our video
	const SDL_VideoInfo *vi = SDL_GetVideoInfo();
#if 1 
#ifndef __DARWIN__
	screen = SDL_SetVideoMode(640, 480, 0, SDL_RESIZABLE);
#else
	screen = SDL_SetVideoMode(640, 480, 24, SDL_RESIZABLE);
#endif
#else
	screen = SDL_SetVideoMode(vi->current_w, vi->current_h, 0, SDL_FULLSCREEN);
#endif
	if(!screen) {
		fprintf(stderr, "SDL: could not set video mode - exiting\n");
		goto MAIN_RET;
	}

	is = av_mallocz(sizeof(VideoState));
	if(is == NULL)
	{
		printf("av_mallocz error: VideoState\n");
		goto MAIN_RET;
	}

	av_strlcpy(is->filename, argv[1], 1024);

	is->pictq_mutex = SDL_CreateMutex();
	is->pictq_cond = SDL_CreateCond();

	is->subpq_mutex = SDL_CreateMutex();
	is->subpq_cond = SDL_CreateCond();

	schedule_refresh(is, 40);

	is->av_sync_type = DEFAULT_AV_SYNC_TYPE;
	is->parse_tid = SDL_CreateThread(decode_thread, is);
	if(!is->parse_tid) {
		goto MAIN_RET;
	}

	av_init_packet(&flush_pkt);
	flush_pkt.data = (unsigned char *)"FLUSH";

	for(;;) {
		double incr, pos;
		SDL_WaitEvent(&event);
		switch(event.type) {
			case SDL_KEYDOWN:
				switch(event.key.keysym.sym) {
					case SDLK_LEFT:
						incr = -10.0;
						goto do_seek;
					case SDLK_RIGHT:
						incr = 10.0;
						goto do_seek;
					case SDLK_UP:
						incr = 60.0;
						goto do_seek;
					case SDLK_DOWN:
						incr = -60.0;
						goto do_seek;
do_seek:
						if(global_video_state) {
							pos = get_master_clock(global_video_state);
							pos += incr;
							stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
						}
						break;
				//	case SDL_ESC:

					default:
						break;
				}
				break;
			case SDL_VIDEORESIZE:
				{
					printf("resize window 1\n");
					//screen = SDL_SetVideoMode(event.resize.w, event.resize.h, 24, SDL_RESIZABLE);
					screen = SDL_SetVideoMode(event.resize.w, event.resize.h, 24, 0);
					printf("resize over\n");
					break;
				}
			case FF_QUIT_EVENT:
			case SDL_QUIT:
				do_exit(is);
				break;
			case FF_ALLOC_EVENT:
				alloc_picture(event.user.data1);
				break;
			case FF_REFRESH_EVENT:
				video_refresh_timer(event.user.data1);
				break;
			default:
				break;
		}
	}

MAIN_RET:
	quit_main(is);

	return 0;
}
