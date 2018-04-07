#ifndef __DSS_SLOTTEDSESSIONCHANGEDHANDLER_H__
#define __DSS_SLOTTEDSESSIONCHANGEDHANDLER_H__

/*====================================================

FILE:  DSS_SlottedSessionChangedHandler.h

SERVICES:
   Handle Slotted Session Changed event.

=====================================================

Copyright (c) 2008 Qualcomm Technologies, Inc.
All Rights Reserved.
Qualcomm Technologies Confidential and Proprietary

=====================================================*/
/*===========================================================================
  EDIT HISTORY FOR MODULE

  Please notice that the changes are listed in reverse chronological order.

  $Header: //source/qcom/qct/modem/datamodem/interface/dss/rel/11.03/src/DSS_SlottedSessionChangedHandler.h#1 $
  $DateTime: 2011/06/17 12:02:33 $$Author: zhasan $

  when       who what, where, why
  ---------- --- ------------------------------------------------------------
  2010-04-18 en  History added.

===========================================================================*/

#include "DSS_EventHandlerNetApp.h"

class DSSSlottedSessionChangedHandler : public DSSEventHandlerNetApp
{
protected:
   virtual void EventOccurred();
   virtual AEEResult RegisterIDL();

public:
   static DSSSlottedSessionChangedHandler* CreateInstance();
   DSSSlottedSessionChangedHandler();
};

#endif // __DSS_SLOTTEDSESSIONCHANGEDHANDLER_H__
