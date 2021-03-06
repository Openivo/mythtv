#include <map>
#include <iostream>
using namespace std;

#include "mythlogging.h"
#include "videoout_null.h"
#include "videodisplayprofile.h"

const int kNumBuffers = 31;
const int kNeedFreeFrames = 1;
const int kPrebufferFramesNormal = 12;
const int kPrebufferFramesSmall = 4;
const int kKeepPrebuffer = 2;

void VideoOutputNull::GetRenderOptions(render_opts &opts,
                                       QStringList &cpudeints)
{
    opts.renderers->append("null");
    opts.deints->insert("null", cpudeints);
    (*opts.osds)["null"].append("softblend");
    (*opts.safe_renderers)["dummy"].append("null");
    (*opts.safe_renderers)["nuppel"].append("null");
    if (opts.decoders->contains("ffmpeg"))
        (*opts.safe_renderers)["ffmpeg"].append("null");
    if (opts.decoders->contains("crystalhd"))
        (*opts.safe_renderers)["crystalhd"].append("null");

    opts.priorities->insert("null", 10);
}

VideoOutputNull::VideoOutputNull() :
    VideoOutput(), global_lock(QMutex::Recursive)
{
    LOG(VB_PLAYBACK, LOG_INFO, "VideoOutputNull()");
    memset(&av_pause_frame, 0, sizeof(av_pause_frame));
}

VideoOutputNull::~VideoOutputNull()
{
    LOG(VB_PLAYBACK, LOG_INFO, "~VideoOutputNull()");
    QMutexLocker locker(&global_lock);

    if (av_pause_frame.buf)
    {
        delete [] av_pause_frame.buf;
        memset(&av_pause_frame, 0, sizeof(av_pause_frame));
    }

    vbuffers.DeleteBuffers();
}

// this is documented in videooutbase.cpp
void VideoOutputNull::Zoom(ZoomDirection direction)
{
    QMutexLocker locker(&global_lock);
    VideoOutput::Zoom(direction);
    MoveResize();
}

void VideoOutputNull::CreatePauseFrame(void)
{
    if (av_pause_frame.buf)
    {
        delete [] av_pause_frame.buf;
        av_pause_frame.buf = NULL;
    }

    init(&av_pause_frame, FMT_YV12,
         new unsigned char[vbuffers.GetScratchFrame()->size + 128],
         vbuffers.GetScratchFrame()->width,
         vbuffers.GetScratchFrame()->height,
         vbuffers.GetScratchFrame()->size);

    av_pause_frame.frameNumber = vbuffers.GetScratchFrame()->frameNumber;

    clear(&av_pause_frame);
}

bool VideoOutputNull::InputChanged(const QSize &input_size,
                                   float        aspect,
                                   MythCodecID  av_codec_id,
                                   void        *codec_private,
                                   bool        &aspect_only)
{
    LOG(VB_PLAYBACK, LOG_INFO,
        QString("InputChanged(WxH = %1x%2, aspect = %3)")
            .arg(input_size.width())
            .arg(input_size.height()).arg(aspect));

    if (!codec_is_std(av_codec_id))
    {
        LOG(VB_GENERAL, LOG_ERR, QString("VideoOutputNull::InputChanged(): "
                                         "new video codec is not supported."));
        errorState = kError_Unknown;
        return false;
    }

    QMutexLocker locker(&global_lock);

    if (input_size == window.GetActualVideoDim())
    {
        vbuffers.Clear();
        MoveResize();
        return true;
    }

    VideoOutput::InputChanged(input_size, aspect, av_codec_id, codec_private,
                              aspect_only);
    vbuffers.DeleteBuffers();

    MoveResize();

    const QSize video_dim = window.GetVideoDim();

    bool ok = vbuffers.CreateBuffers(FMT_YV12, video_dim.width(),
                                     video_dim.height());
    if (!ok)
    {
        LOG(VB_GENERAL, LOG_ERR, "VideoOutputNull::InputChanged(): "
                                 "Failed to recreate buffers");
        errorState = kError_Unknown;
    }
    else
    {
        CreatePauseFrame();
    }

    if (db_vdisp_profile)
        db_vdisp_profile->SetVideoRenderer("null");

    return ok;
}

bool VideoOutputNull::Init(int width, int height, float aspect, WId winid,
                           const QRect &win_rect, MythCodecID codec_id)
{
    if ((width <= 0) || (height <= 0))
        return false;

    QMutexLocker locker(&global_lock);

    VideoOutput::Init(width, height, aspect, winid, win_rect, codec_id);

    vbuffers.Init(kNumBuffers, true, kNeedFreeFrames,
                  kPrebufferFramesNormal, kPrebufferFramesSmall,
                  kKeepPrebuffer);

    const QSize video_dim = window.GetVideoDim();

    if (!vbuffers.CreateBuffers(FMT_YV12, video_dim.width(), video_dim.height()))
        return false;

    CreatePauseFrame();

    if (db_vdisp_profile)
        db_vdisp_profile->SetVideoRenderer("null");

    MoveResize();

    return true;
}

void VideoOutputNull::EmbedInWidget(const QRect &rect)
{
    QMutexLocker locker(&global_lock);
    if (!window.IsEmbedding())
        VideoOutput::EmbedInWidget(rect);
}

void VideoOutputNull::StopEmbedding(void)
{
    QMutexLocker locker(&global_lock);
    if (window.IsEmbedding())
        VideoOutput::StopEmbedding();
}

void VideoOutputNull::PrepareFrame(VideoFrame *buffer, FrameScanType t,
                                   OSD *osd)
{
    (void)osd;
    (void)t;

    if (!buffer)
        buffer = vbuffers.GetScratchFrame();

    framesPlayed = buffer->frameNumber + 1;
}

void VideoOutputNull::Show(FrameScanType )
{
}

void VideoOutputNull::DrawUnusedRects(bool)
{
}

void VideoOutputNull::UpdatePauseFrame(void)
{
    QMutexLocker locker(&global_lock);

    // Try used frame first, then fall back to scratch frame.
    vbuffers.begin_lock(kVideoBuffer_used);
    VideoFrame *used_frame = NULL;
    if (vbuffers.size(kVideoBuffer_used) > 0)
        used_frame = vbuffers.head(kVideoBuffer_used);

    if (used_frame)
        CopyFrame(&av_pause_frame, used_frame);
    vbuffers.end_lock();

    if (!used_frame)
    {
        vbuffers.GetScratchFrame()->frameNumber = framesPlayed - 1;
        CopyFrame(&av_pause_frame, vbuffers.GetScratchFrame());
    }
}

void VideoOutputNull::ProcessFrame(VideoFrame *frame, OSD *osd,
                                   FilterChain *filterList,
                                   const PIPMap &pipPlayers,
                                   FrameScanType scan)
{
    (void)frame;
    (void)osd;
    (void)filterList;
    (void)pipPlayers;
    (void)scan;
}
