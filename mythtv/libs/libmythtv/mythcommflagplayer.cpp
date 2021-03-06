#include <QThreadPool>
#include "mythcommflagplayer.h"
#include "mythlogging.h"

#define LOC QString("CommFlagPlayer: ")

class RebuildSaver : public QRunnable
{
  public:
    RebuildSaver(DecoderBase *d, uint64_t f, uint64_t l)
        : m_decoder(d), m_first(f), m_last(l)
    {
        QMutexLocker locker(&s_lock);
        s_cnt[d]++;
    }

    virtual void run(void)
    {
	threadRegister("RebuildSaver");
        m_decoder->SavePositionMapDelta(m_first, m_last);

        QMutexLocker locker(&s_lock);
        s_cnt[m_decoder]--;
        if (!s_cnt[m_decoder])
            s_wait.wakeAll();
	threadDeregister();
    }

    static uint GetCount(DecoderBase *d)
    {
        QMutexLocker locker(&s_lock);
        return s_cnt[d];
    }

    static void Wait(DecoderBase *d)
    {
        QMutexLocker locker(&s_lock);
        if (!s_cnt[d])
            return;
        while (s_wait.wait(&s_lock))
        {
            if (!s_cnt[d])
                return;
        }
    }

  private:
    DecoderBase *m_decoder;
    uint64_t     m_first;
    uint64_t     m_last;

    static QMutex                  s_lock;
    static QWaitCondition          s_wait;
    static QMap<DecoderBase*,uint> s_cnt;
};
QMutex                  RebuildSaver::s_lock;
QWaitCondition          RebuildSaver::s_wait;
QMap<DecoderBase*,uint> RebuildSaver::s_cnt;

bool MythCommFlagPlayer::RebuildSeekTable(
    bool showPercentage, StatusCallback cb, void* cbData)
{
    int percentage = 0;
    uint64_t myFramesPlayed = 0, pmap_first = 0,  pmap_last  = 0;

    killdecoder = false;
    framesPlayed = 0;
    using_null_videoout = true;

    // clear out any existing seektables
    player_ctx->LockPlayingInfo(__FILE__, __LINE__);
    if (player_ctx->playingInfo)
    {
        player_ctx->playingInfo->ClearPositionMap(MARK_KEYFRAME);
        player_ctx->playingInfo->ClearPositionMap(MARK_GOP_START);
        player_ctx->playingInfo->ClearPositionMap(MARK_GOP_BYFRAME);
    }
    player_ctx->UnlockPlayingInfo(__FILE__, __LINE__);

    if (OpenFile() < 0)
        return false;

    SetPlaying(true);

    if (!InitVideo())
    {
        LOG(VB_GENERAL, LOG_ERR,
            LOC + "RebuildSeekTable unable to initialize video");
        SetPlaying(false);
        return false;
    }

    ClearAfterSeek();

    int save_timeout = 1001;
    MythTimer flagTime, ui_timer, inuse_timer, save_timer;
    flagTime.start();
    ui_timer.start();
    inuse_timer.start();
    save_timer.start();

    DecoderGetFrame(kDecodeNothing,true);

    if (showPercentage)
    {
        if (totalFrames)
            printf("             ");
        else
            printf("      ");
        fflush( stdout );
    }

    int prevperc = -1;
    while (!GetEof())
    {
        if (inuse_timer.elapsed() > 2534)
        {
            inuse_timer.restart();
            player_ctx->LockPlayingInfo(__FILE__, __LINE__);
            if (player_ctx->playingInfo)
                player_ctx->playingInfo->UpdateInUseMark();
            player_ctx->UnlockPlayingInfo(__FILE__, __LINE__);
        }

        if (save_timer.elapsed() > save_timeout)
        {
            // Give DB some breathing room if it gets far behind..
            if (myFramesPlayed - pmap_last > 5000)
                usleep(200 * 1000);

            // If we're already saving, just save a larger block next time..
            if (RebuildSaver::GetCount(decoder) < 1)
            {
                pmap_last = myFramesPlayed;
                QThreadPool::globalInstance()->start(
                    new RebuildSaver(decoder, pmap_first, pmap_last));
                pmap_first = pmap_last + 1;
            }

            save_timer.restart();
        }

        if (ui_timer.elapsed() > 98)
        {
            ui_timer.restart();

            if (totalFrames)
            {
                float elapsed = flagTime.elapsed() * 0.001f;
                int flagFPS = (elapsed > 0.0f) ?
                    (int)(myFramesPlayed / elapsed) : 0;

                percentage = myFramesPlayed * 100 / totalFrames;
                if (cb)
                    (*cb)(percentage, cbData);

                if (showPercentage)
                {
                    printf( "\b\b\b\b\b\b\b\b\b\b\b\b\b" );
                    printf( "%3d%%/%5dfps", percentage, flagFPS );
                    fflush( stdout );
                }
		if (percentage % 10 == 0 && prevperc != percentage)
                {
                    prevperc = percentage;
		    LOG(VB_GENERAL, LOG_INFO, QString("%1%/%2""fps")
                                        .arg(percentage).arg(flagFPS));
                }
            }
            else 
            {
                if (showPercentage)
                {
                    printf( "\b\b\b\b\b\b" );
                    printf( "%6lld", (long long)myFramesPlayed );
                    fflush( stdout );
                }
                if (myFramesPlayed % 1000 == 0)
                    LOG(VB_GENERAL, LOG_INFO, QString("%1")
                            .arg(myFramesPlayed));
            }
        }

        if (DecoderGetFrame(kDecodeNothing,true))
            myFramesPlayed = decoder->GetFramesRead();
    }

    if (showPercentage)
    {
        if (totalFrames)
            printf( "\b\b\b\b\b\b\b" );
        printf( "\b\b\b\b\b\b           \b\b\b\b\b\b\b\b\b\b\b" );
        fflush( stdout );
    }

    SaveTotalDuration();

    SetPlaying(false);
    killdecoder = true;

    QThreadPool::globalInstance()->start(
        new RebuildSaver(decoder, pmap_first, myFramesPlayed));
    RebuildSaver::Wait(decoder);

    return true;
}
