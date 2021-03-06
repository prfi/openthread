/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief
 *  This file implements the functions required for handling NetBufferLists in
 *  the data path.
 */

#include "precomp.h"
#include "datapath.tmh"

#ifdef LOG_BUFFERS

__forceinline CHAR ToHex(CHAR n)
{
    if (n > 9) return 'A' + (n - 10);
    else       return '0' + n;
}

#define otLogLineLength 32

// Helper to log a buffer
void
otLogBuffer(
    _In_reads_bytes_(BufferLength) PUCHAR Buffer,
    _In_                           ULONG  BufferLength
    )
{
    ULONG index = 0;
    while (index < BufferLength)
    {
        CHAR szBuffer[otLogLineLength * 4] = "  ";
        PCHAR buf = szBuffer + 2;
        for (ULONG i = 0; i < otLogLineLength && i + index < BufferLength; i++)
        {
            buf[0] = ToHex(Buffer[i + index] >> 4);
            buf[1] = ToHex(Buffer[i + index] & 0x0F);
            buf[2] = ' ';
            buf += 3;
        }
        buf[0] = 0;

        LogVerbose(DRIVER_DATA_PATH, "%s", szBuffer);
        index += otLogLineLength;
    }
}

#endif

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
otLwfEnableDataPath(
    _In_ PMS_FILTER             pFilter
    )
/*++

Routine Description:

    Enables the datapath to allow NLBs to go through to OpenThread.

--*/
{
    LogFuncEntry(DRIVER_DEFAULT);

    LogInfo(DRIVER_DEFAULT, "Interface %!GUID! enabling data path.", &pFilter->InterfaceGuid);

    // Re-enabling data path
    ExReInitializeRundownProtection(&pFilter->DataPathRundown);

    LogFuncExit(DRIVER_DEFAULT);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
otLwfDisableDataPath(
    _In_ PMS_FILTER             pFilter
    )
/*++

Routine Description:

    Disables the datapath and waits for any outstanding calls 
    into OpenThread to complete.

--*/
{
    LogFuncEntry(DRIVER_DEFAULT);

    LogInfo(DRIVER_DEFAULT, "Interface %!GUID! disabling data path.", &pFilter->InterfaceGuid);

    ExWaitForRundownProtectionRelease(&pFilter->DataPathRundown);

    LogFuncExit(DRIVER_DEFAULT);
}

_Use_decl_annotations_
VOID
FilterSendNetBufferListsComplete(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    ULONG               SendCompleteFlags
    )
/*++

Routine Description:

    Send complete handler

    This routine is invoked whenever the lower layer is finished processing
    sent NET_BUFFER_LISTs.  If the filter does not need to be involved in the
    send path, you should remove this routine and the FilterSendNetBufferLists
    routine.  NDIS will pass along send packets on behalf of your filter more
    efficiently than the filter can.

Arguments:

    FilterModuleContext     - our filter context
    NetBufferLists          - a chain of NBLs that are being returned to you
    SendCompleteFlags       - flags (see documentation)

--*/
{
    PMS_FILTER         pFilter = (PMS_FILTER)FilterModuleContext;

    UNREFERENCED_PARAMETER(SendCompleteFlags);
    
    LogFuncEntryMsg(DRIVER_DATA_PATH, "Filter: %p, NBL: %p", FilterModuleContext, NetBufferLists);

    NT_ASSERT(NetBufferLists == pFilter->SendNetBufferList);
    NT_ASSERT(kStateTransmit == pFilter->otPhyState);
    KeSetEvent(&pFilter->SendNetBufferListComplete, 0, FALSE);
    
    LogFuncExit(DRIVER_DATA_PATH);
}

_Use_decl_annotations_
VOID
FilterSendNetBufferLists(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    NDIS_PORT_NUMBER    PortNumber,
    ULONG               SendFlags
    )
/*++

Routine Description:

    Send Net Buffer List handler
    This function is an optional function for filter drivers. If provided, NDIS
    will call this function to transmit a linked list of NetBuffers, described by a
    NetBufferList, over the network. If this handler is NULL, NDIS will skip calling
    this filter when sending a NetBufferList and will call the next lower
    driver in the stack.  A filter that doesn't provide a FilerSendNetBufferList
    handler can not originate a send on its own.

Arguments:

    FilterModuleContext     - our filter context area
    NetBufferLists          - a List of NetBufferLists to send
    PortNumber              - Port Number to which this send is targeted
    SendFlags               - specifies if the call is at DISPATCH_LEVEL

--*/
{
    PMS_FILTER          pFilter = (PMS_FILTER)FilterModuleContext;
    BOOLEAN             DispatchLevel = NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags);
    
    LogFuncEntryMsg(DRIVER_DATA_PATH, "Filter: %p, NBL: %p", FilterModuleContext, NetBufferLists);
    
    // Try to grab a ref on the data path first, to make sure we are allowed
    if (!ExAcquireRundownProtection(&pFilter->DataPathRundown))
    {
        LogVerbose(DRIVER_DEFAULT, "Failing SendNetBufferLists because data path isn't active.");

        // Ignore any NBLs we get if we aren't active (can't get a ref)
        PNET_BUFFER_LIST CurrNbl = NetBufferLists;
        while (CurrNbl)
        {
            NET_BUFFER_LIST_STATUS(CurrNbl) = NDIS_STATUS_PAUSED;
            CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
        }
        NdisFSendNetBufferListsComplete(
            pFilter->FilterHandle,
            NetBufferLists,
            DispatchLevel ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0
            );
    }
    else
    {
        // Indicate a new NBL to process on our worker thread
        otLwfEventProcessingIndicateNewNetBufferLists(
            pFilter,
            DispatchLevel,
            FALSE,
            PortNumber,
            NetBufferLists
            );

        // Release the data path ref now
        ExReleaseRundownProtection(&pFilter->DataPathRundown);
    }
    
    LogFuncExit(DRIVER_DATA_PATH);
}

_Use_decl_annotations_
VOID
FilterReturnNetBufferLists(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    ULONG               ReturnFlags
    )
/*++

Routine Description:

    FilterReturnNetBufferLists handler.
    FilterReturnNetBufferLists is an optional function. If provided, NDIS calls
    FilterReturnNetBufferLists to return the ownership of one or more NetBufferLists
    and their embedded NetBuffers to the filter driver. If this handler is NULL, NDIS
    will skip calling this filter when returning NetBufferLists to the underlying
    miniport and will call the next lower driver in the stack. A filter that doesn't
    provide a FilterReturnNetBufferLists handler cannot originate a receive indication
    on its own.

Arguments:

    FilterInstanceContext       - our filter context area
    NetBufferLists              - a linked list of NetBufferLists that this
                                  filter driver indicated in a previous call to
                                  NdisFIndicateReceiveNetBufferLists
    ReturnFlags                 - flags specifying if the caller is at DISPATCH_LEVEL

--*/
{
    PMS_FILTER          pFilter = (PMS_FILTER)FilterModuleContext;

    UNREFERENCED_PARAMETER(ReturnFlags);
    
    LogFuncEntryMsg(DRIVER_DATA_PATH, "Filter: %p, NBL: %p", pFilter, NetBufferLists);

    PNET_BUFFER_LIST CurrNbl = NetBufferLists;
    while (CurrNbl)
    {
        if (!NT_SUCCESS(CurrNbl->Status))
        {
            LogVerbose(DRIVER_DATA_PATH, "NBL failed on return: %!STATUS!", CurrNbl->Status);
        }

        PNET_BUFFER_LIST NblToFree = CurrNbl;
        PNET_BUFFER NbToFree = NET_BUFFER_LIST_FIRST_NB(NblToFree);

        CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
        NET_BUFFER_LIST_NEXT_NBL(NblToFree) = NULL;

        NdisAdvanceNetBufferDataStart(NbToFree, NET_BUFFER_DATA_LENGTH(NbToFree), TRUE, NULL);
        NdisFreeNetBufferList(NblToFree);
    }
    
    LogFuncExit(DRIVER_DATA_PATH);
}

_Use_decl_annotations_
VOID
FilterReceiveNetBufferLists(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    NDIS_PORT_NUMBER    PortNumber,
    ULONG               NumberOfNetBufferLists,
    ULONG               ReceiveFlags
    )
/*++

Routine Description:

    FilerReceiveNetBufferLists is an optional function for filter drivers.
    If provided, this function processes receive indications made by underlying
    NIC or lower level filter drivers. This function  can also be called as a
    result of loopback. If this handler is NULL, NDIS will skip calling this
    filter when processing a receive indication and will call the next higher
    driver in the stack. A filter that doesn't provide a
    FilterReceiveNetBufferLists handler cannot provide a
    FilterReturnNetBufferLists handler and cannot a initiate an original receive
    indication on its own.

Arguments:

    FilterModuleContext      - our filter context area.
    NetBufferLists           - a linked list of NetBufferLists
    PortNumber               - Port on which the receive is indicated
    ReceiveFlags             -

N.B.: It is important to check the ReceiveFlags in NDIS_TEST_RECEIVE_CANNOT_PEND.
    This controls whether the receive indication is an synchronous or
    asynchronous function call.

--*/
{

    PMS_FILTER  pFilter = (PMS_FILTER)FilterModuleContext;
    BOOLEAN     DispatchLevel = NDIS_TEST_RECEIVE_AT_DISPATCH_LEVEL(ReceiveFlags);
    
    LogFuncEntryMsg(DRIVER_DATA_PATH, "Filter: %p, NBL: %p", FilterModuleContext, NetBufferLists);

    ASSERT(NumberOfNetBufferLists >= 1);
    UNREFERENCED_PARAMETER(NumberOfNetBufferLists);
    
    // We don't support non-pending NBLs
    NT_ASSERT(NDIS_TEST_RECEIVE_CAN_PEND(ReceiveFlags));
    if (NDIS_TEST_RECEIVE_CANNOT_PEND(ReceiveFlags))
    {
        PNET_BUFFER_LIST CurrNbl = NetBufferLists;
        while (CurrNbl)
        {
            NET_BUFFER_LIST_STATUS(CurrNbl) = NDIS_STATUS_NOT_SUPPORTED;
            CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
        }
    }
    // Try to grab a ref on the data path first, to make sure we are allowed
    else if(pFilter->otPhyState == kStateDisabled || !ExAcquireRundownProtection(&pFilter->DataPathRundown))
    {
        LogVerbose(DRIVER_DATA_PATH, "Failing ReceiveNetBufferLists because data path isn't active.");

        // Ignore any NBLs we get if we aren't active (can't get a ref)
        PNET_BUFFER_LIST CurrNbl = NetBufferLists;
        while (CurrNbl)
        {
            NET_BUFFER_LIST_STATUS(CurrNbl) = NDIS_STATUS_PAUSED;
            CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
        }
        NdisFReturnNetBufferLists(
            pFilter->FilterHandle,
            NetBufferLists,
            DispatchLevel ? NDIS_RETURN_FLAGS_DISPATCH_LEVEL : 0
            );
    }
    else
    {
#if DBG
        PNET_BUFFER_LIST CurrNbl = NetBufferLists;
        while (CurrNbl)
        {
            POT_NBL_CONTEXT NblContext = GetNBLContext(CurrNbl);
            NT_ASSERT(NblContext->Channel >= 11 && NblContext->Channel <= 26);
            CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
        }
        
#endif
        // Indicate a new NBL to process on our worker thread
        otLwfEventProcessingIndicateNewNetBufferLists(
            pFilter,
            DispatchLevel,
            TRUE,
            PortNumber,
            NetBufferLists
            );

        // Release the data path ref now
        ExReleaseRundownProtection(&pFilter->DataPathRundown);
    }
    
    LogFuncExit(DRIVER_DATA_PATH);
}

_Use_decl_annotations_
VOID
FilterCancelSendNetBufferLists(
    NDIS_HANDLE             FilterModuleContext,
    PVOID                   CancelId
    )
/*++

Routine Description:

    This function cancels any NET_BUFFER_LISTs pended in the filter and then
    calls the NdisFCancelSendNetBufferLists to propagate the cancel operation.

    If your driver does not queue any send NBLs, you may omit this routine.
    NDIS will propagate the cancelation on your behalf more efficiently.

Arguments:

    FilterModuleContext      - our filter context area.
    CancelId                 - an identifier for all NBLs that should be dequeued

*/
{
    PMS_FILTER pFilter = (PMS_FILTER)FilterModuleContext;
    
    LogFuncEntryMsg(DRIVER_DATA_PATH, "Filter: %p, CancelId: %p", FilterModuleContext, CancelId);

    otLwfEventProcessingIndicateNetBufferListsCancelled(pFilter, CancelId);
    
    LogFuncExit(DRIVER_DATA_PATH);
}

#pragma pack(push)
#pragma pack(1)

typedef struct UDPHeader
{
    USHORT SourcePort;
    USHORT DestinationPort;
    USHORT TotalLength;
    USHORT Checksum;

} UDPHeader;

#pragma pack(pop)

// Callback received from OpenThread when it has an IPv6 packet ready for
// delivery to TCPIP.
void 
otLwfReceiveIp6DatagramCallback(
    _In_ otMessage aMessage, 
    _In_ void *aContext
    )
{
    PMS_FILTER pFilter = (PMS_FILTER)aContext;
    uint16_t messageLength = otGetMessageLength(aMessage);
    PNET_BUFFER_LIST NetBufferList = NULL;
    PNET_BUFFER NetBuffer = NULL;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;
    PUCHAR DataBuffer = NULL;
    int BytesRead = 0;
    IPV6_HEADER* v6Header;

#ifdef FORCE_SYNCHRONOUS_RECEIVE
    KIRQL irql;
#endif

    // Create the NetBufferList
    NetBufferList =
        NdisAllocateNetBufferAndNetBufferList(
            pFilter->NetBufferListPool,     // PoolHandle
            0,                              // ContextSize
            0,                              // ContextBackFill
            NULL,                           // MdlChain
            0,                              // DataOffset
            0                               // DataLength
            );
    if (NetBufferList == NULL)
    {
        LogWarning(DRIVER_DEFAULT, "Failed to create Recv NetBufferList");
        goto error;
    }

    // Set the flag to indicate its a IPv6 packet
    NdisSetNblFlag(NetBufferList, NDIS_NBL_FLAGS_IS_IPV6);
    NET_BUFFER_LIST_INFO(NetBufferList, NetBufferListFrameType) =
        UlongToPtr(RtlUshortByteSwap(ETHERNET_TYPE_IPV6));
        
    // Initialize NetBuffer fields
    NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
    NET_BUFFER_CURRENT_MDL(NetBuffer) = NULL;
    NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer) = 0;
    NET_BUFFER_DATA_LENGTH(NetBuffer) = 0;
    NET_BUFFER_DATA_OFFSET(NetBuffer) = 0;
    NET_BUFFER_FIRST_MDL(NetBuffer) = NULL;

    // Allocate the NetBuffer for SendNetBufferList
    Status = NdisRetreatNetBufferDataStart(NetBuffer, messageLength, 0, NULL);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisFreeNetBufferList(NetBufferList);
        LogError(DRIVER_DEFAULT, "Failed to allocate NB for Recv NetBufferList, %!NDIS_STATUS!", Status);
        goto error;
    }

    // Get the data buffer to write to
    DataBuffer = NdisGetDataBuffer(NetBuffer, messageLength, NULL, 1, 0);
    NT_ASSERT(DataBuffer);
    if (DataBuffer == NULL)
    {
        NdisAdvanceNetBufferDataStart(NetBuffer, messageLength, TRUE, NULL);
        NdisFreeNetBufferList(NetBufferList);
        LogError(DRIVER_DEFAULT, "Failed to get contiguous data buffer for Recv NetBufferList");
        goto error;
    }

    // Read the bytes to the buffer
    BytesRead = otReadMessage(aMessage, 0, DataBuffer, messageLength);
    NT_ASSERT(BytesRead == (int)messageLength);
    if (BytesRead != (int)messageLength)
    {
        NdisAdvanceNetBufferDataStart(NetBuffer, messageLength, TRUE, NULL);
        NdisFreeNetBufferList(NetBufferList);
        LogError(DRIVER_DEFAULT, "Failed to read message buffer for Recv NetBufferList");
        goto error;
    }

    v6Header = (IPV6_HEADER*)DataBuffer;
    
    // Filter messages to addresses we expose
    if (!IN6_IS_ADDR_MULTICAST(&v6Header->DestinationAddress) &&
        otLwfFindCachedAddrIndex(pFilter, &v6Header->DestinationAddress) == -1)
    {
        NdisAdvanceNetBufferDataStart(NetBuffer, messageLength, TRUE, NULL);
        NdisFreeNetBufferList(NetBufferList);
        LogVerbose(DRIVER_DATA_PATH, "Filter: %p dropping internal address message.", pFilter);
        goto error;
    }
    
    // Filter internal Thread messages
    if (v6Header->NextHeader == IPPROTO_UDP &&
        messageLength >= sizeof(IPV6_HEADER) + sizeof(UDPHeader) &&
        memcmp(&pFilter->otLinkLocalAddr, &v6Header->DestinationAddress, sizeof(IN6_ADDR)) == 0)
    {
        // Check for MLE message
        UDPHeader* UdpHeader = (UDPHeader*)(v6Header + 1);
        if (UdpHeader->DestinationPort == UdpHeader->SourcePort &&
            UdpHeader->DestinationPort == RtlUshortByteSwap(19788)) // MLE Port
        {
            NdisAdvanceNetBufferDataStart(NetBuffer, messageLength, TRUE, NULL);
            NdisFreeNetBufferList(NetBufferList);
            LogVerbose(DRIVER_DATA_PATH, "Filter: %p dropping MLE message.", pFilter);
            goto error;
        }
    }
    
    LogVerbose(DRIVER_DATA_PATH, "Filter: %p, IP6_RECV: %p : %!IPV6ADDR! => %!IPV6ADDR! (%u bytes)", 
               pFilter, NetBufferList, &v6Header->SourceAddress, &v6Header->DestinationAddress,
               messageLength);

#ifdef LOG_BUFFERS
    otLogBuffer(DataBuffer, messageLength);
#endif

#ifdef FORCE_SYNCHRONOUS_RECEIVE
    irql = KfRaiseIrql(DISPATCH_LEVEL);

    if (messageLength == 248) // Magic length used for TAEF test packets
    {
        DbgBreakPoint();
    }
#endif

    // Indicate the NBL up
    NdisFIndicateReceiveNetBufferLists(
        pFilter->FilterHandle,
        NetBufferList,
        NDIS_DEFAULT_PORT_NUMBER,
        1,
#ifdef FORCE_SYNCHRONOUS_RECEIVE
        NDIS_RECEIVE_FLAGS_RESOURCES | NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL
#else
        0
#endif
        );
    
#ifdef FORCE_SYNCHRONOUS_RECEIVE
    KeLowerIrql(irql);
    FilterReturnNetBufferLists(pFilter, NetBufferList, 0);
#endif

error:

    otFreeMessage(aMessage);
}
