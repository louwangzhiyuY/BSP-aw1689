/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Module Name:
    hw_isr.c

Abstract:
    Implements interrupt handling functions for the HW layer
    
Revision History:
      When        What
    ----------    ----------------------------------------------
    09-04-2007    Created

Notes:

--*/
#include "Ethmini.h"
#include "hw_isr.tmh"


NDIS_STATUS
HwRegisterInterrupt(
    __in  PHWADAPTER                     Hw
    )
{

    NDIS_STATUS                 ndisStatus;
    NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS     InterruptChars;
	
    DEBUGP(MP_TRACE, "HwRegisterInterrupt ---> \n");

    NdisZeroMemory(&InterruptChars, sizeof(NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS));

    InterruptChars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT;
    InterruptChars.Header.Revision = NDIS_MINIPORT_INTERRUPT_REVISION_1;
    InterruptChars.Header.Size = sizeof(NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS);

    InterruptChars.InterruptHandler = HWInterrupt;

    // Use the new interrupt DPC routine that supports receive side throttling
    InterruptChars.InterruptDpcHandler = HWInterruptDPC;
    
    InterruptChars.DisableInterruptHandler = HwDisableInterrupt;
    InterruptChars.EnableInterruptHandler = HwEnableInterrupt;

    ndisStatus = NdisMRegisterInterruptEx(
                    Hw->Adapter->AdapterHandle,
                    Hw,
                    &InterruptChars,
                    &Hw->InterruptHandle
                    );
    if(ndisStatus != NDIS_STATUS_SUCCESS)
    {
        DEBUGP(MP_ERROR, "Failed to register interrupt with NDIS. Status = 0x%x\n", ndisStatus);
    }

	DEBUGP(MP_TRACE, " <--- HwRegisterInterrupt\n");
    return ndisStatus;
}

// This is called from multiple places and we cannot just change this to call 
// NdisMSynchronizeWithInterruptEx
VOID
HwDisableInterrupt(
    NDIS_HANDLE             MiniportInterruptContext
    )
{
    PHWADAPTER                         Hw = (PHWADAPTER)MiniportInterruptContext;
	
    HW_Mac_Int_Disable(&Hw->Mac);
}

VOID
HwEnableInterrupt(
    NDIS_HANDLE             MiniportInterruptContext
    )
{
    PHWADAPTER                         Hw = (PHWADAPTER)MiniportInterruptContext;
	
    HW_Mac_Int_Enable(&Hw->Mac);
}

/**
 * Called by NDIS when an interrupt occurs that may belong this this miniport
 * 
 * \param MiniportInterruptContext          The Interrupt Context registered with NDIS
 * \param pbQueueMiniportHandleInterrupt    Set to true by this function if we want to
 * queue the Handle Interrupt DPC
 * \param TargetProcessors                  Ununsed
 * \return TRUE if the interrupt was generated by this adapter
 * \sa MPHandleInterrupt
 */
BOOLEAN 
HWInterrupt(
    NDIS_HANDLE             MiniportInterruptContext,
    PBOOLEAN                QueueMiniportHandleInterrupt,
    PULONG                  TargetProcessors
    )
{
    PHWADAPTER                  hw = (PHWADAPTER)MiniportInterruptContext;
    BOOLEAN                     interruptRecognized = FALSE;
	ULONG						IntVal;

    UNREFERENCED_PARAMETER(TargetProcessors);

    do
    {        
        //
        // If the NIC is in low power state, this cannot be our interrupt
        //
        HW_Mac_Get_Int_Status(&hw->Mac, &IntVal);
		hw->IntStatus |= IntVal;

		DEBUGP(MP_TRACE, "HWInterrupt Get 0x%x\n", IntVal);

		if(IntVal & (TX_INT | RX_INT))
		{
			// Schedule DPC
			*QueueMiniportHandleInterrupt = TRUE;
			interruptRecognized = TRUE;
			HwDisableInterrupt(hw);
		}

		HW_Mac_Clear_Int(&hw->Mac, hw->IntStatus);

    } while(FALSE);

    return interruptRecognized;
}


/**
 * Called by NDIS when the miniport claims an interrupt and requests this DPC to
 * be queued during a previous call to MPISR (or by NDIS as part of RST)
 * 
 * \param MiniportInterruptContext    The HW context for this miniport
 * \sa MPISR
 */
VOID
HWInterruptDPC(
    NDIS_HANDLE             MiniportInterruptContext,
    PVOID                   MiniportDpcContext,
    PVOID       			ReceiveThrottleParameters,
    PULONG                  NdisReserved2
    )
{
    PHWADAPTER                         hw = (PHWADAPTER)MiniportInterruptContext;
    PNDIS_RECEIVE_THROTTLE_PARAMETERS   throttleParameters 
            = (PNDIS_RECEIVE_THROTTLE_PARAMETERS)ReceiveThrottleParameters;
    ULONG                       maxNblsToIndicate = NIC_MAX_BUSY_SENDS;
    BOOLEAN                     moreNblsPending = FALSE;

    UNREFERENCED_PARAMETER(NdisReserved2);
    UNREFERENCED_PARAMETER(MiniportDpcContext);

	if(hw->IntStatus & TX_INT)
	{	
		TXSendComplete(hw->Adapter);
		hw->IntStatus &= (~TX_INT);
	}

	if(hw->IntStatus & RX_STOP_INT)
	{	
		HW_MAC_Start_DMA_Transfer(&hw->Mac, FALSE);
		hw->IntStatus &= (~RX_STOP_INT);
	}

	if(hw->IntStatus & RX_INT)
	{
		hw->IntStatus &= (~RX_INT);
		maxNblsToIndicate = (maxNblsToIndicate < throttleParameters->MaxNblsToIndicate)?
							maxNblsToIndicate : throttleParameters->MaxNblsToIndicate;
		moreNblsPending = RXReceiveIndicate(hw->Adapter, maxNblsToIndicate);
	}

    if (moreNblsPending && (throttleParameters->MaxNblsToIndicate != NDIS_INDICATE_ALL_NBLS))
    {
        //
        // Receive side throttling is enabled in the OS. If we have receive packets
        // pending, let the OS requeue the DPC for us without us 
        // enabling the interrupt
        //
        throttleParameters->MoreNblsPending = TRUE;
    }
    else
    {
        //
        // Either RST is not enabled or we dont have packets pending in the H/W.
        // Enable back the interrupts. These were disabled in HWInterrupt routine.
        //
        throttleParameters->MoreNblsPending = FALSE;
        HwEnableInterrupt(hw);
    }

}
