//////////////////////////////////////////////////////////////////////////////
// Program Name: channel.h
// Created     : Apr. 8, 2011
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

#ifndef CHANNEL_H
#define CHANNEL_H

#include <QScriptEngine>

#include "services/channelServices.h"

class Channel : public ChannelServices
{
    Q_OBJECT

    public:

        Q_INVOKABLE Channel( QObject *parent = 0 ) {}

    public:

//        QList<uint>  GetChannels           ( int    SourceID );

//        QList<uint>  GetCardIDs            ( uint   ChanID   );
        QString      GetIcon               ( uint   ChanID   );
        uint         GetMplexID            ( uint   ChanID   );
        QString      GetDefaultAuthority   ( uint   ChanID   );
        uint         GetSourceIDForChannel ( uint   ChanID   );
};

#endif
