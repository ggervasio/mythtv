#ifndef _UTIL_OSX_COCOA_H__
#define _UTIL_OSX_COCOA_H__

#import "ApplicationServices/ApplicationServices.h"
#include "mythexp.h"

MPUBLIC void *CreateOSXCocoaPool(void);
MPUBLIC void DeleteOSXCocoaPool(void*&);

CGDirectDisplayID GetOSXCocoaDisplay(void* view);

class CocoaAutoReleasePool
{
  public:
    CocoaAutoReleasePool() { m_pool = CreateOSXCocoaPool(); }
   ~CocoaAutoReleasePool() { DeleteOSXCocoaPool(m_pool);    }
    void *m_pool;
};

#endif // _UTIL_OSX_COCOA_H__
