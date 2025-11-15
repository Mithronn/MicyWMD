/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    adapter.cpp

Abstract:

    Setup and miniport installation.  No resources are used by simple audio sample.
    This sample is to demonstrate how to develop a full featured audio miniport driver.
--*/

#pragma warning (disable : 4127)

//
// All the GUIDS for all the miniports end up in this object.
//
#define PUT_GUIDS_HERE

#include "definitions.h"
#include "endpoints.h"
#include "minipairs.h"

#define MICY_IOCTL_TYPE     29
#define NT_DEVICE_NAME      L"\\Device\\MICY"
#define DOS_DEVICE_NAME     L"\\DosDevices\\MicyAudio"
// IOCTL code for setting user-provided audio data
#define IOCTL_SIMPLEAUDIOSAMPLE_SET_AUDIO_DATA \
    CTL_CODE(MICY_IOCTL_TYPE, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef void (*fnPcDriverUnload) (PDRIVER_OBJECT);
fnPcDriverUnload gPCDriverUnloadRoutine = NULL;
extern "C" DRIVER_UNLOAD DriverUnload;

// Store original CREATE/CLOSE handlers before we replace them
DRIVER_DISPATCH* g_OriginalCreateHandler = NULL;
DRIVER_DISPATCH* g_OriginalCloseHandler = NULL;

//-----------------------------------------------------------------------------
// Referenced forward.
//-----------------------------------------------------------------------------

DRIVER_ADD_DEVICE AddDevice;

NTSTATUS
StartDevice
( 
    _In_  PDEVICE_OBJECT,      
    _In_  PIRP,                
    _In_  PRESOURCELIST        
); 

_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH PnpHandler;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH DeviceControlHandler;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH ControlDeviceCreateClose;

//
// Rendering streams are not saved to a file by default. Use the registry value 
// DoNotCreateDataFiles (DWORD) = 0 to override this default.
//
DWORD g_DoNotCreateDataFiles = 1;  // default is off.
DWORD g_DisableToneGenerator = 0;  // default is to generate tones.
UNICODE_STRING g_RegistryPath;      // This is used to store the registry settings path for the driver
PDEVICE_OBJECT g_ControlDeviceObject = NULL;  // Control device for IOCTL communication

// =============================================================================
// Simple lock-protected circular buffer to feed capture (microphone) path
// =============================================================================
extern "C" {
    typedef struct _USER_PCM_RING_BUFFER {
        PUCHAR              buffer;
        ULONG               capacity;   // bytes
        volatile ULONG      readIndex;  // modulo capacity
        volatile ULONG      writeIndex; // modulo capacity
        volatile ULONG      count;      // bytes currently stored
        KSPIN_LOCK          lock;       // protects read/write/count
        BOOLEAN             initialized;
    } USER_PCM_RING_BUFFER;

    static USER_PCM_RING_BUFFER g_UserPcmRb = { 0 };

    static __forceinline ULONG _min_ul(ULONG a, ULONG b) { return (a < b) ? a : b; }

    VOID UserPcmBuffer_Term()
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_UserPcmRb.lock, &oldIrql);
        if (g_UserPcmRb.buffer) {
            PUCHAR buf = g_UserPcmRb.buffer;
            g_UserPcmRb.buffer = NULL;
            g_UserPcmRb.capacity = 0;
            g_UserPcmRb.readIndex = 0;
            g_UserPcmRb.writeIndex = 0;
            g_UserPcmRb.count = 0;
            g_UserPcmRb.initialized = FALSE;
            KeReleaseSpinLock(&g_UserPcmRb.lock, oldIrql);
            ExFreePoolWithTag(buf, MINADAPTER_POOLTAG);
            return;
        }
        KeReleaseSpinLock(&g_UserPcmRb.lock, oldIrql);
    }

    NTSTATUS UserPcmBuffer_Init(_In_ ULONG capacityBytes)
    {
        if (capacityBytes == 0) {
            capacityBytes = 1024 * 1024; // default 1MB
        }

        RtlZeroMemory(&g_UserPcmRb, sizeof(g_UserPcmRb));
        g_UserPcmRb.buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, capacityBytes, MINADAPTER_POOLTAG);
        if (!g_UserPcmRb.buffer) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        g_UserPcmRb.capacity = capacityBytes;
        g_UserPcmRb.readIndex = 0;
        g_UserPcmRb.writeIndex = 0;
        g_UserPcmRb.count = 0;
        KeInitializeSpinLock(&g_UserPcmRb.lock);
        g_UserPcmRb.initialized = TRUE;
        return STATUS_SUCCESS;
    }

    ULONG UserPcmBuffer_Write(_In_reads_bytes_(length) const UCHAR* src, _In_ ULONG length)
    {
        if (!g_UserPcmRb.initialized || src == NULL || length == 0) return 0;
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_UserPcmRb.lock, &oldIrql);

        ULONG toWrite = length;
        // If not enough space, drop oldest (advance readIndex)
        if (toWrite > (g_UserPcmRb.capacity - g_UserPcmRb.count)) {
            ULONG overflow = toWrite - (g_UserPcmRb.capacity - g_UserPcmRb.count);
            if (overflow > g_UserPcmRb.count) overflow = g_UserPcmRb.count;
            g_UserPcmRb.readIndex = (g_UserPcmRb.readIndex + overflow) % g_UserPcmRb.capacity;
            g_UserPcmRb.count -= overflow;
        }

        // Write in up to two segments (wrap-aware)
        ULONG first = _min_ul(toWrite, g_UserPcmRb.capacity - g_UserPcmRb.writeIndex);
        RtlCopyMemory(g_UserPcmRb.buffer + g_UserPcmRb.writeIndex, src, first);
        ULONG remaining = toWrite - first;
        if (remaining) {
            RtlCopyMemory(g_UserPcmRb.buffer, src + first, remaining);
        }
        g_UserPcmRb.writeIndex = (g_UserPcmRb.writeIndex + toWrite) % g_UserPcmRb.capacity;
        g_UserPcmRb.count += toWrite;

        KeReleaseSpinLock(&g_UserPcmRb.lock, oldIrql);
        return toWrite;
    }

    ULONG UserPcmBuffer_Read(_Out_writes_bytes_(length) UCHAR* dst, _In_ ULONG length)
    {
        if (!g_UserPcmRb.initialized || dst == NULL || length == 0) return 0;
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_UserPcmRb.lock, &oldIrql);

        ULONG toRead = _min_ul(length, g_UserPcmRb.count);
        if (toRead) {
            ULONG first = _min_ul(toRead, g_UserPcmRb.capacity - g_UserPcmRb.readIndex);
            RtlCopyMemory(dst, g_UserPcmRb.buffer + g_UserPcmRb.readIndex, first);
            ULONG remaining = toRead - first;
            if (remaining) {
                RtlCopyMemory(dst + first, g_UserPcmRb.buffer, remaining);
            }
            g_UserPcmRb.readIndex = (g_UserPcmRb.readIndex + toRead) % g_UserPcmRb.capacity;
            g_UserPcmRb.count -= toRead;
        }

        KeReleaseSpinLock(&g_UserPcmRb.lock, oldIrql);
        return toRead;
    }

    ULONG UserPcmBuffer_Count()
    {
        if (!g_UserPcmRb.initialized) return 0;
        return g_UserPcmRb.count;
    }

    VOID UserPcmBuffer_Clear()
    {
        if (!g_UserPcmRb.initialized) return;
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_UserPcmRb.lock, &oldIrql);
        g_UserPcmRb.readIndex = 0;
        g_UserPcmRb.writeIndex = 0;
        g_UserPcmRb.count = 0;
        KeReleaseSpinLock(&g_UserPcmRb.lock, oldIrql);
    }
}
//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

#pragma code_seg("PAGE")
void ReleaseRegistryStringBuffer()
{
    PAGED_CODE();

    if (g_RegistryPath.Buffer != NULL)
    {
        ExFreePool(g_RegistryPath.Buffer);
        g_RegistryPath.Buffer = NULL;
        g_RegistryPath.Length = 0;
        g_RegistryPath.MaximumLength = 0;
    }
}

//=============================================================================
#pragma code_seg("PAGE")
extern "C"
void DriverUnload 
(
    _In_ PDRIVER_OBJECT DriverObject
)
/*++

Routine Description:

  Our driver unload routine. This just frees the WDF driver object.

Arguments:

  DriverObject - pointer to the driver object

Environment:

    PASSIVE_LEVEL

--*/
{
    PAGED_CODE(); 

    DPF(D_TERSE, ("[DriverUnload]"));

    ReleaseRegistryStringBuffer();

    if (DriverObject == NULL)
    {
        goto Done;
    }
    
    //
    // Invoke first the port unload.
    //
    if (gPCDriverUnloadRoutine != NULL)
    {
        gPCDriverUnloadRoutine(DriverObject);
    }

    //
    // Unload WDF driver object. 
    //
    if (WdfGetDriver() != NULL)
    {
        WdfDriverMiniportUnload(WdfGetDriver());
    }

    //
    // Unload control device
    //
    UNICODE_STRING uniWin32NameString;
    RtlInitUnicodeString(&uniWin32NameString, DOS_DEVICE_NAME);

    IoDeleteSymbolicLink(&uniWin32NameString);

    if (g_ControlDeviceObject != NULL)
    {
        IoDeleteDevice(g_ControlDeviceObject);
        g_ControlDeviceObject = NULL;
    }
    // Release user PCM ring buffer
    UserPcmBuffer_Term();
Done:
    return;
}

//=============================================================================
#pragma code_seg("INIT")
__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS
CopyRegistrySettingsPath(
    _In_ PUNICODE_STRING RegistryPath
)
/*++

Routine Description:

Copies the following registry path to a global variable.

\REGISTRY\MACHINE\SYSTEM\ControlSetxxx\Services\<driver>\Parameters

Arguments:

RegistryPath - Registry path passed to DriverEntry

Returns:

NTSTATUS - SUCCESS if able to configure the framework

--*/

{
    // Initializing the unicode string, so that if it is not allocated it will not be deallocated too.
    RtlInitUnicodeString(&g_RegistryPath, NULL);

    g_RegistryPath.MaximumLength = RegistryPath->Length + sizeof(WCHAR);

    g_RegistryPath.Buffer = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, g_RegistryPath.MaximumLength, MINADAPTER_POOLTAG);

    if (g_RegistryPath.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlAppendUnicodeToString(&g_RegistryPath, RegistryPath->Buffer);

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("INIT")
__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS
GetRegistrySettings(
    _In_ PUNICODE_STRING RegistryPath
   )
/*++

Routine Description:

    Initialize Driver Framework settings from the driver
    specific registry settings under

    \REGISTRY\MACHINE\SYSTEM\ControlSetxxx\Services\<driver>\Parameters

Arguments:

    RegistryPath - Registry path passed to DriverEntry

Returns:

    NTSTATUS - SUCCESS if able to configure the framework

--*/

{
    NTSTATUS                    ntStatus;
    PDRIVER_OBJECT              DriverObject;
    HANDLE                      DriverKey;
    RTL_QUERY_REGISTRY_TABLE    paramTable[] = {
    // QueryRoutine     Flags                                               Name                     EntryContext             DefaultType                                                    DefaultData              DefaultLength
        { NULL,   RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK, L"DoNotCreateDataFiles", &g_DoNotCreateDataFiles, (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD, &g_DoNotCreateDataFiles, sizeof(ULONG)},
        { NULL,   RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK, L"DisableToneGenerator", &g_DisableToneGenerator, (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD, &g_DisableToneGenerator, sizeof(ULONG)},
        { NULL,   0,                                                        NULL,                    NULL,                    0,                                                             NULL,                    0}
    };

    DPF(D_TERSE, ("[GetRegistrySettings]"));

    PAGED_CODE();
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject = WdfDriverWdmGetDriverObject(WdfGetDriver());
    DriverKey = NULL;
    ntStatus = IoOpenDriverRegistryKey(DriverObject, 
                                 DriverRegKeyParameters,
                                 KEY_READ,
                                 0,
                                 &DriverKey);

    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    ntStatus = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                                  (PCWSTR) DriverKey,
                                  &paramTable[0],
                                  NULL,
                                  NULL);

    if (!NT_SUCCESS(ntStatus)) 
    {
        DPF(D_VERBOSE, ("RtlQueryRegistryValues failed, using default values, 0x%x", ntStatus));
        //
        // Don't return error because we will operate with default values.
        //
    }

    //
    // Dump settings.
    //
    DPF(D_VERBOSE, ("DoNotCreateDataFiles: %u", g_DoNotCreateDataFiles));
    DPF(D_VERBOSE, ("DisableToneGenerator: %u", g_DisableToneGenerator));

    if (DriverKey)
    {
        ZwClose(DriverKey);
    }

    return STATUS_SUCCESS;
}

#pragma code_seg("INIT")
extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS
DriverEntry
( 
    _In_  PDRIVER_OBJECT          DriverObject,
    _In_  PUNICODE_STRING         RegistryPathName
)
{
/*++

Routine Description:

  Installable driver initialization entry point.
  This entry point is called directly by the I/O system.

  All audio adapter drivers can use this code without change.

Arguments:

  DriverObject - pointer to the driver object

  RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

  STATUS_SUCCESS if successful,
  STATUS_UNSUCCESSFUL otherwise.

--*/
    NTSTATUS                    ntStatus;
    WDF_DRIVER_CONFIG           config;

    //
    // Create control device for IOCTL communication via symbolic link
    //
    UNICODE_STRING  ntUnicodeString;    // NT Device Name "\Device\MICY"
    UNICODE_STRING  ntWin32NameString;    // Win32 Name "\DosDevices\MicyAudio"
    PDEVICE_OBJECT  deviceObject = NULL;    // ptr to device object

    DPF(D_TERSE, ("[DriverEntry]"));

    // Copy registry Path name in a global variable to be used by modules inside driver.
    // !! NOTE !! Inside this function we are initializing the registrypath, so we MUST NOT add any failing calls
    // before the following call.
    ntStatus = CopyRegistrySettingsPath(RegistryPathName);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("Registry path copy error 0x%x", ntStatus)),
        Done);
    
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    //
    // Set WdfDriverInitNoDispatchOverride flag to tell the framework
    // not to provide dispatch routines for the driver. In other words,
    // the framework must not intercept IRPs that the I/O manager has
    // directed to the driver. In this case, they will be handled by Audio
    // port driver.
    //
    config.DriverInitFlags |= WdfDriverInitNoDispatchOverride;
    config.DriverPoolTag    = MINADAPTER_POOLTAG;

    ntStatus = WdfDriverCreate(DriverObject,
                               RegistryPathName,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &config,
                               WDF_NO_HANDLE);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("WdfDriverCreate failed, 0x%x", ntStatus)),
        Done);

    //
    // Get registry configuration.
    //
    ntStatus = GetRegistrySettings(RegistryPathName);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("Registry Configuration error 0x%x", ntStatus)),
        Done);

    //
    // Tell the class driver to initialize the driver.
    //
    ntStatus =  PcInitializeAdapterDriver(DriverObject,
                                          RegistryPathName,
                                          (PDRIVER_ADD_DEVICE)AddDevice);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("PcInitializeAdapterDriver failed, 0x%x", ntStatus)),
        Done);

    RtlInitUnicodeString(&ntUnicodeString, NT_DEVICE_NAME);

    ntStatus = IoCreateDevice(
        DriverObject,                   // Our Driver Object
        0,                              // We don't use a device extension
        &ntUnicodeString,               // Device name "\Device\MICY"
        MICY_IOCTL_TYPE,                // Device type
        FILE_DEVICE_SECURE_OPEN,        // Device characteristics
        FALSE,                          // Not an exclusive device
        &deviceObject);                 // Returned ptr to Device Object

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("Couldn't create the device object\n")),
        Done);

    //
    // Initialize a Unicode String containing the Win32 name
    // for our device.
    //

    RtlInitUnicodeString(&ntWin32NameString, DOS_DEVICE_NAME);

    //
    // Create a symbolic link between our device name  and the Win32 name
    //

    ntStatus = IoCreateSymbolicLink(
        &ntWin32NameString, &ntUnicodeString);

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("Couldn't create symbolic link\n")),
        Done);

    //
    // Set up the control device's dispatch routines
    // These are set directly on the device object, not globally on the driver
    //
    deviceObject->Flags |= DO_BUFFERED_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    //
    // Store the control device object globally so we can identify it in handlers
    //
    g_ControlDeviceObject = deviceObject;
    // Initialize user PCM ring buffer (best-effort)
    (void)UserPcmBuffer_Init(7680 * 4);

    //
    // To intercept stop/remove/surprise-remove for audio devices.
    //
    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpHandler;

    //
    // Register handlers for the control device (CREATE/CLOSE)
    // Save original handlers first, then set ours
    //
    g_OriginalCreateHandler = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_OriginalCloseHandler = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    DriverObject->MajorFunction[IRP_MJ_CREATE] = ControlDeviceCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = ControlDeviceCreateClose;
    
    //
    // Register DeviceIoControl handler for the control device only
    // We check in DeviceControlHandler if it's the control device
    //
    deviceObject->DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControlHandler;

    //
    // Hook the port class unload function
    //
    gPCDriverUnloadRoutine = DriverObject->DriverUnload;
    DriverObject->DriverUnload = DriverUnload;

    //
    // All done.
    //
    ntStatus = STATUS_SUCCESS;
    
Done:

    if (!NT_SUCCESS(ntStatus))
    {
        if (WdfGetDriver() != NULL)
        {
            WdfDriverMiniportUnload(WdfGetDriver());
        }

        ReleaseRegistryStringBuffer();

        // Clean up control device if it was created
        if (g_ControlDeviceObject != NULL)
        {
            // Symbolic link was created, try to delete it
            UNICODE_STRING uniWin32NameString;
            RtlInitUnicodeString(&uniWin32NameString, DOS_DEVICE_NAME);
            IoDeleteSymbolicLink(&uniWin32NameString);  // Ignore error if already deleted
            IoDeleteDevice(g_ControlDeviceObject);
            g_ControlDeviceObject = NULL;
        }
        else if (deviceObject != NULL)
        {
            // Device was created but symbolic link wasn't created yet, so just delete device
            IoDeleteDevice(deviceObject);
        }
    }
    
    return ntStatus;
} // DriverEntry

#pragma code_seg()
// disable prefast warning 28152 because 
// DO_DEVICE_INITIALIZING is cleared in PcAddAdapterDevice
#pragma warning(disable:28152)
#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS AddDevice
( 
    _In_  PDRIVER_OBJECT    DriverObject,
    _In_  PDEVICE_OBJECT    PhysicalDeviceObject 
)
/*++

Routine Description:

  The Plug & Play subsystem is handing us a brand new PDO, for which we
  (by means of INF registration) have been asked to provide a driver.

  We need to determine if we need to be in the driver stack for the device.
  Create a function device object to attach to the stack
  Initialize that device object
  Return status success.

  All audio adapter drivers can use this code without change.

Arguments:

  DriverObject - pointer to a driver object

  PhysicalDeviceObject -  pointer to a device object created by the
                            underlying bus driver.

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    NTSTATUS        ntStatus;
    ULONG           maxObjects;

    DPF(D_TERSE, ("[AddDevice]"));

    maxObjects = g_MaxMiniports;

    // Tell the class driver to add the device.
    //
    ntStatus = 
        PcAddAdapterDevice
        ( 
            DriverObject,
            PhysicalDeviceObject,
            PCPFNSTARTDEVICE(StartDevice),
            maxObjects,
            0
        );

    // Note: Do NOT modify PhysicalDeviceObject->Flags - it's owned by the bus driver
    // Do NOT set DeviceControlHandler globally - it should only be for the control device
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    
    return ntStatus;
} // AddDevice

#pragma code_seg()
NTSTATUS
_IRQL_requires_max_(DISPATCH_LEVEL)
PowerControlCallback
(
    _In_        LPCGUID PowerControlCode,
    _In_opt_    PVOID   InBuffer,
    _In_        SIZE_T  InBufferSize,
    _Out_writes_bytes_to_(OutBufferSize, *BytesReturned) PVOID OutBuffer,
    _In_        SIZE_T  OutBufferSize,
    _Out_opt_   PSIZE_T BytesReturned,
    _In_opt_    PVOID   Context
)
{
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InBuffer);
    UNREFERENCED_PARAMETER(InBufferSize);
    UNREFERENCED_PARAMETER(OutBuffer);
    UNREFERENCED_PARAMETER(OutBufferSize);
    UNREFERENCED_PARAMETER(BytesReturned);
    UNREFERENCED_PARAMETER(Context);
    
    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
NTSTATUS
InstallEndpointCaptureFilters(
    _In_ PDEVICE_OBJECT     _pDeviceObject,
    _In_ PIRP               _pIrp,
    _In_ PADAPTERCOMMON     _pAdapterCommon,
    _In_ PENDPOINT_MINIPAIR _pAeMiniports
)
{
    NTSTATUS    ntStatus = STATUS_SUCCESS;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(_pDeviceObject);

    ntStatus = _pAdapterCommon->InstallEndpointFilters(
        _pIrp,
        _pAeMiniports,
        NULL,
        NULL,
        NULL,
        NULL, NULL);

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
InstallAllCaptureFilters(
    _In_ PDEVICE_OBJECT _pDeviceObject,
    _In_ PIRP           _pIrp,
    _In_ PADAPTERCOMMON _pAdapterCommon
)
{
    NTSTATUS            ntStatus;
    PENDPOINT_MINIPAIR* ppAeMiniports = g_CaptureEndpoints;

    PAGED_CODE();

    for (ULONG i = 0; i < g_cCaptureEndpoints; ++i, ++ppAeMiniports)
    {
        ntStatus = InstallEndpointCaptureFilters(_pDeviceObject, _pIrp, _pAdapterCommon, *ppAeMiniports);
        IF_FAILED_JUMP(ntStatus, Exit);
    }

    ntStatus = STATUS_SUCCESS;

Exit:
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
StartDevice
( 
    _In_  PDEVICE_OBJECT          DeviceObject,     
    _In_  PIRP                    Irp,              
    _In_  PRESOURCELIST           ResourceList      
)  
{
/*++

Routine Description:

  This function is called by the operating system when the device is 
  started.
  It is responsible for starting the miniports.  This code is specific to    
  the adapter because it calls out miniports for functions that are specific 
  to the adapter.                                                            

Arguments:

  DeviceObject - pointer to the driver object

  Irp - pointer to the irp 

  ResourceList - pointer to the resource list assigned by PnP manager

Return Value:

  NT status code.

--*/
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();

    ASSERT(DeviceObject);
    ASSERT(Irp);
    ASSERT(ResourceList);

    NTSTATUS                    ntStatus        = STATUS_SUCCESS;

    PADAPTERCOMMON              pAdapterCommon  = NULL;
    PUNKNOWN                    pUnknownCommon  = NULL;
    PortClassDeviceContext*     pExtension      = static_cast<PortClassDeviceContext*>(DeviceObject->DeviceExtension);

    DPF_ENTER(("[StartDevice]"));

    //
    // create a new adapter common object
    //
    ntStatus = NewAdapterCommon( 
                                &pUnknownCommon,
                                IID_IAdapterCommon,
                                NULL,
                                POOL_FLAG_NON_PAGED 
                                );
    IF_FAILED_JUMP(ntStatus, Exit);

    ntStatus = pUnknownCommon->QueryInterface( IID_IAdapterCommon,(PVOID *) &pAdapterCommon);
    IF_FAILED_JUMP(ntStatus, Exit);

    ntStatus = pAdapterCommon->Init(DeviceObject);
    IF_FAILED_JUMP(ntStatus, Exit);

    //
    // register with PortCls for power-management services
    ntStatus = PcRegisterAdapterPowerManagement( PUNKNOWN(pAdapterCommon), DeviceObject);
    IF_FAILED_JUMP(ntStatus, Exit);

    //
    // Install wave+topology filters for capture devices
    //
    ntStatus = InstallAllCaptureFilters(DeviceObject, Irp, pAdapterCommon);
    IF_FAILED_JUMP(ntStatus, Exit);

Exit:

    //
    // Stash the adapter common object in the device extension so
    // we can access it for cleanup on stop/removal.
    //
    if (pAdapterCommon)
    {
        ASSERT(pExtension != NULL);
        pExtension->m_pCommon = pAdapterCommon;
    }

    //
    // Release the adapter IUnknown interface.
    //
    SAFE_RELEASE(pUnknownCommon);
    
    return ntStatus;
} // StartDevice

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS 
PnpHandler
(
    _In_ DEVICE_OBJECT *_DeviceObject, 
    _Inout_ IRP *_Irp
)
/*++

Routine Description:

  Handles PnP IRPs                                                           

Arguments:

  _DeviceObject - Functional Device object pointer.

  _Irp - The Irp being passed

Return Value:

  NT status code.

--*/
{
    NTSTATUS                ntStatus = STATUS_UNSUCCESSFUL;
    IO_STACK_LOCATION      *stack;
    PortClassDeviceContext *ext;

    // Documented https://msdn.microsoft.com/en-us/library/windows/hardware/ff544039(v=vs.85).aspx
    // This method will be called in IRQL PASSIVE_LEVEL
#pragma warning(suppress: 28118)
    PAGED_CODE(); 

    ASSERT(_DeviceObject);
    ASSERT(_Irp);

    //
    // Check for the REMOVE_DEVICE irp.  If we're being unloaded, 
    // uninstantiate our devices and release the adapter common
    // object.
    //
    stack = IoGetCurrentIrpStackLocation(_Irp);

    switch (stack->MinorFunction)
    {
    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_STOP_DEVICE:
        ext = static_cast<PortClassDeviceContext*>(_DeviceObject->DeviceExtension);

        if (ext->m_pCommon != NULL)
        {
            ext->m_pCommon->Cleanup();
            
            ext->m_pCommon->Release();
            ext->m_pCommon = NULL;
        }
        break;

    default:
        break;
    }
    
    ntStatus = PcDispatchIrp(_DeviceObject, _Irp);

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
DeviceControlHandler
(
    _In_ DEVICE_OBJECT* _DeviceObject,
    _Inout_ IRP* _Irp
)
/*++

Routine Description:

  Handles DeviceIoControl IRPs for custom IOCTLs
  This handler is called for all devices, but only processes IOCTLs for the control device.
  Audio device IOCTLs are passed to PortCls.

Arguments:

  _DeviceObject - Device object pointer (could be control device or audio FDO).
  _Irp - The Irp being passed

Return Value:

  NT status code.

--*/
{
    NTSTATUS                ntStatus = STATUS_SUCCESS;
    PIO_STACK_LOCATION      stack;
    ULONG                   ioControlCode;
    ULONG                   inputBufferLength;
    ULONG                   outputBufferLength;
    PVOID                   systemBuffer;
    ULONG_PTR               bytesTransferred = 0;

    PAGED_CODE();

    ASSERT(_DeviceObject);
    ASSERT(_Irp);

    ////
    //// Check if this IRP is for our control device
    //// If not, pass it to PortCls for audio device handling
    ////
    //if (_DeviceObject != g_ControlDeviceObject)
    //{
    //    // This is an audio device, let PortCls handle it
    //    return PcDispatchIrp(_DeviceObject, _Irp);
    //}

    //
    // This is the control device - handle our custom IOCTLs
    //
    stack = IoGetCurrentIrpStackLocation(_Irp);
    ioControlCode = stack->Parameters.DeviceIoControl.IoControlCode;

    // Handle our custom IOCTL
    if (ioControlCode == IOCTL_SIMPLEAUDIOSAMPLE_SET_AUDIO_DATA)
    {
        __try {
            inputBufferLength = stack->Parameters.DeviceIoControl.InputBufferLength;
            outputBufferLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
            systemBuffer = _Irp->AssociatedIrp.SystemBuffer;

            // Validate buffer sizes
            if (inputBufferLength == 0)
            {
                ntStatus = STATUS_INVALID_PARAMETER;
                goto End;
            }

            // For METHOD_BUFFERED, systemBuffer is valid if either input or output length > 0
            if (systemBuffer == NULL && inputBufferLength > 0)
            {
                ntStatus = STATUS_INVALID_PARAMETER;
                goto End;
            }

            // Write received PCM to the ring buffer feeding capture stream
            ULONG written = UserPcmBuffer_Write((const UCHAR*)systemBuffer, inputBufferLength);
            DbgPrint("MICY.SYS: Received audio data: %lu bytes, written: %lu, buffered: %lu\n",
                inputBufferLength, written, UserPcmBuffer_Count());
            bytesTransferred = 0;  // No output data for this IOCTL

        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            DbgPrint("SIMPLEAUDIOSAMPLE: Exception in IOCTL handler\n");
            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
        }
    }
    else {
        // Unknown IOCTL for control device
        return PcDispatchIrp(_DeviceObject, _Irp);
    }

End:
    //
    // Complete the I/O operation
    //
    _Irp->IoStatus.Status = ntStatus;
    _Irp->IoStatus.Information = bytesTransferred;
    IoCompleteRequest(_Irp, IO_NO_INCREMENT);

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
ControlDeviceCreateClose
(
    _In_ PDEVICE_OBJECT _DeviceObject,
    _Inout_ PIRP _Irp
)
/*++

Routine Description:

  Handles Create and Close IRPs
  For the control device, simply succeed.
  For PortCls audio devices, call the original handler (if any) or pass to PortCls.

Arguments:

  _DeviceObject - Device object pointer (could be control device or FDO)
  _Irp - The Irp being passed

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(_DeviceObject);
    ASSERT(_Irp);

    //
    // Check if this is our control device
    //
    if (g_ControlDeviceObject != NULL && _DeviceObject == g_ControlDeviceObject)
    {
        // Control device - just succeed
        _Irp->IoStatus.Status = STATUS_SUCCESS;
        _Irp->IoStatus.Information = 0;
        IoCompleteRequest(_Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    //
    // This is NOT the control device
    // If it's a PortCls audio device (has DeviceExtension), PortCls/WDF handles
    // CREATE/CLOSE through its own mechanisms. We should not intercept these.
    // Call the original handler if it existed, otherwise let PortCls handle it.
    //
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(_Irp);
    DRIVER_DISPATCH* originalHandler = NULL;
    
    if (stack->MajorFunction == IRP_MJ_CREATE)
    {
        originalHandler = g_OriginalCreateHandler;
    }
    else if (stack->MajorFunction == IRP_MJ_CLOSE)
    {
        originalHandler = g_OriginalCloseHandler;
    }

    if (originalHandler != NULL)
    {
        // Call the original handler (likely PortCls's or default)
        return originalHandler(_DeviceObject, _Irp);
    }

    //
    // No original handler - this shouldn't happen, but handle gracefully
    // For PortCls devices, they handle CREATE/CLOSE through WDF device objects
    // Complete with success to allow normal operation
    //
    _Irp->IoStatus.Status = STATUS_SUCCESS;
    _Irp->IoStatus.Information = 0;
    IoCompleteRequest(_Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

#pragma code_seg()

