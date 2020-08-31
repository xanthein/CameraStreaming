#include "ByteStreamCameraSource.hh"

ByteStreamCameraSource *
ByteStreamCameraSource::createNew(UsageEnvironment& env)
{
	ByteStreamCameraSource *newSource =
		new ByteStreamCameraSource(env);

	return newSource;
}

#define DEV_NAME "/dev/video0"
ByteStreamCameraSource::ByteStreamCameraSource(UsageEnvironment& env)
	: FramedSource(env), thread_run(0), LastPlayTime(0), startReading(false),
	temp_buffer_used(0)
{
	camera_fd = open(DEV_NAME, O_RDWR | O_NONBLOCK, 0);
	if (camera_fd == -1) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
				DEV_NAME, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

    struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;

	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (!ioctl(camera_fd, VIDIOC_CROPCAP, &cropcap)) {
		printf("width %d, height %d\n", cropcap.defrect.width,
				cropcap.defrect.height);
	}
   
	//set capture format
	struct v4l2_format fmt;
	
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = 640; //replace
	fmt.fmt.pix.height      = 480; //replace
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; //replace
	fmt.fmt.pix.field       = V4L2_FIELD_ANY;

	if(ioctl(camera_fd, VIDIOC_S_FMT, &fmt) == -1)
		printf("VIDIOC_S_FMT failed\n");

    struct v4l2_streamparm parm;

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    //parm.parm.capture.timeperframe.numerator = 1;
    //parm.parm.capture.timeperframe.denominator = 30;

    if(ioctl(camera_fd, VIDIOC_G_PARM, &parm) == -1)
		printf("VIDIOC_G_PARM failed\n");

	struct v4l2_requestbuffers req;

	memset(&req, 0, sizeof(req));

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == ioctl(camera_fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support "
							"memory mapping\n", DEV_NAME);
			exit(EXIT_FAILURE);
    	} else {
			errno_exit("VIDIOC_REQBUFS");
    	}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory on %s\n",
						DEV_NAME);
		exit(EXIT_FAILURE);
	}

	buffers = new buffer [req.count];

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));

		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = n_buffers;

		if (-1 == ioctl(camera_fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start =
			mmap(NULL /* start anywhere */,
				buf.length,
				PROT_READ | PROT_WRITE /* required */,
				MAP_SHARED /* recommended */,
				camera_fd, buf.m.offset);

		if (MAP_FAILED == buffers[n_buffers].start)
			errno_exit("mmap");
	}

	for (int i = 0; i < n_buffers; ++i) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == ioctl(camera_fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");
	}
    
	enum v4l2_buf_type type;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == ioctl(camera_fd, VIDIOC_STREAMON, &type))
		errno_exit("VIDIOC_STREAMON");


	const AVCodec *codec;
	int ret;
	
	avcodec_register_all();

    codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        fprintf(stderr, "Codec libx264 not found\n");
        exit(1);
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    /* put sample parameters */
    ctx->bit_rate = 400000;
    /* resolution must be a multiple of two */
    ctx->width = 640;
    ctx->height = 480;
    /* frames per second */
    ctx->time_base = (AVRational){1, 30};
    ctx->framerate = (AVRational){30, 1};

    /* emit one intra frame every ten frames
     * check frame pict_type before passing frame
     * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
     * then gop_size is ignored and the output of encoder
     * will always be I frame irrespective to gop_size
     */
    ctx->gop_size = 10;
    ctx->max_b_frames = 1;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (codec->id == AV_CODEC_ID_H264)
        av_opt_set(ctx->priv_data, "preset", "slow", 0);

    /* open it */
    ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    frame->format = ctx->pix_fmt;
    frame->width  = ctx->width;
    frame->height = ctx->height;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        exit(1);
    }

	sws_ctx = sws_getContext(ctx->width,
		ctx->height,
		AV_PIX_FMT_YUYV422,
		ctx->width,
		ctx->height,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR, NULL, NULL, NULL);
}

ByteStreamCameraSource::~ByteStreamCameraSource()
{
    avcodec_free_context(&ctx);
	sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

	enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == ioctl(camera_fd, VIDIOC_STREAMOFF, &type))
		errno_exit("VIDIOC_STREAMOFF");

	::close(camera_fd);
	
	for (int i = 0; i < n_buffers; ++i)
		if (-1 == munmap(buffers[i].start, buffers[i].length))
			errno_exit("munmap");
	free(buffers);	
}

void ByteStreamCameraSource::doRead(ByteStreamCameraSource *source)
{
	//get frame
	int ret = 0;
	source->fFrameSize = 0;
	unsigned int frame_count = 0;

	if(source->temp_buffer_used != 0 && source->temp_buffer_used < source->fMaxSize) { 
    	memcpy(source->fTo, source->temp_buffer, source->temp_buffer_used);
		source->fFrameSize += source->temp_buffer_used;
		frame_count++;
		source->temp_buffer_used = 0;
	}

	while (ret >= 0 && source->fFrameSize < source->fMaxSize) {
        ret = avcodec_receive_packet(source->ctx, source->pkt);
		//printf("ret = %d\n", ret);
        if (ret == AVERROR(EAGAIN)) {
			usleep(1000);
			//ret = 0;
			continue;
		}
		else if(ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            //exit(1);
			break;
        }

        //printf("Write packet %3"PRId64" (size=%5d)\n", source->pkt->pts, source->pkt->size);
		int remain_size = source->fMaxSize - source->fFrameSize;
		if(remain_size > source->pkt->size) {
        	memcpy(source->fTo+source->fFrameSize, source->pkt->data, source->pkt->size);
			source->fFrameSize += source->pkt->size;
			frame_count++;
		}
		else {
        	memcpy(source->fTo+source->fFrameSize, source->pkt->data, remain_size);
			memcpy(source->temp_buffer, source->pkt->data+remain_size, source->pkt->size - remain_size);
			source->temp_buffer_used = source->pkt->size - remain_size;
			source->fFrameSize += remain_size;
		}
        av_packet_unref(source->pkt);
    }

	//printf("++ret = %d\n", ret);
	//printf("fFrameSize %d\n", source->fFrameSize);
	//printf("frame_count %d\n", frame_count);
	if (frame_count > 0) {
	    if (source->fPresentationTime.tv_sec == 0 && source->fPresentationTime.tv_usec == 0)
			gettimeofday(&source->fPresentationTime, NULL);
		else {
			unsigned uSeconds = source->fPresentationTime.tv_usec + source->LastPlayTime;
			source->fPresentationTime.tv_sec += uSeconds/1000000;
			source->fPresentationTime.tv_usec = uSeconds%1000000;
		}
		source->LastPlayTime = frame_count * 33333;
		source->fDurationInMicroseconds = source->LastPlayTime;

		//nextTask() = envir().taskScheduler().scheduleDelayedTask(0,
		//			(TaskFunc*)FramedSource::afterGetting, this);
	}
	source->startReading = false;
	FramedSource::afterGetting(source);
}

void ByteStreamCameraSource::doGetNextFrame()
{
	//start thread
	int ret;

	if (!thread_run) {
		thread_run = 1;
		ret = pthread_create(&threadId, NULL, thread_fun, this);
		if (ret)
		{
			printf("thread create failed\n");
			return;
		}
	}

	if(!startReading) {
		nextTask() = envir().taskScheduler().scheduleDelayedTask(0,
					(TaskFunc*)&doRead, this);
		startReading = true;
	}
}

void ByteStreamCameraSource::doStopGettingFrames()
{
	//stop thread
	thread_run = 0;
    pthread_join(threadId, NULL);

	//read all remain data out
    int ret = avcodec_send_frame(ctx, NULL);
    if (ret < 0)
        printf("Error sending a frame for encoding\n");

	while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            //exit(1);
			break;
        }

        printf("Get packet %3"PRId64" (size=%5d)\n", pkt->pts, pkt->size);
        av_packet_unref(pkt);
    }

    fPresentationTime.tv_sec = 0;
	fPresentationTime.tv_usec = 0;
}

void ByteStreamCameraSource::errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

void *ByteStreamCameraSource::thread_fun(void *data)
{
	ByteStreamCameraSource *t = (ByteStreamCameraSource *)data;
	fd_set fds;
	struct timeval tv;
	int r;
	const uint8_t *temp_buf[AV_NUM_DATA_POINTERS];
	int sws_linesize[4] = {1280, 0, 0, 0};
	int frame_count = 0;
	
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	while(t->thread_run) {
		FD_ZERO(&fds);
		FD_SET(t->camera_fd, &fds);

		/* Timeout. */
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		r = select(t->camera_fd + 1, &fds, NULL, NULL, &tv);

		if (-1 == r) {
			//if (EINTR == errno)
			//	continue;
			t->errno_exit("select");
		}

		if (0 == r) {
			fprintf(stderr, "select timeout\n");
			exit(EXIT_FAILURE);
		}

		if (-1 == ioctl(t->camera_fd, VIDIOC_DQBUF, &buf)) {
			switch (errno) {
				case EAGAIN:
					printf("EAGAIN\n");
				case EIO:
					printf("EIO\n");
					/* Could ignore EIO, see spec. */
					/* fall through */
				default:
					t->errno_exit("VIDIOC_DQBUF");
			}
		}

		assert(buf.index < t->n_buffers);

		temp_buf[0] = (uint8_t*)t->buffers[buf.index].start;
	
		sws_scale(t->sws_ctx, temp_buf,
			sws_linesize,
			0,
			480,
			t->frame->data,
			t->frame->linesize);

		if (av_frame_make_writable(t->frame) < 0)
    		printf("av_frame_make_writeable failed\n");
		t->frame->pts = frame_count++;
		
        //printf("Send frame %3"PRId64"\n", t->frame->pts);
		int ret = avcodec_send_frame(t->ctx, t->frame);
		if (ret < 0) {
			fprintf(stderr, "Error sending a frame for encoding\n");
			//exit(1);
		}

		if (-1 == ioctl(t->camera_fd, VIDIOC_QBUF, &buf))
			t->errno_exit("VIDIOC_QBUF");
	}
	return NULL;
}
