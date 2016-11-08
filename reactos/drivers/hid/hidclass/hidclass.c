/*
 * PROJECT:     ReactOS Universal Serial Bus Human Interface Device Driver
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/hid/hidclass/hidclass.c
 * PURPOSE:     HID Class Driver
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static LPWSTR ClientIdentificationAddress = L"HIDCLASS";
static ULONG HidClassDeviceNumber = 0;

NTSTATUS
NTAPI
DllInitialize(
    IN PUNICODE_STRING RegistryPath)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

//create and attach！
NTSTATUS
NTAPI
HidClassAddDevice( //这是主业，将在minidriver注册时当成特洛伊
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject) //不见得一定是物理对象，只要在设备堆栈中的就行
{
    WCHAR CharDeviceName[64];
    NTSTATUS Status;
    UNICODE_STRING DeviceName;
    PDEVICE_OBJECT NewDeviceObject;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    ULONG DeviceExtensionSize;
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;

    /* increment device number */
    InterlockedIncrement((PLONG)&HidClassDeviceNumber);

    /* construct device name */
    swprintf(CharDeviceName, L"\\Device\\_HID%08x", HidClassDeviceNumber);

    /* initialize device name */
    RtlInitUnicodeString(&DeviceName, CharDeviceName); //等会创建设备时用

    /* get driver object extension */
    DriverExtension = IoGetDriverObjectExtension(DriverObject, ClientIdentificationAddress);//将被保存在设备扩展的common字段中，要不找不到mini的派遣函数
    if (!DriverExtension)
    {
        /* device removed */
        ASSERT(FALSE);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* calculate device extension size */
    DeviceExtensionSize = sizeof(HIDCLASS_FDO_EXTENSION) + DriverExtension->DeviceExtensionSize;//二合一

    /* now create the device */
    Status = IoCreateDevice(DriverObject, DeviceExtensionSize, &DeviceName, FILE_DEVICE_UNKNOWN, 0, FALSE, &NewDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* failed to create device object */
        ASSERT(FALSE);
        return Status;
    }

    /* get device extension */
    FDODeviceExtension = NewDeviceObject->DeviceExtension;//fdo的设备扩展在前，mini的设备扩展在后

    /* zero device extension */
    RtlZeroMemory(FDODeviceExtension, sizeof(HIDCLASS_FDO_EXTENSION));

    /* initialize device extension */
    FDODeviceExtension->Common.IsFDO = TRUE; //AddDevice得来的一定是fdo?可能吧
    FDODeviceExtension->Common.DriverExtension = DriverExtension;
	
	//下面设置著名的HID_DEVICE_EXTENTION结构，这个结构给minidriver的直通派遣函数用，本HIDClass也用
	//都是把IRP包转发给minidriver的下层，说转发给minidriver的下层也就是转发给HIDClass的下层
	//某种意义上说：HIDClass和minidriver是一个意思，只是HIDClass只为该minidriver服务而已
	//HID_DEVICE_EXTENTION代表了HIDClass和minidriver的联系，这种联系是上下层的关系，很紧密，并且带有minidriver的设备扩展
    FDODeviceExtension->Common.HidDeviceExtension.PhysicalDeviceObject = PhysicalDeviceObject;
    FDODeviceExtension->Common.HidDeviceExtension.MiniDeviceExtension = (PVOID)((ULONG_PTR)FDODeviceExtension + sizeof(HIDCLASS_FDO_EXTENSION));
    FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject = IoAttachDeviceToDeviceStack(NewDeviceObject, PhysicalDeviceObject);
    
	if (FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject == NULL)
    {
        /* no PDO */
        IoDeleteDevice(NewDeviceObject);
        DPRINT1("[HIDCLASS] failed to attach to device stack\n");
        return STATUS_DEVICE_REMOVED;
    }

    /* sanity check */
    ASSERT(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject);

    /* increment stack size */
    NewDeviceObject->StackSize++;//为什么？因为执行了IoAttachDeviceToDeviceStack

    /* init device object */
    NewDeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    NewDeviceObject->Flags  &= ~DO_DEVICE_INITIALIZING;

    /* now call driver provided add device routine */
    ASSERT(DriverExtension->AddDevice != 0);
    Status = DriverExtension->AddDevice(DriverObject, NewDeviceObject);//minidriver有什么要做的
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        DPRINT1("HIDCLASS: AddDevice failed with %x\n", Status);
        IoDetachDevice(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject);
        IoDeleteDevice(NewDeviceObject);
        return Status;
    }

    /* succeeded */
    return Status;
}

VOID
NTAPI
HidClassDriverUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    UNIMPLEMENTED
}

//主要创建每fileobject的context，保存在FileObject->FsContext
NTSTATUS
NTAPI
HidClass_Create(
    IN PDEVICE_OBJECT DeviceObject, //pdo
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;
    PHIDCLASS_FILEOP_CONTEXT Context;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;
    if (CommonDeviceExtension->IsFDO)
    {
#ifndef __REACTOS__

         //
         // only supported for PDO
         //
         Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return STATUS_UNSUCCESSFUL;
#else
         //
         // ReactOS PnP manager [...]
         //
         DPRINT1("[HIDCLASS] PnP HACK\n");
         Irp->IoStatus.Status = STATUS_SUCCESS;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return STATUS_SUCCESS;
#endif
    }

    //
    // must be a PDO
    //
    ASSERT(CommonDeviceExtension->IsFDO == FALSE);

    //
    // get device extension
    //
    PDODeviceExtension = DeviceObject->DeviceExtension;

    //
    // get stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    DPRINT("ShareAccess %x\n", IoStack->Parameters.Create.ShareAccess);
    DPRINT("Options %x\n", IoStack->Parameters.Create.Options);
    DPRINT("DesiredAccess %x\n", IoStack->Parameters.Create.SecurityContext->DesiredAccess);

    //
    // allocate HIDCLASS_FILEOP_CONTEXT，无论是键盘还是鼠标，这个结构都是一样大小
    //
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(HIDCLASS_FILEOP_CONTEXT), HIDCLASS_TAG);
    if (!Context)
    {
        //
        // no memory
        //
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // init context
    //
    RtlZeroMemory(Context, sizeof(HIDCLASS_FILEOP_CONTEXT));
    Context->DeviceExtension = PDODeviceExtension; //找collection信息有用
    KeInitializeSpinLock(&Context->Lock);
    InitializeListHead(&Context->ReadPendingIrpListHead);
    InitializeListHead(&Context->IrpCompletedListHead);
    KeInitializeEvent(&Context->IrpReadComplete, NotificationEvent, FALSE);

    //
    // store context
    //
    ASSERT(IoStack->FileObject);
    IoStack->FileObject->FsContext = Context; //每fileobject

    //
    // done
    //
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

//通过irp得到FsContext以后就大开杀戒
NTSTATUS
NTAPI
HidClass_Close(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHIDCLASS_FILEOP_CONTEXT IrpContext;
    BOOLEAN IsRequestPending = FALSE;
    KIRQL OldLevel;
    PLIST_ENTRY Entry;
    PIRP ListIrp;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // is it a FDO request
    //
    if (CommonDeviceExtension->IsFDO)
    {
        //
        // how did the request get there
        //
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER_1;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER_1;
    }

    //
    // get stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // sanity checks
    //
    ASSERT(IoStack->FileObject);
    ASSERT(IoStack->FileObject->FsContext);

    //
    // get irp context，下面看到每fileobject的context有什么用！
    //
    IrpContext = IoStack->FileObject->FsContext;
    ASSERT(IrpContext);

    //
    // acquire lock
    //
    KeAcquireSpinLock(&IrpContext->Lock, &OldLevel);

    if (!IsListEmpty(&IrpContext->ReadPendingIrpListHead))
    {
        //
        // FIXME cancel irp
        //
        IsRequestPending = TRUE;
    }

    //
    // signal stop
    //
    IrpContext->StopInProgress = TRUE;

    //
    // release lock
    //
    KeReleaseSpinLock(&IrpContext->Lock, OldLevel);

    if (IsRequestPending)
    {
        //
        // wait for request to complete
        //
        DPRINT1("[HIDCLASS] Waiting for read irp completion...\n");
        KeWaitForSingleObject(&IrpContext->IrpReadComplete, Executive, KernelMode, FALSE, NULL);
    }

    //
    // acquire lock
    //
    KeAcquireSpinLock(&IrpContext->Lock, &OldLevel);

    //
    // sanity check
    //
    ASSERT(IsListEmpty(&IrpContext->ReadPendingIrpListHead));

    //
    // now free all irps
    //
    while (!IsListEmpty(&IrpContext->IrpCompletedListHead))
    {
        //
        // remove head irp
        //
        Entry = RemoveHeadList(&IrpContext->IrpCompletedListHead);

        //
        // get irp
        //
        ListIrp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        //
        // free the irp
        //
        IoFreeIrp(ListIrp);
    }

    //
    // release lock
    //
    KeReleaseSpinLock(&IrpContext->Lock, OldLevel);

    //
    // remove context
    //
    IoStack->FileObject->FsContext = NULL;

    //
    // free context
    //
    ExFreePoolWithTag(IrpContext, HIDCLASS_TAG);

    //
    // complete request
    //
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

//完成函数返回STATUS_MORE_PROCESSING_REQUIRED的例子
NTSTATUS
NTAPI
HidClass_ReadCompleteIrp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP newIrp,
    IN PVOID Ctx)
{
    PHIDCLASS_IRP_CONTEXT IrpContext;
    KIRQL OldLevel;
    PUCHAR Address;
    ULONG Offset;
    PHIDP_COLLECTION_DESC CollectionDescription;
    PHIDP_REPORT_IDS ReportDescription;
    BOOLEAN IsEmpty;

    //
    // get irp context
    //
    IrpContext = Ctx;

    DPRINT("HidClass_ReadCompleteIrp Irql %lu\n", KeGetCurrentIrql());
    DPRINT("HidClass_ReadCompleteIrp Status %lx\n", newIrp->IoStatus.Status);
    DPRINT("HidClass_ReadCompleteIrp Length %lu\n", newIrp->IoStatus.Information);
    DPRINT("HidClass_ReadCompleteIrp newIrp %p\n", newIrp);
    DPRINT("HidClass_ReadCompleteIrp InputReportBuffer %p\n", IrpContext->InputReportBuffer);
    DPRINT("HidClass_ReadCompleteIrp InputReportBufferLength %li\n", IrpContext->InputReportBufferLength);
    DPRINT("HidClass_ReadCompleteIrp OriginalIrp %p\n", IrpContext->OriginalIrp);

    //
    // copy result
    //
    if (newIrp->IoStatus.Information)
    {
        //
        // get address
        //
        Address = MmGetSystemAddressForMdlSafe(IrpContext->OriginalIrp->MdlAddress, NormalPagePriority);
        if (Address)
        {
            //
            // reports may have a report id prepended
            //
            Offset = 0;

            //
            // get collection description
            //
            CollectionDescription = HidClassPDO_GetCollectionDescription(&IrpContext->FileOp->DeviceExtension->Common.DeviceDescription,
                                                                         IrpContext->FileOp->DeviceExtension->CollectionNumber);
            ASSERT(CollectionDescription);

            //
            // get report description
            //
            ReportDescription = HidClassPDO_GetReportDescription(&IrpContext->FileOp->DeviceExtension->Common.DeviceDescription,
                                                                 IrpContext->FileOp->DeviceExtension->CollectionNumber);
            ASSERT(ReportDescription);

            if (CollectionDescription && ReportDescription)
            {
                //
                // calculate offset
                //
                ASSERT(CollectionDescription->InputLength >= ReportDescription->InputLength);
                Offset = CollectionDescription->InputLength - ReportDescription->InputLength;
            }

            //
            // copy result to OriginalIrp's buffer
            //
            RtlCopyMemory(&Address[Offset], IrpContext->InputReportBuffer, IrpContext->InputReportBufferLength);
        }
    }

    //
    // copy result status OriginalIrp's IoStatus
    //
    IrpContext->OriginalIrp->IoStatus.Status = newIrp->IoStatus.Status;
    IrpContext->OriginalIrp->IoStatus.Information = newIrp->IoStatus.Information;

    //
    // free input report buffer
    //
    ExFreePoolWithTag(IrpContext->InputReportBuffer, HIDCLASS_TAG);

    //
    // remove us from pending list
    //
    KeAcquireSpinLock(&IrpContext->FileOp->Lock, &OldLevel);

    //
    // remove from pending list
    //
    RemoveEntryList(&newIrp->Tail.Overlay.ListEntry);

    //
    // is list empty
    //
    IsEmpty = IsListEmpty(&IrpContext->FileOp->ReadPendingIrpListHead);

    //
    // insert into completed list
    //
    InsertTailList(&IrpContext->FileOp->IrpCompletedListHead, &newIrp->Tail.Overlay.ListEntry);

    //
    // release lock
    //
    KeReleaseSpinLock(&IrpContext->FileOp->Lock, OldLevel);

    //
    // complete original request
    //
    IoCompleteRequest(IrpContext->OriginalIrp, IO_NO_INCREMENT);


    DPRINT("StopInProgress %x IsEmpty %x\n", IrpContext->FileOp->StopInProgress, IsEmpty);
    if (IrpContext->FileOp->StopInProgress && IsEmpty)
    {
        //
        // last pending irp
        //
        DPRINT1("[HIDCLASS] LastPendingTransfer Signalling\n");
        KeSetEvent(&IrpContext->FileOp->IrpReadComplete, 0, FALSE);
    }


    //
    // free irp context
    //
    ExFreePoolWithTag(IrpContext, HIDCLASS_TAG);

    //
    // done
    //
    return STATUS_MORE_PROCESSING_REQUIRED; //难道为了irp重用？是，挂在Ctx->FileOp->IrpCompletedListHead
}

PIRP
HidClass_GetIrp(
    IN PHIDCLASS_FILEOP_CONTEXT Context)
{
   KIRQL OldLevel;
   PIRP Irp = NULL;
   PLIST_ENTRY ListEntry;

    //
    // acquire lock
    //
    KeAcquireSpinLock(&Context->Lock, &OldLevel);

    //
    // is list empty?
    //
    if (!IsListEmpty(&Context->IrpCompletedListHead))
    {
        //
        // grab first entry
        //
        ListEntry = RemoveHeadList(&Context->IrpCompletedListHead);

        //
        // get irp
        //
        Irp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);
    }

    //
    // release lock
    //
    KeReleaseSpinLock(&Context->Lock, OldLevel);

    //
    // done
    //
    return Irp;
}

//创建控制包，目前只有HidClass_Read会用到
// 深刻理解irpcontext：（1）作为完成函数的参数，如此伴随irp；（2）相当于不适用iostack的替代
NTSTATUS
HidClass_BuildIrp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP RequestIrp,
    IN PHIDCLASS_FILEOP_CONTEXT Context, //每文件的context,指针将被保存在OutIrpContext中
    IN ULONG DeviceIoControlCode,
    IN ULONG BufferLength,
    OUT PIRP *OutIrp,
    OUT PHIDCLASS_IRP_CONTEXT *OutIrpContext) //重用irp的相关信息（OriginalIrp、InputReportBuffer等），在完成时用
{
    PIRP Irp;
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_IRP_CONTEXT IrpContext;
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;
    PHIDP_COLLECTION_DESC CollectionDescription;
    PHIDP_REPORT_IDS ReportDescription;

    //
    // get an irp from fresh list
    //
    Irp = HidClass_GetIrp(Context);
    if (!Irp)
    {
        //
        // build new irp
        //
        Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
        if (!Irp)
        {
            //
            // no memory
            //
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    else
    {
        //
        // re-use irp
        //
        IoReuseIrp(Irp, STATUS_SUCCESS);
    }

    //
    // allocate completion context
    // 第一次看到completion context的概念，好好理解
    IrpContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(HIDCLASS_IRP_CONTEXT), HIDCLASS_TAG);
    if (!IrpContext)
    {
        //
        // no memory
        //
        IoFreeIrp(Irp);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // get device extension
    //
    PDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(PDODeviceExtension->Common.IsFDO == FALSE);

    //
    // init irp context
    //
    RtlZeroMemory(IrpContext, sizeof(HIDCLASS_IRP_CONTEXT));
    IrpContext->OriginalIrp = RequestIrp;
    IrpContext->FileOp = Context;

    //
    // get collection description
    //
    CollectionDescription = HidClassPDO_GetCollectionDescription(&IrpContext->FileOp->DeviceExtension->Common.DeviceDescription,
                                                                 IrpContext->FileOp->DeviceExtension->CollectionNumber);
    ASSERT(CollectionDescription);

    //
    // get report description，为了得到report报告的长度即ReportDescription->InputLength
    //
    ReportDescription = HidClassPDO_GetReportDescription(&IrpContext->FileOp->DeviceExtension->Common.DeviceDescription,
                                                         IrpContext->FileOp->DeviceExtension->CollectionNumber);
    ASSERT(ReportDescription);

    //
    // sanity check
    //
    ASSERT(CollectionDescription->InputLength >= ReportDescription->InputLength);//找ReportDescription->InputLength

    if (Context->StopInProgress) //这里是个不错的检查点
    {
         //
         // stop in progress
         //
         DPRINT1("[HIDCLASS] Stop In Progress\n");
         Irp->IoStatus.Status = STATUS_CANCELLED;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return STATUS_CANCELLED;

    }

    //
    // store report length
    //
    IrpContext->InputReportBufferLength = ReportDescription->InputLength;

    //
    // allocate buffer
    //
    IrpContext->InputReportBuffer = ExAllocatePoolWithTag(NonPagedPool, IrpContext->InputReportBufferLength, HIDCLASS_TAG);
    if (!IrpContext->InputReportBuffer)
    {
        //
        // no memory
        //
        IoFreeIrp(Irp);
        ExFreePoolWithTag(IrpContext, HIDCLASS_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // get stack location
    //
    IoStack = IoGetNextIrpStackLocation(Irp);

    //
    // init stack location
    //
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = DeviceIoControlCode; //输入参数
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = IrpContext->InputReportBufferLength;//上面刚刚取得
    IoStack->Parameters.DeviceIoControl.InputBufferLength = 0;
    IoStack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;
    Irp->UserBuffer = IrpContext->InputReportBuffer;
    IoStack->DeviceObject = DeviceObject;

    //
    // store result
    //
    *OutIrp = Irp;
    *OutIrpContext = IrpContext;

    //
    // done
    //
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidClass_Read(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_FILEOP_CONTEXT Context;
    KIRQL OldLevel;
    NTSTATUS Status;
    PIRP NewIrp;
    PHIDCLASS_IRP_CONTEXT NewIrpContext;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;

    //
    // get current stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(CommonDeviceExtension->IsFDO == FALSE);

    //
    // sanity check
    //
    //ASSERT(IoStack->FileObject);
    //ASSERT(IoStack->FileObject->FsContext);

    //
    // get context
    //
    Context = IoStack->FileObject->FsContext; //很好，注意理解，将随irp走
    //ASSERT(Context);

    //
    // FIXME support polled devices
    //
    //ASSERT(Context->DeviceExtension->Common.DriverExtension->DevicesArePolled == FALSE);

    if (Context->StopInProgress)
    {
        //
        // stop in progress
        //
        DPRINT1("[HIDCLASS] Stop In Progress\n");
        Irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }

    //
    // build irp request
    //
    Status = HidClass_BuildIrp(DeviceObject,
                               Irp,
                               Context,//将随irpcontext走
                               IOCTL_HID_READ_REPORT, //注意构造IOCTL包，为什么是IOCTL包，不是IPR_MAJOR_READ?
                               IoStack->Parameters.Read.Length,
                               &NewIrp,
                               &NewIrpContext);
    if (!NT_SUCCESS(Status))
    {
        //
        // failed
        //
        DPRINT1("HidClass_BuildIrp failed with %x\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    //
    // acquire lock
    //
    KeAcquireSpinLock(&Context->Lock, &OldLevel);

    //
    // insert irp into pending list
    //
    InsertTailList(&Context->ReadPendingIrpListHead, &NewIrp->Tail.Overlay.ListEntry);

    //
    // set completion routine
    //
    IoSetCompletionRoutine(NewIrp, HidClass_ReadCompleteIrp, NewIrpContext, TRUE, TRUE, TRUE);

    //
    // make next location current
    //
    IoSetNextIrpStackLocation(NewIrp);

    //
    // release spin lock
    //
    KeReleaseSpinLock(&Context->Lock, OldLevel);

    //
    // mark irp pending
    //
    IoMarkIrpPending(Irp);

    //-----------------------------
    // let's dispatch the request
    //-----------------------------
	//看出来context的好处：我们现在在pdo里面，要找到fdo才能下传请求包
	//问题:什么要通过这种形式下传？
	//答案：
    //ASSERT(Context->DeviceExtension);
    Status = Context->DeviceExtension->Common.DriverExtension->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL](Context->DeviceExtension->FDODeviceObject, NewIrp);

	//二传：这一传是把newirp发出去了，原来的irp自然就pending了，在二传中，即在newirp的完成函数中继续irp
 
    return STATUS_PENDING;
}

NTSTATUS
NTAPI
HidClass_Write(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    UNIMPLEMENTED
    ASSERT(FALSE);
    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

//主要为了上层可以得到CollectionDescription->PreparsedData
NTSTATUS
NTAPI
HidClass_DeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHID_COLLECTION_INFORMATION CollectionInformation;
    PHIDP_COLLECTION_DESC CollectionDescription;
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // only PDO are supported
    //
    if (CommonDeviceExtension->IsFDO)
    {
        //
        // invalid request
        //
        DPRINT1("[HIDCLASS] DeviceControl Irp for FDO arrived\n");
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER_1;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER_1;
    }

    ASSERT(CommonDeviceExtension->IsFDO == FALSE);

    //
    // get pdo device extension
    //
    PDODeviceExtension = DeviceObject->DeviceExtension;

    //
    // get stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_HID_GET_COLLECTION_INFORMATION:
        {
            //
            // check if output buffer is big enough
            //
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_COLLECTION_INFORMATION))
            {
                //
                // invalid buffer size
                //
                Irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_BUFFER_SIZE;
            }

            //
            // get output buffer
            //
            CollectionInformation = Irp->AssociatedIrp.SystemBuffer;
            ASSERT(CollectionInformation);

            //
            // get collection description
            //
            CollectionDescription = HidClassPDO_GetCollectionDescription(&CommonDeviceExtension->DeviceDescription,
                                                                         PDODeviceExtension->CollectionNumber);
            ASSERT(CollectionDescription);

            //
            // init result buffer
            //
            CollectionInformation->DescriptorSize = CollectionDescription->PreparsedDataLength;
            CollectionInformation->Polled = CommonDeviceExtension->DriverExtension->DevicesArePolled;
            CollectionInformation->VendorID = CommonDeviceExtension->Attributes.VendorID;
            CollectionInformation->ProductID = CommonDeviceExtension->Attributes.ProductID;
            CollectionInformation->VersionNumber = CommonDeviceExtension->Attributes.VersionNumber;

            //
            // complete request
            //
            Irp->IoStatus.Information = sizeof(HID_COLLECTION_INFORMATION);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
        {
            //
            // get collection description
            //
            CollectionDescription = HidClassPDO_GetCollectionDescription(&CommonDeviceExtension->DeviceDescription,
                                                                         PDODeviceExtension->CollectionNumber);
            ASSERT(CollectionDescription);

            //
            // check if output buffer is big enough
            //
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < CollectionDescription->PreparsedDataLength)
            {
                //
                // invalid buffer size
                //
                Irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_BUFFER_SIZE;
            }

            //
            // copy result
            //
            ASSERT(Irp->UserBuffer);
            RtlCopyMemory(Irp->UserBuffer, CollectionDescription->PreparsedData, CollectionDescription->PreparsedDataLength);

            //
            // complete request
            //
            Irp->IoStatus.Information = CollectionDescription->PreparsedDataLength;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        default:
        {
            DPRINT1("[HIDCLASS] DeviceControl IoControlCode 0x%x not implemented\n", IoStack->Parameters.DeviceIoControl.IoControlCode);
            Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_IMPLEMENTED;
        }
    }
}

//不支持
NTSTATUS
NTAPI
HidClass_InternalDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    UNIMPLEMENTED
    ASSERT(FALSE);
    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

//对pdo，完成
//对fdo，往下传
NTSTATUS
NTAPI
HidClass_Power(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    CommonDeviceExtension = DeviceObject->DeviceExtension;
 
    if (CommonDeviceExtension->IsFDO)
    {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        return HidClassFDO_DispatchRequest(DeviceObject, Irp);
    }
    else
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
}

NTSTATUS
NTAPI
HidClass_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;

    //
    // get common device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // check type of device object
    //
    if (CommonDeviceExtension->IsFDO)
    {
        //
        // handle request
        //
        return HidClassFDO_PnP(DeviceObject, Irp);
    }
    else
    {
        //
        // handle request
        //
        return HidClassPDO_PnP(DeviceObject, Irp);
    }
}

NTSTATUS
NTAPI
HidClass_DispatchDefault(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;

    //
    // get common device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // FIXME: support PDO
    //
    ASSERT(CommonDeviceExtension->IsFDO == TRUE);

    //
    // skip current irp stack location
    //
    IoSkipCurrentIrpStackLocation(Irp);

    //
    // dispatch to lower device object
    //
    return IoCallDriver(CommonDeviceExtension->HidDeviceExtension.NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
HidClassDispatch(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;

    //
    // get current stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    DPRINT("[HIDCLASS] Dispatch Major %x Minor %x\n", IoStack->MajorFunction, IoStack->MinorFunction);

    //
    // dispatch request based on major function
    //
    switch (IoStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            return HidClass_Create(DeviceObject, Irp);
        case IRP_MJ_CLOSE:
            return HidClass_Close(DeviceObject, Irp);
        case IRP_MJ_READ:
            return HidClass_Read(DeviceObject, Irp);
        case IRP_MJ_WRITE:
            return HidClass_Write(DeviceObject, Irp);
        case IRP_MJ_DEVICE_CONTROL:
            return HidClass_DeviceControl(DeviceObject, Irp);
        case IRP_MJ_INTERNAL_DEVICE_CONTROL:
           return HidClass_InternalDeviceControl(DeviceObject, Irp);
        case IRP_MJ_POWER:
            return HidClass_Power(DeviceObject, Irp);
        case IRP_MJ_PNP:
            return HidClass_PnP(DeviceObject, Irp);
        default:
            return HidClass_DispatchDefault(DeviceObject, Irp);
    }
}

NTSTATUS
NTAPI
HidRegisterMinidriver(
    IN PHID_MINIDRIVER_REGISTRATION MinidriverRegistration)
{
    NTSTATUS Status;
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;

    /* check if the version matches */
    if (MinidriverRegistration->Revision > HID_REVISION)
    {
        /* revision mismatch */
        ASSERT(FALSE);
        return STATUS_REVISION_MISMATCH;
    }

    /* now allocate the driver object extension */
    Status = IoAllocateDriverObjectExtension(MinidriverRegistration->DriverObject,
                                             ClientIdentificationAddress,
                                             sizeof(HIDCLASS_DRIVER_EXTENSION),
                                             (PVOID *)&DriverExtension);
    if (!NT_SUCCESS(Status))
    {
        /* failed to allocate driver extension */
        ASSERT(FALSE);
        return Status;
    }

    /* zero driver extension */
    RtlZeroMemory(DriverExtension, sizeof(HIDCLASS_DRIVER_EXTENSION));

    /* init driver extension */
    DriverExtension->DriverObject = MinidriverRegistration->DriverObject;
    DriverExtension->DeviceExtensionSize = MinidriverRegistration->DeviceExtensionSize;
    DriverExtension->DevicesArePolled = MinidriverRegistration->DevicesArePolled;
    DriverExtension->AddDevice = MinidriverRegistration->DriverObject->DriverExtension->AddDevice;
    DriverExtension->DriverUnload = MinidriverRegistration->DriverObject->DriverUnload;

    /* copy driver dispatch routines */
    RtlCopyMemory(DriverExtension->MajorFunction,
                  MinidriverRegistration->DriverObject->MajorFunction,
                  sizeof(PDRIVER_DISPATCH) * (IRP_MJ_MAXIMUM_FUNCTION + 1));

    /* initialize lock */
    KeInitializeSpinLock(&DriverExtension->Lock);

    /* now replace dispatch routines */
    DriverExtension->DriverObject->DriverExtension->AddDevice = HidClassAddDevice;
    DriverExtension->DriverObject->DriverUnload = HidClassDriverUnload;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_CREATE] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_CLOSE] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_READ] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_WRITE] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_POWER] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_PNP] = HidClassDispatch;

    /* done */
    return STATUS_SUCCESS;
}
