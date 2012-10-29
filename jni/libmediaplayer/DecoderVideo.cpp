#include <unistd.h>
#include "Utils.h"
#include "DecoderVideo.h"
#include "IDecoder.h"
#include "BuddyRunnable.h"

#define LOG_TAG "DecoderVideo"
#include <common/logwrapper.h>

/* no AV sync correction is done if below the AV sync threshold */
#define AV_SYNC_THRESHOLD 0.01
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

#define FRAME_SKIP_FACTOR 0.05
static uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;

namespace ffplayer {
DecoderVideo::DecoderVideo(AVStream* stream, PacketQueue *queue,
		Thread *loopThread) :
		mStream(stream), mQueue(queue), mLoopThread(loopThread), mVideoClock(
				0.0) {
	mStream->codec->get_buffer = getBuffer;
	mStream->codec->release_buffer = releaseBuffer;
}

DecoderVideo::~DecoderVideo() {

}

void DecoderVideo::onInitialize() {
	mFrame = avcodec_alloc_frame();
	mLastPts = 0.0;
}

void DecoderVideo::onFinalize() {
	return;
}

BuddyRunnable::BuddyType DecoderVideo::getBuddyType() {
	return FOLLOWER;
}

int DecoderVideo::onBuddyEvent(BuddyRunnable *buddy, BuddyEvent evt) {
	ISSUE_FUNCTION_IN();
	LOGD("Event type:%d", evt.type);
	switch (evt.type) {
	case AV_SYNC:
	{
		double sysClock = evt.data.dData;
		LOGD("Event type:%f", sysClock);
		decodeRender(sysClock);
	}
		break;

	default:
		break;
	}
}
int DecoderVideo::bindBuddy(BuddyRunnable *buddy) {
	if (mBuddy) {
		return -1;
	} else {
		mBuddy = buddy;
	}
}

int DecoderVideo::getGCFlags() {
	return BuddyRunnable::GC_BY_EXTERNAL;
}

// interface of IDecoder
void DecoderVideo::flush() {
	//TODO: more precise, should put a flush pkt
	CHECK_POINTER_VOID(mQueue);
	mQueue->flush();
	CHECK_POINTER_VOID(mStream);
	avcodec_flush_buffers(mStream->codec);
}

int DecoderVideo::stop() {

}

int DecoderVideo::start() {

}

/* pts is in the unit of second */
int DecoderVideo::synchronize(AVFrame *srcFrame, double pts, double clock) {
	ISSUE_FUNCTION_IN();
	if (pts != 0) {
		mVideoClock = pts;
	} else {
		pts = mVideoClock;
	}

	double delay, sync_threshold, diff;
	delay = av_q2d(mStream->codec->time_base);
	delay += srcFrame->repeat_pict * (delay * 0.5);
	mVideoClock += delay;
	LOGD("mVideoClock:%f", mVideoClock);

	/* compute nominal delay */
//	delay = pts - mLastPts;
//	if (delay <= 0 || delay >= 10.0) {
//		/* if incorrect delay, use previous one */
//		delay = mLastPts;
//	} else {
//		mLastPts = delay;
//	}
//	mLastPts = pts;
	diff = pts - clock;
	sync_threshold = FFMAX(AV_SYNC_THRESHOLD, delay);
	if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
		if (diff <= -sync_threshold)
			delay = 0;
		else if (diff >= sync_threshold)
			delay = 1.5 * delay;
	}

	LOGD("Video Decoder Thread sync diff:%f ms!", diff);

	if (delay > 0.0) {
		usleep(delay * 1e6);
	}
	return 0;
}

int DecoderVideo::decodeRender(double clock) {
	AVPacket pkt, *pPacket = &pkt;
	LOGD("Video Queue get start()");
	if (mQueue->get(&pkt, true) < 0) {
		LOGD("getVideo Packet error!");
		return -1;
	}LOGD("Video Queue get passed()");
	global_video_pkt_pts = pkt.pts;

	/* Process some special Events
	 * 1) BOS
	 * 2) EOS
	 */
	if (&BOS_PKT == pPacket) {
		LOGD("Video Decoder Received BOS PKT!");
		return true;
	} else if (&EOS_PKT == pPacket) {
		LOGD("Video Decoder Received EOS PKT!");
		return false;
	}

	int got_picture;
	long long pts = 0;

	// Decode video frame
	if (pPacket->size == 0 || NULL == pPacket->data) {
		return true;
	}

	avcodec_decode_video2(mStream->codec, mFrame, &got_picture, pPacket);
	LOGD("pPacket->dts:%lld, packet->pts:%lld", pPacket->dts, pPacket->pts);

	if (got_picture) {
		if (pPacket->dts == AV_NOPTS_VALUE && mFrame->opaque
				&& *(uint64_t*) mFrame->opaque != AV_NOPTS_VALUE) {
			pts = *(uint64_t *) mFrame->opaque;
			LOGD("Our case(pts:%ld)!", pts);
		} else if (pPacket->dts != AV_NOPTS_VALUE) {
			pts = pPacket->dts;
		} else {
			pts = 0;
		}

		double videoTs = pts * av_q2d(mStream->time_base);

		pts = synchronize(mFrame, videoTs, clock);
		rendorHook(mFrame);
		LOGD("Video Decoder Thread Processing!");
	}

	LOGD("Video Decoder Thread Decoding error ret:%d, pkt:(data %p size:%d)!", got_picture, pPacket->data, pPacket->size);
	// Free the packet that was allocated by av_read_frame
	av_free_packet(pPacket);

	LOGD("decoding video ended");
	// Free the RGB image
	av_free(mFrame);
	return 0;
}

void DecoderVideo::run(void* ptr) {

}

/* These are called whenever we allocate a frame
 * buffer. We use this to store the global_pts in
 * a frame at the time it is allocated.
 */
int DecoderVideo::getBuffer(struct AVCodecContext *c, AVFrame *pic) {
	int ret = avcodec_default_get_buffer(c, pic);
	uint64_t *pts = (uint64_t *) av_malloc(sizeof(uint64_t));
	*pts = global_video_pkt_pts;
	pic->opaque = pts;
	return ret;
}

void DecoderVideo::releaseBuffer(struct AVCodecContext *c, AVFrame *pic) {
	if (pic)
		av_freep(&pic->opaque);
	avcodec_default_release_buffer(c, pic);
}
}
