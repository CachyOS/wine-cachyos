/*
 * Copyright (c) 2015 Mark Harmstone
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "mmsystem.h"
#include "winternl.h"
#include "vfwmsgs.h"
#include "wine/debug.h"
#include "dsound.h"
#include "dsound_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(eax);

HRESULT WINAPI EAX_Get(IDirectSoundBufferImpl *buf, REFGUID guidPropSet,
        ULONG dwPropID, void *pInstanceData, ULONG cbInstanceData, void *pPropData,
        ULONG cbPropData, ULONG *pcbReturned)
{
    TRACE("(buf=%p,guidPropSet=%s,dwPropID=%lu,pInstanceData=%p,cbInstanceData=%lu,pPropData=%p,cbPropData=%lu,pcbReturned=%p)\n",
        buf, debugstr_guid(guidPropSet), dwPropID, pInstanceData, cbInstanceData, pPropData, cbPropData, pcbReturned);

    *pcbReturned = 0;

    return E_PROP_ID_UNSUPPORTED;
}

HRESULT WINAPI EAX_Set(IDirectSoundBufferImpl *buf, REFGUID guidPropSet,
        ULONG dwPropID, void *pInstanceData, ULONG cbInstanceData, void *pPropData,
        ULONG cbPropData)
{
    TRACE("(%p,%s,%lu,%p,%lu,%p,%lu)\n",
        buf, debugstr_guid(guidPropSet), dwPropID, pInstanceData, cbInstanceData, pPropData, cbPropData);

    return E_PROP_ID_UNSUPPORTED;
}
