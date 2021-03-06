#ifndef LOOKUP_H_
#define LOOKUP_H_

#include <QObject>
#include <QList>

#include "programinfo.h"
#include "metadatafactory.h"

class LookerUpper : public QObject
{
  public:
    LookerUpper();
    ~LookerUpper();

    bool StillWorking();

    void HandleSingleRecording(const uint chanid,
                               const QDateTime starttime);
    void HandleAllRecordings();

  private:
    void customEvent(QEvent *event);

    MetadataFactory      *m_metadataFactory;

    QList<ProgramInfo*>   m_busyRecList;
};

#endif //LOOKUP_H_
