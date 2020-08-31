#ifndef  _BYTE_STREAM_CAMERA_SOURCE_
#define _BYTE_STREMA_CAMERA_SOURCE_

#ifndef _FRAMED_SOURCE_HH
#include "FramedSource.hh"
#endif

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct buffer {
	void   *start;
	size_t  length;
};

class ByteStreamCameraSource: public FramedSource {
public:
	static ByteStreamCameraSource* createNew(UsageEnvironment& env);

protected:
	ByteStreamCameraSource(UsageEnvironment& env);
	~ByteStreamCameraSource();
private:
	virtual void doGetNextFrame();
	virtual void doStopGettingFrames();
	void errno_exit(const char *);
	static void *thread_fun(void *);
	static void doRead(ByteStreamCameraSource *);
private:
	struct buffer *buffers;
	unsigned int n_buffers;
	int camera_fd;
    AVCodecContext *ctx;
	struct SwsContext* sws_ctx;
	AVFrame *frame;
	AVPacket *pkt;
	int thread_run;
	pthread_t threadId;
	unsigned LastPlayTime;
	bool startReading;
	char temp_buffer[20000];
	int temp_buffer_used;
};
#endif
