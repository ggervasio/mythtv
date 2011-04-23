//////////////////////////////////////////////////////////////////////////////
// Program Name: contentServices.h
// Created     : Apr. 21, 2011
//
// Purpose - Imported Video Services API Interface definition
//
// Copyright (c) 2011 Robert McNamara <rmcnamara@mythtv.org>
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

#ifndef VIDEOSERVICES_H_
#define VIDEOSERVICES_H_

#include <QFileInfo>
#include <QStringList>

#include "service.h"

#include "datacontracts/videoMetadataInfoList.h"
#include "datacontracts/blurayInfo.h"

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
//
// Notes -
//
//  * This implementation can't handle declared default parameters
//
//  * When called, any missing params are sent default values for its datatype
//
//  * Q_CLASSINFO( "<methodName>_Method", ...) is used to determine HTTP method
//    type.  Defaults to "BOTH", available values:
//          "GET", "POST" or "BOTH"
//
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

class SERVICE_PUBLIC VideoServices : public Service  //, public QScriptable ???
{
    Q_OBJECT
    Q_CLASSINFO( "version"    , "1.02" );
    Q_CLASSINFO( "RemoveVideoFromDB_Method",           "POST" )
    Q_CLASSINFO( "AddVideo_Method",                    "POST" )

    public:

        // Must call InitializeCustomTypes for each unique Custom Type used
        // in public slots below.

        VideoServices( QObject *parent = 0 ) : Service( parent )
        {
            DTC::VideoMetadataInfoList::InitializeCustomTypes();
            DTC::BlurayInfo::InitializeCustomTypes();
        }

    public slots:

        // Video Metadata

        virtual DTC::VideoMetadataInfoList* GetVideos          ( bool             Descending,
                                                                 int              StartIndex,
                                                                 int              Count      ) = 0;

        virtual DTC::VideoMetadataInfo*     GetVideoById       ( int              Id         ) = 0;

        virtual DTC::VideoMetadataInfo*     GetVideoByFilename ( const QString    &Filename  ) = 0;

        virtual bool                        RemoveVideoFromDB  ( int              Id         ) = 0;

        virtual bool                        AddVideo           ( const QString    &Filename,
                                                                 const QString    &Host      ) = 0;

        // Bluray Metadata

        virtual DTC::BlurayInfo*            GetBluray          ( const QString    &Path      ) = 0;
};

#endif

