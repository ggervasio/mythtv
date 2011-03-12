//////////////////////////////////////////////////////////////////////////////
// Program Name: myth.cpp
// Created     : Jan. 19, 2010
//
// Copyright (c) 2010 David Blain <dblain@mythtv.org>
//                                          
// This library is free software; you can redistribute it and/or 
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or at your option any later version of the LGPL.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library.  If not, see <http://www.gnu.org/licenses/>.
//
//////////////////////////////////////////////////////////////////////////////

#include "myth.h"

#include <QDir>

#include "mythcorecontext.h"
#include "storagegroup.h"

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

DTC::ConnectionInfo* Myth::GetConnectionInfo( const QString  &sPin )
{
    QString sSecurityPin = gCoreContext->GetSetting( "SecurityPin", "");

    if ( sSecurityPin.isEmpty() )
        throw( QString( "No Security Pin assigned. Run mythtv-setup to set one." ));
        //SB: UPnPResult_HumanInterventionRequired,

    if ((sSecurityPin != "0000" ) && ( sPin != sSecurityPin ))
        throw( QString( "Not Authorized" ));
        //SB: UPnPResult_ActionNotAuthorized );
  
    DatabaseParams params = gCoreContext->GetDatabaseParams();

    // ----------------------------------------------------------------------
    // Check for DBHostName of "localhost" and change to public name or IP
    // ----------------------------------------------------------------------

    QString sServerIP = gCoreContext->GetSetting( "BackendServerIP", "localhost" );
    //QString sPeerIP   = pRequest->GetPeerAddress();

    if ((params.dbHostName == "localhost") &&
        (sServerIP         != "localhost"))  // &&
        //(sServerIP         != sPeerIP    ))
    {
        params.dbHostName = sServerIP;
    }

    // ----------------------------------------------------------------------
    // Create and populate a ConnectionInfo object
    // ----------------------------------------------------------------------

    DTC::ConnectionInfo *pInfo     = new DTC::ConnectionInfo();
    DTC::DatabaseInfo   *pDatabase = pInfo->Database();
    DTC::WOLInfo        *pWOL      = pInfo->WOL();

    pDatabase->setHost         ( params.dbHostName    );
    pDatabase->setPing         ( params.dbHostPing    );
    pDatabase->setPort         ( params.dbPort        );
    pDatabase->setUserName     ( params.dbUserName    );
    pDatabase->setPassword     ( params.dbPassword    );
    pDatabase->setName         ( params.dbName        );
    pDatabase->setType         ( params.dbType        );
    pDatabase->setLocalEnabled ( params.localEnabled  );
    pDatabase->setLocalHostName( params.localHostName );

    pWOL->setEnabled           ( params.wolEnabled   );
    pWOL->setReconnect         ( params.wolReconnect );
    pWOL->setRetry             ( params.wolRetry     );
    pWOL->setCommand           ( params.wolCommand   );

    // ----------------------------------------------------------------------
    // Return the pointer... caller is responsible to delete it!!!
    // ----------------------------------------------------------------------

    return pInfo;
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

DTC::StringList* Myth::GetHosts( )
{
    MSqlQuery query(MSqlQuery::InitCon());

    if (!query.isConnected())
        throw( QString( "Database not open while trying to load list of hosts" ));

    query.prepare(
        "SELECT DISTINCTROW hostname "
        "FROM settings "
        "WHERE (not isNull( hostname ))");

    if (!query.exec())
    {
        MythDB::DBError("MythAPI::GetHosts()", query);

        throw( QString( "Database Error executing query." ));
    }

    // ----------------------------------------------------------------------
    // return the results of the query
    // ----------------------------------------------------------------------

    DTC::StringList *pResults = new DTC::StringList();

    //pResults->setObjectName( "HostList" );

    while (query.next())
        pResults->Values().append( query.value(0).toString() );

    return pResults;
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

DTC::StringList *Myth::GetKeys(  ) 
{
    MSqlQuery query(MSqlQuery::InitCon());

    if (!query.isConnected())
        throw( QString("Database not open while trying to load settings"));

    query.prepare("SELECT DISTINCTROW value FROM settings;" );

    if (!query.exec())
    {
        MythDB::DBError("MythAPI::GetKeys()", query);

        throw( QString( "Database Error executing query." ));
    }

    // ----------------------------------------------------------------------
    // return the results of the query
    // ----------------------------------------------------------------------

    DTC::StringList *pResults = new DTC::StringList();

    //pResults->setObjectName( "KeyList" );

    while (query.next())
        pResults->Values().append( query.value(0).toString() );

    return pResults;
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

DTC::SettingList *Myth::GetSetting( const QString &sHostName, 
                                    const QString &sKey, 
                                    const QString &sDefault )
{

    MSqlQuery query(MSqlQuery::InitCon());

    if (!query.isConnected())
    {
        throw( QString("Database not open while trying to load setting: %1")
                  .arg( sKey ));
    }

    // ----------------------------------------------------------------------

    DTC::SettingList *pList = new DTC::SettingList();

    //pList->setObjectName( "Settings" );
    pList->setHostName  ( sHostName  );

    // ----------------------------------------------------------------------
    // Format Results
    // ----------------------------------------------------------------------

    if (!sKey.isEmpty())
    {
        // ------------------------------------------------------------------
        // Key Supplied, lookup just its value.
        // ------------------------------------------------------------------

        query.prepare("SELECT data, hostname from settings "
                      "WHERE value = :KEY AND "
                      "(hostname = :HOSTNAME OR hostname IS NULL) "
                      "ORDER BY hostname DESC;" );

        query.bindValue(":KEY"     , sKey      );
        query.bindValue(":HOSTNAME", sHostName );

        if (!query.exec())
        {
            // clean up unused object we created.

            delete pList;

            MythDB::DBError("MythAPI::GetSetting() w/key ", query);

            throw( QString( "Database Error executing query." ));
        }

        if (query.next())
        {
            if ( (sHostName.isEmpty()) ||
                 ((!sHostName.isEmpty()) &&
                  (sHostName == query.value(1).toString())))
            {
                pList->setHostName( query.value(1).toString() );

                pList->Settings().insert( sKey, query.value(0) );
            }
        }
    }
    else
    {
        // ------------------------------------------------------------------
        // Looking to return all Setting for supplied hostname 
        // ------------------------------------------------------------------

        if (sHostName.isEmpty())
        {
            query.prepare("SELECT value, data FROM settings "
                          "WHERE (hostname IS NULL)" );
        }
        else
        {
            query.prepare("SELECT value, data FROM settings "
                          "WHERE (hostname = :HOSTNAME)" );

            query.bindValue(":HOSTNAME", sHostName );
        }

        if (!query.exec())
        {
            // clean up unused object we created.

            delete pList;

            MythDB::DBError("MythAPI::GetSetting() w/o key ", query);
            throw( QString( "Database Error executing query." ));
        }

        while (query.next())
            pList->Settings().insert( query.value(0).toString(), query.value(1) );
    }

    // ----------------------------------------------------------------------
    // Not found, so return the supplied default value
    // ----------------------------------------------------------------------

    if (pList->Settings().count() == 0)
        pList->Settings().insert( sKey, sDefault );

    return pList;
}

/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////

DTC::SuccessFail *Myth::PutSetting( const QString &sHostName, 
                                    const QString &sKey, 
                                    const QString &sValue ) 

{
    if (!sKey.isEmpty())
    {
        DTC::SuccessFail *pResults = new DTC::SuccessFail();

        if ( gCoreContext->SaveSettingOnHost( sKey, sValue, sHostName ) )
            pResults->setResult( true );
        else
            pResults->setResult( false );

        return pResults;
    }

    throw ( QString( "Key Required" ));
}

