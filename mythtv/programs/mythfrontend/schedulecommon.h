#ifndef SCHEDULE_COMMON_H
#define SCHEDULE_COMMON_H

// QT
#include <QObject>
#include <QEvent>

// libmythui
#include "mythscreentype.h"
#include "mythmainwindow.h"

class ProgramInfo;
class RecordingInfo;

class ScheduleCommon : public MythScreenType
{
    Q_OBJECT

  protected:
    ScheduleCommon(MythScreenStack *parent, const QString &name)
        : MythScreenType(parent, name) {};
   ~ScheduleCommon() {};

    void ShowDetails(ProgramInfo *pginfo) const;
    void ShowUpcoming(const QString &title, const QString &seriesid) const;
    void ShowUpcoming(ProgramInfo *pginfo) const;
    void ShowUpcomingScheduled(ProgramInfo *pginfo) const;
    void QuickRecord(ProgramInfo *pginfo);
    void EditRecording(ProgramInfo *pginfo);
    void EditScheduled(ProgramInfo *pginfo);
    void EditScheduled(RecordingInfo *recinfo);
    void EditCustom(ProgramInfo *pginfo);
    void MakeOverride(RecordingInfo *recinfo, bool startActive = false);
    void ShowRecordingDialog(const RecordingInfo &recinfo);
    void ShowNotRecordingDialog(const RecordingInfo &recinfo);
    void ShowNotScheduledDialog(const RecordingInfo &recinfo);

    virtual void customEvent(QEvent*);

  private:
    bool IsFindApplicable(const RecordingInfo &recInfo) const;

};

#endif
