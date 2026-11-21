#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include "direwolf_interface.h"

#define HEARTBEAT_IN 0x81u
#define HEARTBEAT_OUT 0x05u
#define HEARTBEAT_COMMAND 0x10u
#define DIREWOLF_MODEL 0x84u
#define HEARTBEAT_REPLY_SIZE 32u
#define POOL_TAG 'wDbP'

static const GUID BatteryInterface = DIREWOLF_INTERFACE_GUID;
static const GUID BatteryDeviceClass =
    {0x03937f43, 0x6962, 0x4d84, {0xb3, 0xf9, 0x81, 0x3e, 0x67, 0xf7, 0x52, 0x69}};
// DEVPKEY_Device_InstanceId from devpkey.h, defined locally for the kernel linker.
static const DEVPROPKEY DeviceInstanceId =
    {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

typedef struct _DEVICE_CONTEXT {
    WDFQUEUE ConfigurationQueue;
    WDFWAITLOCK ConfigurationLock;
    KSPIN_LOCK Lock;
    KEVENT ReplyEvent;
    USBD_PIPE_HANDLE InputPipe;
    USBD_PIPE_HANDLE OutputPipe;
    BOOLEAN InD0;
    BOOLEAN Waiting;
    BOOLEAN ReplyValid;
    ULONGLONG LastStarted;
    DIREWOLF_BATTERY Reply;
} DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DirewolfAdd;
EVT_WDF_DEVICE_D0_ENTRY DirewolfD0Entry;
EVT_WDF_DEVICE_D0_EXIT DirewolfD0Exit;
EVT_WDF_DEVICE_RELEASE_HARDWARE DirewolfReleaseHardware;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL DirewolfUsbRequest;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL DirewolfConfigurationRequest;
EVT_WDF_REQUEST_COMPLETION_ROUTINE DirewolfInputComplete;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DirewolfQueryBattery;

static BOOLEAN IsDirewolf(_In_ WDFDEVICE Device)
{
    WCHAR buffer[128] = {0};
    ULONG used = 0;
    DEVPROPTYPE type = 0;
    NTSTATUS status = IoGetDevicePropertyData(WdfDeviceWdmGetPhysicalDevice(Device),
        &DeviceInstanceId, LOCALE_NEUTRAL, 0, sizeof(buffer), buffer, &used, &type);
    if (!NT_SUCCESS(status) || type != DEVPROP_TYPE_STRING ||
        used < sizeof(WCHAR) || used > sizeof(buffer) ||
        used % sizeof(WCHAR) != 0 || buffer[used / sizeof(WCHAR) - 1] != L'\0') {
        return FALSE;
    }
    DECLARE_CONST_UNICODE_STRING(expected, DIREWOLF_USB_INSTANCE);
    UNICODE_STRING instance;
    RtlInitUnicodeString(&instance, buffer);
    return RtlEqualUnicodeString(&expected, &instance, TRUE);
}

static NTSTATUS CreateBatteryPdo(_In_ WDFDEVICE Parent)
{
    DECLARE_CONST_UNICODE_STRING(id, L"PeripheralCharge\\DirewolfBattery");
    DECLARE_CONST_UNICODE_STRING(instance, L"0");
    DECLARE_CONST_UNICODE_STRING(access, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)");
    PWDFDEVICE_INIT init = WdfPdoInitAllocate(Parent);
    if (init == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = WdfPdoInitAssignRawDevice(init, &BatteryDeviceClass);
    if (NT_SUCCESS(status)) status = WdfDeviceInitAssignSDDLString(init, &access);
    if (NT_SUCCESS(status)) status = WdfPdoInitAssignDeviceID(init, &id);
    if (NT_SUCCESS(status)) status = WdfPdoInitAssignInstanceID(init, &instance);
    WDFDEVICE child = NULL;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    if (NT_SUCCESS(status)) status = WdfDeviceCreate(&init, &attrs, &child);
    if (init != NULL) WdfDeviceInitFree(init);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG config;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config, WdfIoQueueDispatchSequential);
    config.EvtIoDeviceControl = DirewolfQueryBattery;
    config.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(child, &config, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (NT_SUCCESS(status)) status = WdfDeviceCreateDeviceInterface(child, &BatteryInterface, NULL);
    if (NT_SUCCESS(status)) status = WdfFdoAddStaticChild(Parent, child);
    if (!NT_SUCCESS(status)) WdfObjectDelete(child);
    return status;
}

static VOID Forward(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(Device), &options)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

static VOID InvalidatePipes(_Inout_ DEVICE_CONTEXT* Context)
{
    KIRQL irql;
    KeAcquireSpinLock(&Context->Lock, &irql);
    Context->InputPipe = NULL;
    Context->OutputPipe = NULL;
    Context->Waiting = FALSE;
    Context->ReplyValid = FALSE;
    Context->LastStarted = 0;
    KeSetEvent(&Context->ReplyEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Context->Lock, irql);
}

static VOID RememberInterface(
    _Inout_ DEVICE_CONTEXT* Context,
    _In_ const USBD_INTERFACE_INFORMATION* Interface,
    _In_ ULONG Available)
{
    const ULONG headerSize = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    if (Available < headerSize || Interface->Length < headerSize || Interface->Length > Available ||
        Interface->NumberOfPipes > (Interface->Length - headerSize) / sizeof(USBD_PIPE_INFORMATION) ||
        Interface->InterfaceNumber != 0 || Interface->AlternateSetting != 0 ||
        Interface->Class != 0xff || Interface->SubClass != 0x5d || Interface->Protocol != 1) {
        return;
    }
    USBD_PIPE_HANDLE input = NULL, output = NULL;
    for (ULONG i = 0; i < Interface->NumberOfPipes; ++i) {
        const USBD_PIPE_INFORMATION* pipe = &Interface->Pipes[i];
        if (pipe->PipeType == UsbdPipeTypeInterrupt && pipe->MaximumPacketSize >= HEARTBEAT_REPLY_SIZE) {
            if (pipe->EndpointAddress == HEARTBEAT_IN) input = pipe->PipeHandle;
            if (pipe->EndpointAddress == HEARTBEAT_OUT) output = pipe->PipeHandle;
        }
    }
    KIRQL irql;
    KeAcquireSpinLock(&Context->Lock, &irql);
    Context->InputPipe = input;
    Context->OutputPipe = output;
    KeReleaseSpinLock(&Context->Lock, irql);
}

NTSTATUS DirewolfD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    DEVICE_CONTEXT* ctx = GetContext(Device);
    KIRQL irql;
    KeAcquireSpinLock(&ctx->Lock, &irql);
    ctx->InD0 = TRUE;
    KeReleaseSpinLock(&ctx->Lock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS DirewolfD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    DEVICE_CONTEXT* ctx = GetContext(Device);
    WdfWaitLockAcquire(ctx->ConfigurationLock, NULL);
    KIRQL irql;
    KeAcquireSpinLock(&ctx->Lock, &irql);
    ctx->InD0 = FALSE;
    ctx->Waiting = FALSE;
    ctx->ReplyValid = FALSE;
    ctx->LastStarted = 0;
    KeSetEvent(&ctx->ReplyEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&ctx->Lock, irql);
    WdfWaitLockRelease(ctx->ConfigurationLock);
    return STATUS_SUCCESS;
}

NTSTATUS DirewolfReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    DEVICE_CONTEXT* ctx = GetContext(Device);
    WdfWaitLockAcquire(ctx->ConfigurationLock, NULL);
    InvalidatePipes(ctx);
    WdfWaitLockRelease(ctx->ConfigurationLock);
    return STATUS_SUCCESS;
}

static PURB GetUrb(_In_ WDFREQUEST Request)
{
    return (PURB)IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request))->Parameters.Others.Argument1;
}

VOID DirewolfUsbRequest(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                       size_t InputLength, ULONG Code)
{
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* ctx = GetContext(device);
    PURB urb = Code == IOCTL_INTERNAL_USB_SUBMIT_URB ? GetUrb(Request) : NULL;
    const USHORT function = urb != NULL ? urb->UrbHeader.Function : 0xffff;

    if (function == URB_FUNCTION_SELECT_CONFIGURATION || function == URB_FUNCTION_SELECT_INTERFACE ||
        Code == IOCTL_INTERNAL_USB_RESET_PORT || Code == IOCTL_INTERNAL_USB_CYCLE_PORT) {
        // Serialize reconfiguration with our short OUT transfer, never with the wait for its reply.
        if (NT_SUCCESS(WdfRequestForwardToIoQueue(Request, ctx->ConfigurationQueue))) return;
        InvalidatePipes(ctx);
        Forward(device, Request);
        return;
    }
    if (function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
        urb->UrbHeader.Length >= sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER) &&
        (urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0) {
        KIRQL irql;
        KeAcquireSpinLock(&ctx->Lock, &irql);
        const BOOLEAN watched = ctx->InputPipe != NULL &&
            urb->UrbBulkOrInterruptTransfer.PipeHandle == ctx->InputPipe;
        KeReleaseSpinLock(&ctx->Lock, irql);
        if (watched) {
            WdfRequestFormatRequestUsingCurrentType(Request);
            WdfRequestSetCompletionRoutine(Request, DirewolfInputComplete, ctx);
            if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS))
                WdfRequestComplete(Request, WdfRequestGetStatus(Request));
            return;
        }
    }
    Forward(device, Request);
}

VOID DirewolfConfigurationRequest(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                                  size_t InputLength, ULONG Code)
{
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* ctx = GetContext(device);
    PURB urb = Code == IOCTL_INTERNAL_USB_SUBMIT_URB ? GetUrb(Request) : NULL;
    const USHORT function = urb != NULL ? urb->UrbHeader.Function : 0xffff;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);

    WdfWaitLockAcquire(ctx->ConfigurationLock, NULL);
    // A plain port reset preserves the configuration on success. A cycle does not.
    if (Code != IOCTL_INTERNAL_USB_RESET_PORT) InvalidatePipes(ctx);
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options);
    const NTSTATUS status = WdfRequestGetStatus(Request);
    if (NT_SUCCESS(status) && urb != NULL && USBD_SUCCESS(urb->UrbHeader.Status)) {
        if (function == URB_FUNCTION_SELECT_CONFIGURATION) {
            const ULONG at = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
            if (urb->UrbHeader.Length >= at && urb->UrbSelectConfiguration.ConfigurationDescriptor != NULL)
                RememberInterface(ctx, &urb->UrbSelectConfiguration.Interface, urb->UrbHeader.Length - at);
        } else if (function == URB_FUNCTION_SELECT_INTERFACE) {
            const ULONG at = FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface);
            if (urb->UrbHeader.Length >= at)
                RememberInterface(ctx, &urb->UrbSelectInterface.Interface, urb->UrbHeader.Length - at);
        }
    } else if (!NT_SUCCESS(status)) {
        InvalidatePipes(ctx);
    }
    WdfWaitLockRelease(ctx->ConfigurationLock);
    WdfRequestCompleteWithInformation(Request, status, WdfRequestGetInformation(Request));
}

VOID DirewolfInputComplete(WDFREQUEST Request, WDFIOTARGET Target,
                           PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    DEVICE_CONTEXT* ctx = (DEVICE_CONTEXT*)Context;
    if (NT_SUCCESS(Params->IoStatus.Status)) {
        PURB urb = GetUrb(Request);
        struct _URB_BULK_OR_INTERRUPT_TRANSFER* transfer = &urb->UrbBulkOrInterruptTransfer;
        if (USBD_SUCCESS(urb->UrbHeader.Status) && transfer->TransferBufferLength == HEARTBEAT_REPLY_SIZE) {
            const UCHAR* data = (const UCHAR*)transfer->TransferBuffer;
            if (data == NULL && transfer->TransferBufferMDL != NULL &&
                MmGetMdlByteCount(transfer->TransferBufferMDL) >= HEARTBEAT_REPLY_SIZE) {
                data = MmGetSystemAddressForMdlSafe(transfer->TransferBufferMDL,
                    NormalPagePriority | MdlMappingNoExecute);
            }
            if (data != NULL && data[0] == 0 && data[1] == 0x14 &&
                data[15] == HEARTBEAT_COMMAND && data[16] == DIREWOLF_MODEL &&
                (data[23] & 0x0f) <= 5 && (data[23] >> 4) <= 1) {
                KIRQL irql;
                KeAcquireSpinLock(&ctx->Lock, &irql);
                if (ctx->Waiting && transfer->PipeHandle == ctx->InputPipe) {
                    ctx->Reply.Version = DIREWOLF_BATTERY_VERSION;
                    ctx->Reply.Level = data[23] & 0x0f;
                    ctx->Reply.Charging = (data[23] >> 4) == 1;
                    ctx->Reply.Reserved = 0;
                    ctx->ReplyValid = TRUE;
                    ctx->Waiting = FALSE;
                    KeSetEvent(&ctx->ReplyEvent, IO_NO_INCREMENT, FALSE);
                }
                KeReleaseSpinLock(&ctx->Lock, irql);
            }
        }
    }
    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

static NTSTATUS SendHeartbeat(_In_ WDFDEVICE Device, _In_ USBD_PIPE_HANDLE Pipe)
{
    typedef struct _HEARTBEAT_TRANSFER {
        struct _URB_BULK_OR_INTERRUPT_TRANSFER Urb;
        UCHAR Packet[15];
    } HEARTBEAT_TRANSFER;
    HEARTBEAT_TRANSFER* transfer = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*transfer), POOL_TAG);
    if (transfer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    transfer->Packet[0] = 0xa5;
    transfer->Packet[1] = HEARTBEAT_COMMAND;
    transfer->Packet[2] = (UCHAR)(KeQueryInterruptTime() % 255);
    UsbBuildInterruptOrBulkTransferRequest((PURB)&transfer->Urb, sizeof(transfer->Urb), Pipe,
        transfer->Packet, NULL, sizeof(transfer->Packet), USBD_TRANSFER_DIRECTION_OUT, NULL);

    WDF_MEMORY_DESCRIPTOR urb;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&urb, &transfer->Urb, sizeof(transfer->Urb));
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(250));
    NTSTATUS status = WdfIoTargetSendInternalIoctlOthersSynchronously(WdfDeviceGetIoTarget(Device),
        WDF_NO_HANDLE, IOCTL_INTERNAL_USB_SUBMIT_URB, &urb, NULL, NULL, &options, NULL);
    if (NT_SUCCESS(status) && (!USBD_SUCCESS(transfer->Urb.Hdr.Status) ||
        transfer->Urb.TransferBufferLength != sizeof(transfer->Packet))) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    ExFreePoolWithTag(transfer, POOL_TAG);
    return status;
}

VOID DirewolfQueryBattery(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                         size_t InputLength, ULONG Code)
{
    UNREFERENCED_PARAMETER(OutputLength);
    if (Code != DIREWOLF_QUERY_BATTERY || InputLength != 0) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    DIREWOLF_BATTERY* output;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID*)&output, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    WDFDEVICE device = WdfPdoGetParent(WdfIoQueueGetDevice(Queue));
    DEVICE_CONTEXT* ctx = GetContext(device);
    KIRQL irql;
    WdfWaitLockAcquire(ctx->ConfigurationLock, NULL);
    KeAcquireSpinLock(&ctx->Lock, &irql);
    const USBD_PIPE_HANDLE pipe = ctx->InD0 && ctx->InputPipe != NULL ? ctx->OutputPipe : NULL;
    const ULONGLONG now = KeQueryInterruptTime();
    if (pipe != NULL && ctx->LastStarted != 0 && now - ctx->LastStarted < 20 * 1000 * 1000) {
        status = ctx->ReplyValid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
        if (ctx->ReplyValid) *output = ctx->Reply;
        KeReleaseSpinLock(&ctx->Lock, irql);
        WdfWaitLockRelease(ctx->ConfigurationLock);
        WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? sizeof(*output) : 0);
        return;
    }
    if (pipe != NULL) {
        ctx->LastStarted = now;
        ctx->ReplyValid = FALSE;
        ctx->Waiting = TRUE;
        KeClearEvent(&ctx->ReplyEvent);
    }
    KeReleaseSpinLock(&ctx->Lock, irql);
    status = pipe != NULL ? SendHeartbeat(device, pipe) : STATUS_DEVICE_NOT_READY;
    WdfWaitLockRelease(ctx->ConfigurationLock);

    if (NT_SUCCESS(status)) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -6500000; // 650 ms, in relative 100 ns units.
        status = KeWaitForSingleObject(&ctx->ReplyEvent, Executive, KernelMode, FALSE, &timeout);
        if (status == STATUS_TIMEOUT) status = STATUS_IO_TIMEOUT;
    }
    KeAcquireSpinLock(&ctx->Lock, &irql);
    if (NT_SUCCESS(status)) {
        if (ctx->ReplyValid) *output = ctx->Reply;
        else status = STATUS_DEVICE_NOT_READY;
    }
    ctx->Waiting = FALSE;
    KeReleaseSpinLock(&ctx->Lock, irql);
    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? sizeof(*output) : 0);
}

NTSTATUS DirewolfAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    UNREFERENCED_PARAMETER(Driver);
    WdfFdoInitSetFilter(Init);

    // Initialize state during add, well before the first PnP or power callback.
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = DirewolfD0Entry;
    pnp.EvtDeviceD0Exit = DirewolfD0Exit;
    pnp.EvtDeviceReleaseHardware = DirewolfReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&Init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    DEVICE_CONTEXT* ctx = GetContext(device);
    KeInitializeSpinLock(&ctx->Lock);
    KeInitializeEvent(&ctx->ReplyEvent, NotificationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctx->ConfigurationLock);
    if (!NT_SUCCESS(status)) return status;
    if (!IsDirewolf(device)) return STATUS_SUCCESS;

    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfFalse;
    queue.EvtIoInternalDeviceControl = DirewolfConfigurationRequest;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queue, &attributes, &ctx->ConfigurationQueue);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.PowerManaged = WdfFalse;
    queue.EvtIoInternalDeviceControl = DirewolfUsbRequest;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    // Battery diagnostics are optional. Failure to expose the child must not block XInput.
    (VOID)CreateBatteryPdo(device);
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, DirewolfAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
