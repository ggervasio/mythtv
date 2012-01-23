// Qt
#include <QString>

// MythTV
#include <mythcorecontext.h>

#include "ratingsettings.h"

RatingSettings::RatingSettings(MythScreenStack *parent, const char *name)
        : MythScreenType(parent, name),
        m_ratingWeight(NULL),
        m_playCountWeight(NULL),
        m_lastPlayWeight(NULL),
        m_randomWeight(NULL),
        m_saveButton(NULL),
        m_cancelButton(NULL)
{
}

RatingSettings::~RatingSettings()
{

}

bool RatingSettings::Create()
{
    bool err = false;

    // Load the theme for this screen
    if (!LoadWindowFromXML("musicsettings-ui.xml", "ratingsettings", this))
        return false;

    m_ratingWeight = dynamic_cast<MythUISpinBox *> (GetChild("ratingweight"));
    m_playCountWeight = dynamic_cast<MythUISpinBox *> (GetChild("playcountweight"));
    m_lastPlayWeight = dynamic_cast<MythUISpinBox *> (GetChild("lastplayweight"));
    m_randomWeight = dynamic_cast<MythUISpinBox *> (GetChild("randomweight"));
    m_helpText = dynamic_cast<MythUIText *> (GetChild("helptext"));
    m_saveButton = dynamic_cast<MythUIButton *> (GetChild("save"));
    m_cancelButton = dynamic_cast<MythUIButton *> (GetChild("cancel"));

    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, "Cannot load screen 'ratingsettings'");
        return false;
    }

    m_ratingWeight->SetRange(0,100,1);
    m_ratingWeight->SetValue(gCoreContext->GetNumSetting("IntelliRatingWeight"));
    m_playCountWeight->SetRange(0,100,1);
    m_playCountWeight->SetValue(gCoreContext->GetNumSetting("IntelliPlayCountWeight"));
    m_lastPlayWeight->SetRange(0,100,1);
    m_lastPlayWeight->SetValue(gCoreContext->GetNumSetting("IntelliLastPlayWeight"));
    m_randomWeight->SetRange(0,100,1);
    m_randomWeight->SetValue(gCoreContext->GetNumSetting("IntelliRandomWeight"));

    connect(m_ratingWeight,  SIGNAL(TakingFocus()), SLOT(slotFocusChanged()));
    connect(m_playCountWeight,  SIGNAL(TakingFocus()), SLOT(slotFocusChanged()));
    connect(m_lastPlayWeight,  SIGNAL(TakingFocus()), SLOT(slotFocusChanged()));
    connect(m_randomWeight,  SIGNAL(TakingFocus()), SLOT(slotFocusChanged()));
    connect(m_saveButton, SIGNAL(Clicked()), this, SLOT(slotSave()));
    connect(m_cancelButton, SIGNAL(Clicked()), this, SLOT(Close()));

    BuildFocusList();

    return true;
}

void RatingSettings::slotSave(void)
{
    gCoreContext->SaveSetting("IntelliRatingWeight", m_ratingWeight->GetValue());
    gCoreContext->SaveSetting("IntelliPlayCountWeight", m_playCountWeight->GetValue());
    gCoreContext->SaveSetting("IntelliLastPlayWeight", m_lastPlayWeight->GetValue());
    gCoreContext->SaveSetting("IntelliRandomWeight", m_randomWeight->GetValue());

    Close();
}

void RatingSettings::slotFocusChanged(void)
{
    if (!m_helpText)
        return;

    QString msg = "";
    if (GetFocusWidget() == m_ratingWeight)
        msg = tr("Used in \"Smart\" Shuffle mode. "
                 "This weighting affects how much strength is "
                 "given to your rating of a given track when "
                 "ordering a group of songs.");
    else if (GetFocusWidget() == m_playCountWeight)
        msg = tr("Used in \"Smart\" Shuffle mode. "
                 "This weighting affects how much strength is "
                 "given to how many times a given track has been "
                 "played when ordering a group of songs.");
    else if (GetFocusWidget() == m_lastPlayWeight)
        msg = tr("Used in \"Smart\" Shuffle mode. "
                 "This weighting affects how much strength is "
                 "given to how long it has been since a given "
                 "track was played when ordering a group of songs.");
    else if (GetFocusWidget() == m_randomWeight)
        msg = tr("Used in \"Smart\" Shuffle mode. "
                 "This weighting affects how much strength is "
                 "given to good old (peudo-)randomness "
                 "when ordering a group of songs.");
    else if (GetFocusWidget() == m_cancelButton)
        msg = tr("Exit without saving settings");
    else if (GetFocusWidget() == m_saveButton)
        msg = tr("Save settings and Exit");

    m_helpText->SetText(msg);
}

