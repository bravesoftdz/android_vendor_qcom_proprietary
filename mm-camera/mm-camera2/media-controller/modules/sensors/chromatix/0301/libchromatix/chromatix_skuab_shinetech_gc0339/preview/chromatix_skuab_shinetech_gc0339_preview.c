/*============================================================================

  Copyright (c) 2014 Qualcomm Technologies, Inc. All Rights Reserved.
  Qualcomm Technologies Proprietary and Confidential.

============================================================================*/

/*============================================================================
 *                      INCLUDE FILES
 *===========================================================================*/
#include "chromatix.h"
#include "camera_dbg.h"

static chromatix_parms_type chromatix_skuab_shinetech_gc0339_parms = {
#include "chromatix_skuab_shinetech_gc0339_preview.h"
};

/*============================================================================
 * FUNCTION    - load_chromatix -
 *
 * DESCRIPTION:
 *==========================================================================*/
void *load_chromatix(void)
{
  CDBG("%s:%d%d\n", __func__, __LINE__);
  return &chromatix_skuab_shinetech_gc0339_parms;
}