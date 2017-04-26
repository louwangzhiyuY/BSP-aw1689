/** @file
*
*  Copyright (c) 2007-2016, Allwinner Technology Co., Ltd. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
**/

#include "Device.h"
#include "Trace.h"
#include "Registry.h"
#include "AWCodec.h"

#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include "Device.tmh"

EVT_WDF_INTERRUPT_ISR CodecInterruptIsr;
EVT_WDF_INTERRUPT_WORKITEM CodecInterruptWorkItem;

NTSTATUS CodecCreateDevice(
    _In_ PWDFDEVICE_INIT pDeviceInit)
/*++

Routine Description:

    Worker routine called to create a device and its software resources.

Arguments:

    pDeviceInit - Pointer to an opaque init structure. Memory for this
                    structure will be freed by the framework when the WdfDeviceCreate
                    succeeds. So don't access the structure after that point.

Return Value:

    NTSTATUS

--*/
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_OBJECT_ATTRIBUTES ObjectAttributes = { 0 };
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks = { 0 };
    PDEVICE_CONTEXT pDevExt = NULL;
    WDFDEVICE pDevice = NULL;
	WDF_QUERY_INTERFACE_CONFIG QueryInterfaceConfig = { 0 };
	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings = { 0 };
	ULONG IdleTimeout = 0;

	FunctionEnter();

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDevicePrepareHardware = CodecEvtPrepareHardware;
	PnpPowerCallbacks.EvtDeviceReleaseHardware = CodecEvtReleaseHardware;
	PnpPowerCallbacks.EvtDeviceD0Entry = CodecEvtD0Entry;
	PnpPowerCallbacks.EvtDeviceD0Exit = CodecEvtD0Exit;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	// Declare as the PPO
	WdfDeviceInitSetPowerPolicyOwnership(pDeviceInit, TRUE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ObjectAttributes, DEVICE_CONTEXT);

	Status = WdfDeviceCreate(&pDeviceInit, 
		&ObjectAttributes,
		&pDevice);
	if (!NT_SUCCESS(Status))
	{
		DbgPrint_E("WdfDeviceCreate failed with 0x%lx.", Status);
		goto Exit;
	}
    
	//
	// Get a pointer to the device context structure that we just associated
	// with the device object. We define this structure in the device.h
	// header file. DeviceGetContext is an inline function generated by
	// using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
	// This function will do the type checking and return the device context.
	// If you pass a wrong object handle it will return NULL and assert if
	// run under framework verifier mode.
	//
	pDevExt = DeviceGetContext(pDevice);

	//
	// Initialize the context.
	//
	pDevExt->pDevice = pDevice;
	pDevExt->PowerState = WdfPowerDeviceD0;
	pDevExt->IsFirstD0Entry = TRUE;
	pDevExt->pIoTarget = NULL;
	pDevExt->IdleState = 0;

	memset(&pDevExt->PowerDownCompletionCallback, 0, sizeof(pDevExt->PowerDownCompletionCallback));

	WDF_OBJECT_ATTRIBUTES_INIT(&ObjectAttributes);
	ObjectAttributes.ParentObject = pDevice;
	WdfWaitLockCreate(&ObjectAttributes, &pDevExt->pWaitLock);

	WDF_OBJECT_ATTRIBUTES_INIT(&ObjectAttributes);
	ObjectAttributes.ParentObject = pDevice;
	WdfSpinLockCreate(&ObjectAttributes, &pDevExt->pListSpinLock);

	InitializeListHead(&pDevExt->NotificationList);

	//
	// Register remote device function interface
	//
	pDevExt->CodecInterfaceRef = 0;
	pDevExt->CodecFunctionInterface.InterfaceHeader.Size = sizeof(pDevExt->CodecFunctionInterface);
	pDevExt->CodecFunctionInterface.InterfaceHeader.Version = 1;
	pDevExt->CodecFunctionInterface.InterfaceHeader.Context = (PVOID)pDevice;

	pDevExt->CodecFunctionInterface.InterfaceHeader.InterfaceReference = CodecInterfaceReference;
	pDevExt->CodecFunctionInterface.InterfaceHeader.InterfaceDereference = CodecInterfaceDereference;
	pDevExt->CodecFunctionInterface.CodecRegisterEndpointNotification = CodecRegisterEndpointNotification;
	pDevExt->CodecFunctionInterface.CodecSetPowerState = CodecSetPowerState;
	pDevExt->CodecFunctionInterface.CodecSetPowerDownCompletionCallback = CodecSetPowerDownCompletionCallback;
	pDevExt->CodecFunctionInterface.CodecSetStreamState = CodecSetStreamState;
	pDevExt->CodecFunctionInterface.CodecSetStreamMode = CodecSetStreamMode;
	pDevExt->CodecFunctionInterface.CodecSetStreamFormat = CodecSetStreamFormat;
	pDevExt->CodecFunctionInterface.CodecSetVolume = CodecSetVolume;
	pDevExt->CodecFunctionInterface.CodecGetVolume = CodecGetVolume;
	pDevExt->CodecFunctionInterface.CodecSetMute = CodecSetMute;
	pDevExt->CodecFunctionInterface.CodecGetMute = CodecGetMute;
	pDevExt->CodecFunctionInterface.CodecGetPeakMeter = CodecGetPeakMeter;

	WDF_QUERY_INTERFACE_CONFIG_INIT(&QueryInterfaceConfig,
		(PINTERFACE)&pDevExt->CodecFunctionInterface,
		&GUID_CODEC_FUNCTION_INTERFACE,
		NULL);

	Status = WdfDeviceAddQueryInterface(pDevice, &QueryInterfaceConfig);
	if (!NT_SUCCESS(Status))
	{
		DbgPrint_E("WdfDeviceAddQueryInterface failed with 0x%lx.", Status);
		goto Exit;
	}

	//
	// Register device interface
	//
	Status = WdfDeviceCreateDeviceInterface(pDevice,
		&GUID_CODEC_INTERFACE,
		NULL);
	if (!NT_SUCCESS(Status))
	{
		DbgPrint_E("WdfDeviceCreateDeviceInterface failed with 0x%lx.", Status);
	}

	//
	// Register idle capabilities
	//

	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(
		&IdleSettings,
		IdleCanWakeFromS0);

	Status = RegQueryDeviceDwordValue(pDevice, IDLE_TIMEOUT_VALUE_NAME, &IdleTimeout);
	if (!NT_SUCCESS(Status))
	{
		DbgPrint_W("Failed to query device idle timeout value with 0x%lx.", Status);
		Status = STATUS_SUCCESS;
		IdleTimeout = DEFAULT_IDLE_TIMEOUT_VALUE_MS;
	}

	IdleSettings.IdleTimeout = IdleTimeout;
	IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;

	Status = WdfDeviceAssignS0IdleSettings(pDevice, &IdleSettings);
	if (STATUS_POWER_STATE_INVALID == Status)
	{
		DbgPrint_W("WdfDeviceAssignS0IdleSettings with IdleCanWakeFromS0 failed.");

		IdleSettings.IdleCaps = IdleCannotWakeFromS0;
		Status = WdfDeviceAssignS0IdleSettings(pDevice, &IdleSettings);
		if (!NT_SUCCESS(Status))
		{
			DbgPrint_E("WdfDeviceAssignS0IdleSettings with IdleCannotWakeFromS0 failed with 0x%lx.", Status);
		}
	}

	AWCodecMapReg();

Exit:
	FunctionExit(Status);
    return Status;
}

//
// Disable interrupt processing now, need to re-enable it if in necessary
//
NTSTATUS CodecEvtPrepareHardware(
	_In_ WDFDEVICE pDevice,
	_In_ WDFCMRESLIST pResources,
	_In_ WDFCMRESLIST pResourcesTranslated)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDevExt = NULL;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR pPartialResourceDescRaw = NULL;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR pPartialResourceDescTrans = NULL;
	//WDFINTERRUPT pInterrupt = NULL;
	WDF_INTERRUPT_CONFIG InterruptConfig = { 0 };
	WDF_OBJECT_ATTRIBUTES ObjectAttributes = { 0 };
	WDF_IO_TARGET_OPEN_PARAMS OpenParams = { 0 };
	LARGE_INTEGER GpioIoConnId = { 0 };
	//ULONG GpioIntResourceCount = 0;
	//ULONG GpioIoResourceCount = 0;
	ULONG ResourceListCountRaw = 0;
	ULONG ResourceListCountTrans = 0;
	//ULONG DesiredAccess = 0;
	UINT i = 0;
	DECLARE_UNICODE_STRING_SIZE(DevicePath, RESOURCE_HUB_PATH_SIZE);

	FunctionEnter();

	pDevExt = DeviceGetContext(pDevice);

	// Determines how many resources we will iterate over.
	ResourceListCountRaw = WdfCmResourceListGetCount(pResources);
	ResourceListCountTrans = WdfCmResourceListGetCount(pResourcesTranslated);

	if (ResourceListCountRaw != ResourceListCountTrans)
	{
		Status = STATUS_UNSUCCESSFUL;
		goto Exit;
	}

	for (i = 0; i < ResourceListCountRaw; i++)
	{
		// Gets the raw partial resource descriptor.
		pPartialResourceDescRaw =
			WdfCmResourceListGetDescriptor(pResources, i);

		// Gets the translated partial resource descriptor.
		pPartialResourceDescTrans =
			WdfCmResourceListGetDescriptor(pResourcesTranslated, i);

		// Switch based upon the type of resource.
		switch (pPartialResourceDescTrans->Type)
		{
			case CmResourceTypeConnection:
			{
				//if ((CM_RESOURCE_CONNECTION_CLASS_GPIO == pPartialResourceDescTrans->u.Connection.Class)
				//	&& (CM_RESOURCE_CONNECTION_TYPE_GPIO_IO == pPartialResourceDescTrans->u.Connection.Type))
				//{
				//	if (GpioIoResourceCount < INTERRUPT_PIN_NUMBER)
				//	{
				//		GpioIoConnId.LowPart = pPartialResourceDescTrans->u.Connection.IdLowPart;
				//		GpioIoConnId.HighPart = pPartialResourceDescTrans->u.Connection.IdHighPart;
				//		GpioIoResourceCount++;
				//	}
				//}
				//else if ((CM_RESOURCE_CONNECTION_CLASS_SERIAL == pPartialResourceDescTrans->u.Connection.Class)
				//	&& (CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C == pPartialResourceDescTrans->u.Connection.Type))
				//{
				//	continue;
				//}

				break;
			}

			case CmResourceTypeInterrupt:
			{
				//if (++GpioIntResourceCount == INTERRUPT_PIN_NUMBER) 
				//{
				//	WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, CodecInterruptIsr, NULL);
				//	InterruptConfig.InterruptRaw = pPartialResourceDescRaw;
				//	InterruptConfig.InterruptTranslated = pPartialResourceDescTrans;
				//	InterruptConfig.EvtInterruptWorkItem = CodecInterruptWorkItem;
				//	InterruptConfig.PassiveHandling = TRUE;

				//	WDF_OBJECT_ATTRIBUTES_INIT(&ObjectAttributes);
				//	//ObjectAttributes.ParentObject = pDevExt->pDevice;

				//	Status = WdfInterruptCreate(pDevExt->pDevice, 
				//		&InterruptConfig, 
				//		&ObjectAttributes, 
				//		&pInterrupt);
				//	if (!NT_SUCCESS(Status))
				//	{
				//		DbgPrint_E("WdfInterruptCreate failed with 0x%lx.", Status);
				//		goto Exit;
				//	}
				//}

				break;
			}

			default:
				break;
		}
	}

	//if ((INTERRUPT_PIN_NUMBER != GpioIoResourceCount)
	//	|| (INTERRUPT_PIN_NUMBER != GpioIntResourceCount))
	//{
	//	DbgPrint_E("Invalid GPIO resources, interrupt count %ld, I/O count %ld.", GpioIntResourceCount, GpioIoResourceCount);
	//	Status = STATUS_INVALID_DEVICE_STATE;
	//	goto Exit;
	//}

	//WDF_OBJECT_ATTRIBUTES_INIT(&ObjectAttributes);
	//ObjectAttributes.ParentObject = pDevExt->pDevice;
	//
	//Status = WdfIoTargetCreate(pDevExt->pDevice,
	//	&ObjectAttributes,
	//	&pDevExt->pIoTarget);
	//if (!NT_SUCCESS(Status)) 
	//{
	//	DbgPrint_E("WdfIoTargetCreate failed with 0x%lx.", Status);
	//	goto Exit;
	//}

	//DevicePath.Length = 0;
	//memset(DevicePath.Buffer, 0, DevicePath.MaximumLength);
	//Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&DevicePath,
	//	GpioIoConnId.LowPart,
	//	GpioIoConnId.HighPart);

	//DesiredAccess = FILE_GENERIC_READ;

	//RtlZeroMemory(&OpenParams, sizeof(OpenParams));
	//WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams,
	//	&DevicePath,
	//	DesiredAccess);

	//Status = WdfIoTargetOpen(pDevExt->pIoTarget, &OpenParams);
	//if (!NT_SUCCESS(Status)) 
	//{
	//	DbgPrint_E("WdfIoTargetOpen failed with 0x%lx.", Status);
	//	goto Exit;
	//}

	//
	// TODO: Initial codec hardware here
	//
	AWCodecInitHw();
	
Exit:
	FunctionExit(Status);
	return Status;
}

NTSTATUS CodecEvtReleaseHardware(
	_In_ WDFDEVICE pDevice,
	_In_ WDFCMRESLIST pResourcesTranslated)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDevExt = NULL;

	UNREFERENCED_PARAMETER(pResourcesTranslated);

	FunctionEnter();

	pDevExt = DeviceGetContext(pDevice);

	//
	// TODO: Uninitialize codec hardware here
	//

	if (NULL != pDevExt->pIoTarget) 
	{
		WdfIoTargetClose(pDevExt->pIoTarget);
		WdfObjectDelete(pDevExt->pIoTarget);
		pDevExt->pIoTarget = NULL;
	}

	if (NULL != pDevExt->pWaitLock) 
	{
		WdfObjectDelete(pDevExt->pWaitLock);
		pDevExt->pWaitLock = NULL;
	}

	if (NULL != pDevExt->pListSpinLock) 
	{
		EmptyEndpointNotificationList(pDevExt);

		WdfObjectDelete(pDevExt->pListSpinLock);
		pDevExt->pListSpinLock = NULL;
	}

	//
	// TODO: Check the function interface reference here???
	//

	FunctionExit(Status);
	return Status;
}

NTSTATUS CodecEvtD0Entry(
	_In_ WDFDEVICE pDevice,
	_In_ WDF_POWER_DEVICE_STATE PreviousState) 
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDevExt = NULL;

	UNREFERENCED_PARAMETER(PreviousState);
	
	FunctionEnter();
	DbgPrint_I("Entered, device is coming from [D %d] state.", PreviousState - WdfPowerDeviceD0);

	pDevExt = DeviceGetContext(pDevice);

	//
	// TODO: Power up the device here
	//

	pDevExt->PowerState = WdfPowerDeviceD0;
	if (TRUE == pDevExt->IsFirstD0Entry)
	{
		WdfDeviceStopIdle(pDevExt->pDevice, FALSE);
		pDevExt->IdleState = 1;
		pDevExt->IsFirstD0Entry = FALSE;
	}

	FunctionExit(Status);
	return Status;
}

NTSTATUS CodecEvtD0Exit(
	_In_ WDFDEVICE pDevice,
	_In_ WDF_POWER_DEVICE_STATE TargetState)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDevExt = NULL;

	UNREFERENCED_PARAMETER(TargetState);

	FunctionEnter();
	DbgPrint_I("Entered, device is going to [D %d] state.", TargetState - WdfPowerDeviceD0);

	pDevExt = DeviceGetContext(pDevice);

	//
	// TODO: Power down the device here
	//

	pDevExt->PowerState = TargetState;

	//
	// Notify the audio adapter after the D3 entry
	//
	if ((NULL != pDevExt->PowerDownCompletionCallback.pCallbackContext)
	&& (NULL != pDevExt->PowerDownCompletionCallback.pCallbackRoutine))
	{
		pDevExt->PowerDownCompletionCallback.pCallbackRoutine(pDevExt->PowerDownCompletionCallback.pCallbackContext);
	}

	FunctionExit(Status);
	return Status;
}

BOOLEAN CodecInterruptIsr(
	_In_ WDFINTERRUPT pInterrupt,
	_In_ ULONG MessageId) 
{
	UNREFERENCED_PARAMETER(MessageId);

	if (NULL != pInterrupt)
	{
		WdfInterruptQueueWorkItemForIsr(pInterrupt);
	}

	return TRUE;
}

void CodecInterruptWorkItem(
	_In_ WDFINTERRUPT pInterrupt,
	_In_ WDFOBJECT pAssociatedObject) 
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDevExt = NULL;
	PENDPOINT_NOTIFICATION_LIST_ITEM pItem = NULL;
	JACK_INFO_CHANGE_NOTIFICATION NotificationInfo;

	FunctionEnter();

	if ((NULL == pInterrupt) || (NULL == pAssociatedObject))
	{
		DbgPrint_E("Invalid parameters.");
		Status = STATUS_INVALID_PARAMETER;
		goto Exit;
	}

	pDevExt = DeviceGetContext(pAssociatedObject);

	//
	// TODO: Check the jack info change interrupt status
	//

	//
	// Initialize the jack info change notification
	//
	memset(&NotificationInfo, 0, sizeof(NotificationInfo));

	NotificationInfo.Header.Size = sizeof(NotificationInfo);
	NotificationInfo.Header.NotificationType = NOTIFICATION_TYPE_JACK_INFO_CHANGE;

	//
	// TODO: Set the jack connection status as the checking result
	//
	NotificationInfo.ConnectionState = JACK_DISCONNECTED;


	//
	// TODO: Notify the audio adapter the exact headset device, speaker or mic as following.
	//
	
	//
	//Find the headset speaker device and notify its endpoint
	//
	NotificationInfo.Header.DeviceType = ENDPOINT_SPEAKER_HEADSET_DEVICE;
	pItem = GetEndpointNotificationItem(pDevExt, NOTIFICATION_TYPE_JACK_INFO_CHANGE, ENDPOINT_SPEAKER_HEADSET_DEVICE);
	if (NULL != pItem) 
	{
		if ((NULL != pItem->pCallbackContex)
			&& (NULL != pItem->pCallbackRoutine)) 
		{
			pItem->pCallbackRoutine(pItem->pCallbackContex, &NotificationInfo);
		}
	}

	//
	// Find the headset mic device and notify its endpoint
	//
	NotificationInfo.Header.DeviceType = ENDPOINT_MIC_HEADSET_DEVICE;
	pItem = GetEndpointNotificationItem(pDevExt, NOTIFICATION_TYPE_JACK_INFO_CHANGE, ENDPOINT_MIC_HEADSET_DEVICE);
	if (NULL != pItem)
	{
		if ((NULL != pItem->pCallbackContex)
			&& (NULL != pItem->pCallbackRoutine))
		{
			pItem->pCallbackRoutine(pItem->pCallbackContex, &NotificationInfo);
		}
	}

	//
	// TODO: Change the jack output/input, if they are controlled by SW
	//

Exit:
	FunctionExit(Status);
	return;
}

PENDPOINT_NOTIFICATION_LIST_ITEM GetEndpointNotificationItem(
	_In_ DEVICE_CONTEXT *pDevExt, 
	_In_ ENDPOINT_NOTIFICATION_TYPE NotificationType, 
	_In_ ENDPOINT_DEVICE_TYPE DeviceType)
{
	PLIST_ENTRY pCurrentEntry = NULL;
	PENDPOINT_NOTIFICATION_LIST_ITEM pCurrentItem = NULL;
	PENDPOINT_NOTIFICATION_LIST_ITEM pFoundItem = NULL;

	WdfSpinLockAcquire(pDevExt->pListSpinLock);

	if (!IsListEmpty(&pDevExt->NotificationList))
	{
		pCurrentEntry = pDevExt->NotificationList.Flink;

		while (pCurrentEntry != &pDevExt->NotificationList)
		{
			pCurrentItem = CONTAINING_RECORD(pCurrentEntry, ENDPOINT_NOTIFICATION_LIST_ITEM, ListEntry);

			if ((NotificationType == pCurrentItem->NotificationType)
				&& (DeviceType == pCurrentItem->DeviceType)) 
			{
				pFoundItem = pCurrentItem;
				break;
			}

			pCurrentEntry = pCurrentEntry->Flink;
		}
	}

	WdfSpinLockRelease(pDevExt->pListSpinLock);

	return pFoundItem;
}

void InsertEndpointNotificationItem(
	_In_ DEVICE_CONTEXT *pDevExt,
	_In_ PENDPOINT_NOTIFICATION_LIST_ITEM pItem) 
{
	PENDPOINT_NOTIFICATION_LIST_ITEM pFoundItem = NULL;

	pFoundItem = GetEndpointNotificationItem(pDevExt, pItem->NotificationType, pItem->DeviceType);

	if (NULL == pFoundItem)
	{
		WdfSpinLockAcquire(pDevExt->pListSpinLock);
		InsertTailList(&pDevExt->NotificationList, &pItem->ListEntry);
		WdfSpinLockRelease(pDevExt->pListSpinLock);
	}
	else 
	{
		DbgPrint_W("The notification callback already registered.");
	}

	return;
}

void EmptyEndpointNotificationList(
	_In_ DEVICE_CONTEXT *pDevExt) 
{
	PLIST_ENTRY pCurrentEntry = NULL;
	PENDPOINT_NOTIFICATION_LIST_ITEM pCurrentItem = NULL;

	WdfSpinLockAcquire(pDevExt->pListSpinLock);

	while (!IsListEmpty(&pDevExt->NotificationList))
	{
		pCurrentEntry = RemoveHeadList(&pDevExt->NotificationList);
		pCurrentItem = CONTAINING_RECORD(pCurrentEntry, ENDPOINT_NOTIFICATION_LIST_ITEM, ListEntry);

		ExFreePoolWithTag(pCurrentItem, CODEC_POOL_TAG);
	}

	WdfSpinLockRelease(pDevExt->pListSpinLock);

	return;
}