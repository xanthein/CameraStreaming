#ifndef _H264_VIDEO_CAMERA_MEDIA_SUBSESSION_HH
#define _H264_VIDEO_CAMERA_MEDIA_SUBSESSION_HH

#ifndef _ON_DEMAND_SERVER_MEDIA_SUBSESSION_HH
#include "OnDemandServerMediaSubsession.hh"
#endif

class H264CameraMediaSubsession: public OnDemandServerMediaSubsession {
public:
	static H264CameraMediaSubsession*
	createNew(UsageEnvironment& env, Boolean reuseFirstSource);

	// Used to implement "getAuxSDPLine()":
	void checkForAuxSDPLine1();
	void afterPlayingDummy1();
	RTPSink* fDummyRTPSink;
	FramedSource *DummyFramedSource;

protected:
	H264CameraMediaSubsession(UsageEnvironment& env,
					Boolean reuseFirstSource);
	// called only by createNew();
	virtual ~H264CameraMediaSubsession();

	void setDoneFlag() { fDoneFlag = ~0; }

protected: // redefined virtual functions
	virtual char const* getAuxSDPLine(RTPSink* rtpSink,
						FramedSource* inputSource);
	virtual FramedSource* createNewStreamSource(unsigned clientSessionId,
						unsigned& estBitrate);
	virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
						unsigned char rtpPayloadTypeIfDynamic,
						FramedSource* inputSource);

private:
	char* fAuxSDPLine;
	char fDoneFlag; // used when setting up "fAuxSDPLine"
};

#endif
