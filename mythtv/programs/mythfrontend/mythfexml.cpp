//////////////////////////////////////////////////////////////////////////////
// Program Name: MythFE.cpp
//                                                                            
// Purpose - Frontend Html & XML status HttpServerExtension
//                                                                            
//////////////////////////////////////////////////////////////////////////////

#include "mythfexml.h"

#include "mythcorecontext.h"
#include "util.h"
#include "mythdbcon.h"

#include "mythmainwindow.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <QRegExp>
#include <QBuffer>

#include "../../config.h"

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

MythFEXML::MythFEXML( UPnpDevice *pDevice , const QString sSharePath)
  : Eventing( "MythFEXML", "MYTHTV_Event", sSharePath)
{

    QString sUPnpDescPath = UPnp::g_pConfig->GetValue( "UPnP/DescXmlPath", m_sSharePath );

    m_sServiceDescFileName = sUPnpDescPath + "MFEXML_scpd.xml";
    m_sControlUrl          = "/MythFE";

    // Add our Service Definition to the device.

    RegisterService( pDevice );
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

MythFEXML::~MythFEXML()
{
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

MythFEXMLMethod MythFEXML::GetMethod( const QString &sURI )
{
    if (sURI == "GetScreenShot") return MFEXML_GetScreenShot;
    if (sURI == "Message")       return MFEXML_Message;

    return( MFEXML_Unknown );
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

bool MythFEXML::ProcessRequest( HttpWorkerThread *pThread, HTTPRequest *pRequest )
{
    try
    {
        if (pRequest)
        {
            if (pRequest->m_sBaseUrl != m_sControlUrl)
                return( false );

            VERBOSE(VB_UPNP, QString("MythFEXML::ProcessRequest: %1 : %2")
                         .arg(pRequest->m_sMethod)
                     .arg(pRequest->m_sRawRequest));

            switch( GetMethod( pRequest->m_sMethod ))
            {
                case MFEXML_GetScreenShot      : GetScreenShot    ( pRequest ); return true;
                case MFEXML_Message            : SendMessage      ( pRequest ); return true;


                default: 
                {
                    UPnp::FormatErrorResponse( pRequest, UPnPResult_InvalidAction );

                    return true;
                }
            }
        }
    }
    catch( ... )
    {
        VERBOSE( VB_IMPORTANT, "MythFEXML::ProcessRequest() - Unexpected Exception" );
    }

    return( false );
}           

// ==========================================================================
// Request handler Methods
// ==========================================================================

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

void MythFEXML::GetScreenShot( HTTPRequest *pRequest )
{
    pRequest->m_eResponseType   = ResponseTypeFile;

    // Optional Parameters

    int     nWidth    = pRequest->m_mapParams[ "width"     ].toInt();
    int     nHeight   = pRequest->m_mapParams[ "height"    ].toInt();
    QString sFormat   = pRequest->m_mapParams[ "format"    ].toLower();

    if (sFormat.isEmpty())
    {
        sFormat = "png";
    }

    if (sFormat != "jpg" && sFormat != "png" && sFormat != "gif") {
        VERBOSE(VB_GENERAL, QString("Invalid screen shot format: %1") 
            .arg(sFormat));
        return;
    }
   
    VERBOSE(VB_GENERAL, QString("Screen shot requested - %1") .arg(sFormat));

    QString sFileName = QString("/%1/myth-screenshot-XML.%2")
                    .arg(gCoreContext->GetSetting("ScreenShotPath","/tmp/"))
                    .arg(sFormat);

    MythMainWindow *window = GetMythMainWindow();
    emit window->remoteScreenShot(sFileName, nWidth, nHeight);

    pRequest->m_sFileName = sFileName;
}

void MythFEXML::SendMessage( HTTPRequest *pRequest )
{
    pRequest->m_eResponseType = ResponseTypeNone;
    QString sText = pRequest->m_mapParams[ "text" ];
    VERBOSE(VB_GENERAL, QString("UPNP message: ") + sText);

    MythMainWindow *window = GetMythMainWindow();
    MythEvent* me = new MythEvent(MythEvent::MythUserMessage, sText);
    qApp->postEvent(window, me);
}
