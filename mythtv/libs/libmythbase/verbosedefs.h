#ifndef VERBOSEDEFS_H_
#define VERBOSEDEFS_H_

#include <stdint.h>

/// This file gets included in two different ways:
/// 1) from mythlogging.h from nearly every file.  This will define the 
///    VerboseMask enum
/// 2) specifically (and a second include with _IMPLEMENT_VERBOSE defined) from
///    mythlogging.cpp.  This is done in verboseInit (in the middle of the
///    function) as it will expand out to a series of calls to verboseAdd()
///    to fill the verboseMap.
///
/// The 4 fields are:
///     enum name (expected to start with VB_)
///     enum value (will be used as a 64-bit unsigned int)
///     additive flag (explicit = false, additive = true)
///     help text for "-v help"
///
/// To create a new VB_* flag, this is the only piece of code you need to
/// modify, then you can start using the new flag and it will automatically be
/// processed by the verboseArgParse() function and help info printed when
/// "-v help" is used.

#undef VERBOSE_PREAMBLE
#undef VERBOSE_POSTAMBLE
#undef VERBOSE_MAP

#ifdef _IMPLEMENT_VERBOSE

// This is used to actually implement the mask in mythlogging.cpp
#define VERBOSE_PREAMBLE
#define VERBOSE_POSTAMBLE
#define VERBOSE_MAP(name,mask,additive,help) \
    verboseAdd((uint64_t)(mask),QString(#name),(bool)(additive),QString(help));

#else // !defined(_IMPLEMENT_VERBOSE)

// This is used to define the enumerated type (used by all files)
#define VERBOSE_PREAMBLE \
    enum VerboseMask {
#define VERBOSE_POSTAMBLE \
        VB_LAST_ITEM \
    };
#define VERBOSE_MAP(name,mask,additive,help) \
    name = (uint64_t)(mask),

#endif

VERBOSE_PREAMBLE
VERBOSE_MAP(VB_ALL,       0xffffffff, false, 
            "ALL available debug output")
VERBOSE_MAP(VB_MOST,      0x3ffeffff, false,
            "Most debug (nodatabase,notimestamp,noextra)")
VERBOSE_MAP(VB_IMPORTANT, 0x00000001, false,
            "Errors or other very important messages")
VERBOSE_MAP(VB_GENERAL,   0x00000002, true,
            "General info")
VERBOSE_MAP(VB_RECORD,    0x00000004, true,
            "Recording related messages")
VERBOSE_MAP(VB_PLAYBACK,  0x00000008, true,
            "Playback related messages")
VERBOSE_MAP(VB_CHANNEL,   0x00000010, true,
            "Channel related messages")
VERBOSE_MAP(VB_OSD,       0x00000020, true,
            "On-Screen Display related messages")
VERBOSE_MAP(VB_FILE,      0x00000040, true,
            "File and AutoExpire related messages")
VERBOSE_MAP(VB_SCHEDULE,  0x00000080, true,
            "Scheduling related messages")
VERBOSE_MAP(VB_NETWORK,   0x00000100, true,
            "Network protocol related messages")
VERBOSE_MAP(VB_COMMFLAG,  0x00000200, true,
            "Commercial detection related messages")
VERBOSE_MAP(VB_AUDIO,     0x00000400, true,
            "Audio related messages")
VERBOSE_MAP(VB_LIBAV,     0x00000800, true,
            "Enables libav debugging")
VERBOSE_MAP(VB_JOBQUEUE,  0x00001000, true,
            "JobQueue related messages")
VERBOSE_MAP(VB_SIPARSER,  0x00002000, true,
            "Siparser related messages")
VERBOSE_MAP(VB_EIT,       0x00004000, true,
            "EIT related messages")
VERBOSE_MAP(VB_VBI,       0x00008000, true,
            "VBI related messages")
VERBOSE_MAP(VB_DATABASE,  0x00010000, true,
            "Display all SQL commands executed")
VERBOSE_MAP(VB_DSMCC,     0x00020000, true,
            "DSMCC carousel related messages")
VERBOSE_MAP(VB_MHEG,      0x00040000, true,
            "MHEG debugging messages")
VERBOSE_MAP(VB_UPNP,      0x00080000, true,
            "UPnP debugging messages")
VERBOSE_MAP(VB_SOCKET,    0x00100000, true,
            "socket debugging messages")
VERBOSE_MAP(VB_XMLTV,     0x00200000, true,
            "xmltv output and related messages")
VERBOSE_MAP(VB_DVBCAM,    0x00400000, true,
            "DVB CAM debugging messages")
VERBOSE_MAP(VB_MEDIA,     0x00800000, true,
            "Media Manager debugging messages")
VERBOSE_MAP(VB_IDLE,      0x01000000, true,
            "System idle messages")
VERBOSE_MAP(VB_CHANSCAN,  0x02000000, true,
            "Channel Scanning messages")
VERBOSE_MAP(VB_GUI,       0x04000000, true,
            "GUI related messages")
VERBOSE_MAP(VB_SYSTEM,    0x08000000, true,
            "External executable related messages")
VERBOSE_MAP(VB_EXTRA,     0x40000000, true,
            "More detailed messages in selected levels")
VERBOSE_MAP(VB_TIMESTAMP, 0x80000000, true,
            "Conditional data driven messages")
VERBOSE_MAP(VB_NONE,      0x00000000, false,
            "NO debug output")
VERBOSE_POSTAMBLE

#endif
