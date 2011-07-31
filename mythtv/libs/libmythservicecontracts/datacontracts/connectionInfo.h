//////////////////////////////////////////////////////////////////////////////
// Program Name: connectionInfo.h
// Created     : Jan. 15, 2010
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

#ifndef CONNECTIONINFO_H_
#define CONNECTIONINFO_H_

#include "serviceexp.h" 
#include "datacontracthelper.h"

#include "versionInfo.h"
#include "databaseInfo.h"
#include "wolInfo.h"

namespace DTC
{

class SERVICE_PUBLIC ConnectionInfo : public QObject
{
    Q_OBJECT

    Q_CLASSINFO( "version"    , "1.1" );

    Q_PROPERTY( QObject*    Version     READ Version    )
    Q_PROPERTY( QObject*    Database    READ Database   )
    Q_PROPERTY( QObject*    WOL         READ WOL        )

    PROPERTYIMP_PTR( VersionInfo , Version    )
    PROPERTYIMP_PTR( DatabaseInfo, Database   )
    PROPERTYIMP_PTR( WOLInfo     , WOL        )

    public:

        static void InitializeCustomTypes()
        {
            qRegisterMetaType< ConnectionInfo   >();
            qRegisterMetaType< ConnectionInfo*  >();

            VersionInfo ::InitializeCustomTypes();
            DatabaseInfo::InitializeCustomTypes();
            WOLInfo     ::InitializeCustomTypes();
        }

    public:

        ConnectionInfo(QObject *parent = 0) 
            : QObject        ( parent ),
              m_Version      ( NULL   ),
              m_Database     ( NULL   ),
              m_WOL          ( NULL   )             
        {
        }
        
        ConnectionInfo( const ConnectionInfo &src ) 
            : m_Version   ( NULL ),
              m_Database  ( NULL ),
              m_WOL       ( NULL )                  
        {
            Copy( src );
        }

        void Copy( const ConnectionInfo &src ) 
        {
            // We always need to make sure the child object is
            // created with the correct parent *

            if (src.m_Version)
                Version()->Copy( src.m_Version );

            if (src.m_Database)
                Database()->Copy( src.m_Database );

            if (src.m_WOL)
                WOL     ()->Copy( src.m_WOL      );
        }
};

typedef ConnectionInfo* ConnectionInfoPtr;

} // namespace DTC

Q_DECLARE_METATYPE( DTC::ConnectionInfo  )
Q_DECLARE_METATYPE( DTC::ConnectionInfo* )

#endif
