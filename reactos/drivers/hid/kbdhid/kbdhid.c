/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/hid/kbdhid/kbdhid.c
 * PURPOSE:     Keyboard HID Driver
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "kbdhid.h"

//子函数
//使用回调把读键传递到类设备对象
//为什么要升IRQL？
VOID
KbdHid_DispatchInputData(
    IN PKBDHID_DEVICE_EXTENSION DeviceExtension,
    IN PKEYBOARD_INPUT_DATA InputData)
{
    KIRQL OldIrql;
    ULONG InputDataConsumed;

    if (!DeviceExtension->ClassService)
        return;

    /* sanity check */
    ASSERT(DeviceExtension->ClassService);
    ASSERT(DeviceExtension->ClassDeviceObject);

    /* raise irql */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* dispatch input data */
    (*(PSERVICE_CALLBACK_ROUTINE)DeviceExtension->ClassService)(DeviceExtension->ClassDeviceObject, InputData, InputData + 1, &InputDataConsumed);

    /* lower irql to previous level */
    KeLowerIrql(OldIrql);
}

BOOLEAN
NTAPI
KbdHid_InsertScanCodes(
    IN PVOID  Context,
    IN PCHAR  NewScanCodes,
    IN ULONG  Length)
{
    KEYBOARD_INPUT_DATA InputData;
    ULONG Index;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    CHAR Prefix = 0;

    /* get device extension */
    DeviceExtension = Context;

    for(Index = 0; Index < Length; Index++)
    {
        DPRINT("[KBDHID] ScanCode Index %lu ScanCode %x\n", Index, NewScanCodes[Index] & 0xFF);

        /* check if this is E0 or E1 prefix */
        if (NewScanCodes[Index] == (CHAR)0xE0 || NewScanCodes[Index] == (CHAR)0xE1)
        {
            Prefix = NewScanCodes[Index];
            continue;
        }

        /* init input data */
        RtlZeroMemory(&InputData, sizeof(KEYBOARD_INPUT_DATA));

        /* use keyboard unit id */ //unit id意思是哪个键盘读来的
        InputData.UnitId = DeviceExtension->KeyboardTypematic.UnitId;

        if (NewScanCodes[Index] & 0x80)
        {
            /* scan codes with 0x80 flag are a key break */
            InputData.Flags |= KEY_BREAK;
        }

        /* set a prefix if needed */
        if (Prefix)
        {
            InputData.Flags |= (Prefix == (CHAR)0xE0 ? KEY_E0 : KEY_E1);
            Prefix = 0;
        }

        /* store key code */
        InputData.MakeCode = NewScanCodes[Index] & 0x7F;

        /* dispatch scan codes */
        KbdHid_DispatchInputData(Context, &InputData);
    }

    /* done */
    return TRUE;
}

//主要是调用一下函数对report进行处理，不过在高IRQL中执行有点浪费吧
//HidP_GetUsagesEx
//HidP_UsageAndPageListDifference
//HidP_TranslateUsageAndPagesToI8042ScanCodes
//KbdHid_InitiateRead
NTSTATUS
NTAPI
KbdHid_ReadCompletion(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;
    ULONG ButtonLength;

    /* get device extension */
    DeviceExtension = Context;

    if (Irp->IoStatus.Status == STATUS_PRIVILEGE_NOT_HELD ||
        Irp->IoStatus.Status == STATUS_DEVICE_NOT_CONNECTED ||
        Irp->IoStatus.Status == STATUS_CANCELLED ||
        DeviceExtension->StopReadReport)
    {
        /* failed to read or should be stopped*/
        DPRINT1("[KBDHID] ReadCompletion terminating read Status %x\n", Irp->IoStatus.Status);

        /* report no longer active */
        DeviceExtension->ReadReportActive = FALSE;

        /* request stopping of the report cycle */
        DeviceExtension->StopReadReport = FALSE;

        /* signal completion event */
		//为了同步KbdHid_Close
        KeSetEvent(&DeviceExtension->ReadCompletionEvent, 0, 0);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    //
    // print out raw report
    //
    ASSERT(DeviceExtension->ReportLength >= 9);
    DPRINT("[KBDHID] ReadCompletion %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", DeviceExtension->Report[0], DeviceExtension->Report[1], DeviceExtension->Report[2],
        DeviceExtension->Report[3], DeviceExtension->Report[4], DeviceExtension->Report[5],
        DeviceExtension->Report[6], DeviceExtension->Report[7], DeviceExtension->Report[8]);


    /* get current usages */
    ButtonLength = DeviceExtension->UsageListLength;
    Status = HidP_GetUsagesEx(HidP_Input, //枚举
                              HIDP_LINK_COLLECTION_UNSPECIFIED,
                              DeviceExtension->CurrentUsageList, //输入输出，ButtonList
                              &ButtonLength,                     //输入输出，ButtonList长度（element数）
                              DeviceExtension->PreparsedData,
                              DeviceExtension->Report, //待处理的report
                              DeviceExtension->ReportLength);
    ASSERT(Status == HIDP_STATUS_SUCCESS);

    /* FIXME check if needs mapping */

    /* get usage difference */
    Status = HidP_UsageAndPageListDifference(DeviceExtension->PreviousUsageList,
                                             DeviceExtension->CurrentUsageList,
                                             DeviceExtension->BreakUsageList, //out
                                             DeviceExtension->MakeUsageList,  //out
                                             DeviceExtension->UsageListLength);
    ASSERT(Status == HIDP_STATUS_SUCCESS);

    /* replace previous usage list with current list */
    RtlMoveMemory(DeviceExtension->PreviousUsageList,
                  DeviceExtension->CurrentUsageList,
                  sizeof(USAGE_AND_PAGE) * DeviceExtension->UsageListLength);

    /* translate break usage list */
    HidP_TranslateUsageAndPagesToI8042ScanCodes(DeviceExtension->BreakUsageList, //ChangedUsageList
                                                DeviceExtension->UsageListLength, //最大可能
                                                HidP_Keyboard_Break, //枚举，Key Action
                                                &DeviceExtension->ModifierState,//输入输出
                                                KbdHid_InsertScanCodes,//回调
                                                DeviceExtension); //回调context
    ASSERT(Status == HIDP_STATUS_SUCCESS);

    /* translate new usage list */
    HidP_TranslateUsageAndPagesToI8042ScanCodes(DeviceExtension->MakeUsageList,
                                                DeviceExtension->UsageListLength,
                                                HidP_Keyboard_Make,//枚举，Key Action
                                                &DeviceExtension->ModifierState,
                                                KbdHid_InsertScanCodes,
                                                DeviceExtension);
    ASSERT(Status == HIDP_STATUS_SUCCESS);

    /* re-init read */
    KbdHid_InitiateRead(DeviceExtension); //重启读很重要，形成自循环

    /* stop completion */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
KbdHid_InitiateRead(
    IN PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PIO_STACK_LOCATION nextIoStack;
    NTSTATUS Status;

    /* re-use irp */
    IoReuseIrp(DeviceExtension->Irp, STATUS_SUCCESS);

    /* init irp */
    DeviceExtension->Irp->MdlAddress = DeviceExtension->ReportMDL;

    /* get next stack location */
    nextIoStack = IoGetNextIrpStackLocation(DeviceExtension->Irp);

    /* init stack location */
    nextIoStack->Parameters.Read.Length = DeviceExtension->ReportLength;
    nextIoStack->Parameters.Read.Key = 0;
    nextIoStack->Parameters.Read.ByteOffset.QuadPart = 0LL;
    nextIoStack->MajorFunction = IRP_MJ_READ; //本驱动没有专门的DispatchRead，但是下层驱动有！
    nextIoStack->FileObject = DeviceExtension->FileObject;

    /* set completion routine */
    IoSetCompletionRoutine(DeviceExtension->Irp, KbdHid_ReadCompletion, DeviceExtension, TRUE, TRUE, TRUE);

    /* read is active */
    DeviceExtension->ReadReportActive = TRUE;

    /* start the read */
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, DeviceExtension->Irp);

    /* done */
    return Status;
}

NTSTATUS
NTAPI
KbdHid_CreateCompletion(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context)
{
    KeSetEvent((PKEVENT)Context, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS
NTAPI
KbdHid_Create(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    KEVENT Event;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    DPRINT("[KBDHID]: IRP_MJ_CREATE\n");

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* get stack location */
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    /* copy stack location to next */
    IoCopyCurrentIrpStackLocationToNext(Irp);

    /* init event */
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    /* prepare irp */
    IoSetCompletionRoutine(Irp, KbdHid_CreateCompletion, &Event, TRUE, TRUE, TRUE);

    /* call lower driver */
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        /* request pending */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    /* check for success */
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    //第一次执行，会有第二次打开的机会么？
    if (DeviceExtension->FileObject == NULL)
    {
         /* did the caller specify correct attributes */
         ASSERT(IoStack->Parameters.Create.SecurityContext);
         if (IoStack->Parameters.Create.SecurityContext->DesiredAccess)
         {
             /* store file object */
             DeviceExtension->FileObject = IoStack->FileObject;

             /* reset event */
             KeResetEvent(&DeviceExtension->ReadCompletionEvent);

             /* initiating read */
			 //向下层发起读IRP很重要，难怪本驱动没有实现IRP_MJ_READ，
			 //当然还有KeyboardClassServiceCallback的关系
             Status = KbdHid_InitiateRead(DeviceExtension);
             DPRINT("[KBDHID] KbdHid_InitiateRead: status %x\n", Status);
             if (Status == STATUS_PENDING)
             {
                 /* report irp is pending */
                 Status = STATUS_SUCCESS;
             }
         }
    }

    /* complete request */
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}


NTSTATUS
NTAPI
KbdHid_Close(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    DPRINT("[KBDHID] IRP_MJ_CLOSE ReadReportActive %x\n", DeviceExtension->ReadReportActive);

	//现在有两个IRP，一个是正常读键的IRP，另一个就是本IRP
	//保证正常读键的IRP要么被很好地cancel，要么很好地完成，并且不会有新的读IRP进入
    if (DeviceExtension->ReadReportActive)
    {
        /* request stopping of the report cycle */
        DeviceExtension->StopReadReport = TRUE;

        /* wait until the reports have been read */
        KeWaitForSingleObject(&DeviceExtension->ReadCompletionEvent, Executive, KernelMode, FALSE, NULL);

        /* cancel irp */
        IoCancelIrp(DeviceExtension->Irp);
    }

    DPRINT("[KBDHID] IRP_MJ_CLOSE ReadReportActive %x\n", DeviceExtension->ReadReportActive);

    /* remove file object */
    DeviceExtension->FileObject = NULL;//有什么奥妙？

    /* skip location */
    IoSkipCurrentIrpStackLocation(Irp);//本层实在没有可执行的了，后续也不想要什么结果

    /* pass irp to down the stack */
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);//必须传递到下层，让下层收到文件关闭请求
}

//内部控制包，很简单，不是来回拷贝就是简单设置，不会往下传递
NTSTATUS
NTAPI
KbdHid_InternalDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    PCONNECT_DATA Data;
    PKEYBOARD_ATTRIBUTES Attributes;

    /* get current stack location */
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    DPRINT("[KBDHID] InternalDeviceControl %x\n", IoStack->Parameters.DeviceIoControl.IoControlCode);

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_KEYBOARD_QUERY_ATTRIBUTES: //现成的，拷贝就行
            /* verify output buffer length */
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUSE_ATTRIBUTES))
            {
                /* invalid request */
                DPRINT1("[MOUHID] IOCTL_MOUSE_QUERY_ATTRIBUTES Buffer too small\n");
                Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_BUFFER_TOO_SMALL;
            }

            /* get output buffer */
            Attributes = Irp->AssociatedIrp.SystemBuffer;

            /* copy attributes */
            RtlCopyMemory(Attributes,
                          &DeviceExtension->Attributes,
                          sizeof(KEYBOARD_ATTRIBUTES));

            /* complete request */
            Irp->IoStatus.Information = sizeof(MOUSE_ATTRIBUTES);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;

        case IOCTL_INTERNAL_KEYBOARD_CONNECT: //连接一下就行
            /* verify input buffer length */
            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA))
            {
                /* invalid request */
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            /* is it already connected */
            if (DeviceExtension->ClassService)
            {
                /* already connected */
                Irp->IoStatus.Status = STATUS_SHARING_VIOLATION;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_SHARING_VIOLATION;
            }

            /* get connect data */
            Data = IoStack->Parameters.DeviceIoControl.Type3InputBuffer;

            /* store connect details */
            DeviceExtension->ClassDeviceObject = Data->ClassDeviceObject;//保存上层类设备对象
            DeviceExtension->ClassService = Data->ClassService;//保存回调函数，众所周知的KeyboardClassServiceCallback

            /* completed successfully */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;

        case IOCTL_INTERNAL_KEYBOARD_DISCONNECT: //未实现
            /* not implemented */
            Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_IMPLEMENTED;

        case IOCTL_INTERNAL_KEYBOARD_ENABLE: //未实现，似乎好实现
            /* not supported */
            Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_SUPPORTED;

        case IOCTL_INTERNAL_KEYBOARD_DISABLE://未实现，似乎好实现
            /* not supported */
            Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_SUPPORTED;

        case IOCTL_KEYBOARD_QUERY_INDICATORS: //现成的，拷贝就行
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KEYBOARD_INDICATOR_PARAMETERS))
            {
                /* invalid parameter */
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            /* copy indicators */
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                          &DeviceExtension->KeyboardIndicator,
                          sizeof(KEYBOARD_INDICATOR_PARAMETERS));

            /* complete request */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(KEYBOARD_INDICATOR_PARAMETERS);
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_IMPLEMENTED;

        case IOCTL_KEYBOARD_QUERY_TYPEMATIC://现成的，拷贝就行
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS))
            {
                /* invalid parameter */
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            /* copy indicators */
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                          &DeviceExtension->KeyboardTypematic,
                          sizeof(KEYBOARD_TYPEMATIC_PARAMETERS));

            /* done */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(KEYBOARD_TYPEMATIC_PARAMETERS);
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;

        case IOCTL_KEYBOARD_SET_INDICATORS://保存就行，但没实施
            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(KEYBOARD_INDICATOR_PARAMETERS))
            {
                /* invalid parameter */
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            /* copy indicators */
            RtlCopyMemory(&DeviceExtension->KeyboardIndicator,
                          Irp->AssociatedIrp.SystemBuffer,
                          sizeof(KEYBOARD_INDICATOR_PARAMETERS));

            /* done */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;

        case IOCTL_KEYBOARD_SET_TYPEMATIC: //保存就行，但没实施
            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS))
            {
                /* invalid parameter */
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            /* copy indicators */
            RtlCopyMemory(&DeviceExtension->KeyboardTypematic,
                          Irp->AssociatedIrp.SystemBuffer,
                          sizeof(KEYBOARD_TYPEMATIC_PARAMETERS));

            /* done */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;

        case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION://未实现
            /* not implemented */
            DPRINT1("IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION not implemented\n");
            Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_IMPLEMENTED;
    }

    /* unknown control code */
    DPRINT1("[KBDHID] Unknown DeviceControl %x\n", IoStack->Parameters.DeviceIoControl.IoControlCode);
    /* unknown request not supported */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED; //没有不完成的情况
}

NTSTATUS
NTAPI
KbdHid_DeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* skip stack location */
    IoSkipCurrentIrpStackLocation(Irp);

    /* pass and forget */
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
KbdHid_Power(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    DeviceExtension = DeviceObject->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
KbdHid_SystemControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    DeviceExtension = DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

//发送IoControl包，同步执行
NTSTATUS
KbdHid_SubmitRequest(
    PDEVICE_OBJECT DeviceObject,
    ULONG IoControlCode,
    ULONG InputBufferSize,
    PVOID InputBuffer,
    ULONG OutputBufferSize,
    PVOID OutputBuffer)
{
    KEVENT Event;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    PIRP Irp;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* init event */
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    /* build request */
    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DeviceExtension->NextDeviceObject,
                                        InputBuffer,
                                        InputBufferSize,
                                        OutputBuffer,
                                        OutputBufferSize,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (!Irp)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* send request */
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        /* wait for request to complete */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    /* done */
    return Status;
}

//子函数
//进行了很多准备工作，获得PreparsedData之类提前需要得到的东西
NTSTATUS
NTAPI
KbdHid_StartDevice(
    IN PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS Status;
    ULONG Buttons;
    HID_COLLECTION_INFORMATION Information;
    PHIDP_PREPARSED_DATA PreparsedData;
    HIDP_CAPS Capabilities;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    PUSAGE_AND_PAGE Buffer;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* query collection information */
    Status = KbdHid_SubmitRequest(DeviceObject,
                                  IOCTL_HID_GET_COLLECTION_INFORMATION,
                                  0,
                                  NULL,
                                  sizeof(HID_COLLECTION_INFORMATION),
                                  &Information);
    if (!NT_SUCCESS(Status))
    {
        /* failed to query collection information */
        DPRINT1("[KBDHID] failed to obtain collection information with %x\n", Status);
        return Status;
    }

    /* lets allocate space for preparsed data */
    PreparsedData = ExAllocatePoolWithTag(NonPagedPool, Information.DescriptorSize, KBDHID_TAG);
    if (!PreparsedData)
    {
        /* no memory */
        DPRINT1("[KBDHID] no memory size %u\n", Information.DescriptorSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* now obtain the preparsed data */
    Status = KbdHid_SubmitRequest(DeviceObject,
                                  IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                  0,
                                  NULL,
                                  Information.DescriptorSize,
                                  PreparsedData);
    if (!NT_SUCCESS(Status))
    {
        /* failed to get preparsed data */
        DPRINT1("[KBDHID] failed to obtain collection information with %x\n", Status);
        ExFreePoolWithTag(PreparsedData, KBDHID_TAG);
        return Status;
    }

    /* lets get the caps */
    Status = HidP_GetCaps(PreparsedData, &Capabilities);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        /* failed to get capabilities */
        DPRINT1("[KBDHID] failed to obtain caps with %x\n", Status);
        ExFreePoolWithTag(PreparsedData, KBDHID_TAG);
        return Status;
    }

    DPRINT("[KBDHID] Usage %x UsagePage %x InputReportLength %lu\n", Capabilities.Usage, Capabilities.UsagePage, Capabilities.InputReportByteLength);

    /* init input report */
    DeviceExtension->ReportLength = Capabilities.InputReportByteLength;
    ASSERT(DeviceExtension->ReportLength);
    DeviceExtension->Report = ExAllocatePoolWithTag(NonPagedPool, DeviceExtension->ReportLength, KBDHID_TAG);
    ASSERT(DeviceExtension->Report);
    RtlZeroMemory(DeviceExtension->Report, DeviceExtension->ReportLength);

    /* build mdl */
    DeviceExtension->ReportMDL = IoAllocateMdl(DeviceExtension->Report,
                                               DeviceExtension->ReportLength,
                                               FALSE,
                                               FALSE,
                                               NULL);
    ASSERT(DeviceExtension->ReportMDL);

    /* init mdl */
    MmBuildMdlForNonPagedPool(DeviceExtension->ReportMDL);

    /* get max number of buttons */
    Buttons = HidP_MaxUsageListLength(HidP_Input, HID_USAGE_PAGE_KEYBOARD, PreparsedData);
    DPRINT("[KBDHID] Buttons %lu\n", Buttons);
    ASSERT(Buttons > 0);

    /* now allocate an array for those buttons */
    Buffer = ExAllocatePoolWithTag(NonPagedPool, sizeof(USAGE_AND_PAGE) * 4 * Buttons, KBDHID_TAG);
    if (!Buffer)
    {
        /* no memory */
        ExFreePoolWithTag(PreparsedData, KBDHID_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DeviceExtension->UsageListBuffer = Buffer;

    /* init usage lists */
    RtlZeroMemory(Buffer, sizeof(USAGE_AND_PAGE) * 4 * Buttons);
    DeviceExtension->CurrentUsageList = Buffer;
    Buffer += Buttons;
    DeviceExtension->PreviousUsageList = Buffer;
    Buffer += Buttons;
    DeviceExtension->MakeUsageList = Buffer;
    Buffer += Buttons;
    DeviceExtension->BreakUsageList = Buffer;

    //
    // FIMXE: implement device hacks
    //
    // UsageMappings
    // KeyboardTypeOverride
    // KeyboardSubTypeOverride
    // KeyboardNumberTotalKeysOverride
    // KeyboardNumberFunctionKeysOverride
    // KeyboardNumberIndicatorsOverride

    /* store number of buttons */
    DeviceExtension->UsageListLength = (USHORT)Buttons;

    /* store preparsed data */
    DeviceExtension->PreparsedData = PreparsedData;

    /* completed successfully */
    return STATUS_SUCCESS;
}

//完成函数保证了回到原来的地方继续执行
NTSTATUS
NTAPI
KbdHid_StartDeviceCompletion(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context)
{
    KeSetEvent((PKEVENT)Context, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
KbdHid_FreeResources(
    IN PDEVICE_OBJECT DeviceObject)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* free resources */
    if (DeviceExtension->PreparsedData)
    {
        ExFreePoolWithTag(DeviceExtension->PreparsedData, KBDHID_TAG);
        DeviceExtension->PreparsedData = NULL;
    }

    if (DeviceExtension->UsageListBuffer)
    {
        ExFreePoolWithTag(DeviceExtension->UsageListBuffer, KBDHID_TAG);
        DeviceExtension->UsageListBuffer = NULL;
        DeviceExtension->CurrentUsageList = NULL;
        DeviceExtension->PreviousUsageList = NULL;
        DeviceExtension->MakeUsageList = NULL;
        DeviceExtension->BreakUsageList = NULL;
    }

    if (DeviceExtension->ReportMDL)
    {
        IoFreeMdl(DeviceExtension->ReportMDL);
        DeviceExtension->ReportMDL = NULL;
    }

    if (DeviceExtension->Report)
    {
        ExFreePoolWithTag(DeviceExtension->Report, KBDHID_TAG);
        DeviceExtension->Report = NULL;
    }

    return STATUS_SUCCESS;
}
//为什么要往下传？因为读的东西来自下层，本层就是并不真正读，而是把读给了下层
//我们接收的命令是IRP_MJ_FLUSH_BUFFERS，对于下层就是IOCTL_HID_FLUSH_QUEUE，所以下层要实现这个控制
NTSTATUS
NTAPI
KbdHid_Flush(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* skip current stack location */
    IoSkipCurrentIrpStackLocation(Irp);

    /* get next stack location */
    IoStack = IoGetNextIrpStackLocation(Irp);

    /* change request to hid flush queue request */
    IoStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_FLUSH_QUEUE;

    /* call device */
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
KbdHid_Pnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    KEVENT Event;
    NTSTATUS Status;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* get current irp stack */
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    DPRINT("[KBDHID] IRP_MJ_PNP Request: %x\n", IoStack->MinorFunction);

    switch (IoStack->MinorFunction)
    {
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        /* free resources */
        KbdHid_FreeResources(DeviceObject);
        /* fall through */
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        /* indicate success */
        Irp->IoStatus.Status = STATUS_SUCCESS;

        /* skip irp stack location */
        IoSkipCurrentIrpStackLocation(Irp);

        /* dispatch to lower device */
        return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);

    case IRP_MN_REMOVE_DEVICE://上层要是阻止remove的化，一定不要传进来，因为本层无条件remove
        /* FIXME synchronization */

        /* cancel irp */
        IoCancelIrp(DeviceExtension->Irp);

        /* free resources */
        KbdHid_FreeResources(DeviceObject);

        /* indicate success */
        Irp->IoStatus.Status = STATUS_SUCCESS;

        /* skip irp stack location */
        IoSkipCurrentIrpStackLocation(Irp);

        /* dispatch to lower device */
        Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);

        IoFreeIrp(DeviceExtension->Irp);
        IoDetachDevice(DeviceExtension->NextDeviceObject);
        IoDeleteDevice(DeviceObject);
        return Status;

    case IRP_MN_START_DEVICE:
        /* init event */
        KeInitializeEvent(&Event, NotificationEvent, FALSE);

        /* copy stack location */
        IoCopyCurrentIrpStackLocationToNext (Irp);

        /* set completion routine */
        IoSetCompletionRoutine(Irp, KbdHid_StartDeviceCompletion, &Event, TRUE, TRUE, TRUE);
        Irp->IoStatus.Status = 0;

        /* pass request */
        Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = Irp->IoStatus.Status;
        }

        if (!NT_SUCCESS(Status))
        {
            /* failed */
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }

        /* lets start the device */
        Status = KbdHid_StartDevice(DeviceObject);
        DPRINT("KbdHid_StartDevice %x\n", Status);

        /* complete request */
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        /* done */
        return Status;

    default:
        /* skip irp stack location */
        IoSkipCurrentIrpStackLocation(Irp);

        /* dispatch to lower device */
        return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    }
}


//设置了键盘的缺省属性(Attributes)
//分配了一个IRP挂在设备扩展上备用
//给键盘上电！
NTSTATUS
NTAPI
KbdHid_AddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject) //只是告知PDO
{
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject, NextDeviceObject;
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    POWER_STATE State;

    /* create device object */
    Status = IoCreateDevice(DriverObject,
                            sizeof(KBDHID_DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_KEYBOARD, //这里是File了
                            0,
                            FALSE,
                            &DeviceObject); //这应该是FDO
    if (!NT_SUCCESS(Status))
    {
        /* failed to create device object */
        return Status;
    }

    /* now attach it */
	//注意下面返回值NextDeviceObject不一定等于PhysicalDeviceObject，如果中间有层的话
    NextDeviceObject = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (!NextDeviceObject)
    {
        /* failed to attach */
        IoDeleteDevice(DeviceObject);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* get device extension */
    DeviceExtension = DeviceObject->DeviceExtension;

    /* zero extension */
    RtlZeroMemory(DeviceExtension, sizeof(KBDHID_DEVICE_EXTENSION));

    /* init device extension */
    DeviceExtension->NextDeviceObject = NextDeviceObject;
    KeInitializeEvent(&DeviceExtension->ReadCompletionEvent, NotificationEvent, FALSE);

    /* init keyboard attributes */ //注意这里设置了很多缺省的重要属性
    DeviceExtension->Attributes.KeyboardIdentifier.Type = KEYBOARD_TYPE_UNKNOWN;
    DeviceExtension->Attributes.KeyboardIdentifier.Subtype = MICROSOFT_KBD_101_TYPE;
    DeviceExtension->Attributes.NumberOfFunctionKeys = MICROSOFT_KBD_FUNC;
    DeviceExtension->Attributes.NumberOfIndicators = 3; // caps, num lock, scroll lock
    DeviceExtension->Attributes.NumberOfKeysTotal = 101;
    DeviceExtension->Attributes.InputDataQueueLength = 1;
    DeviceExtension->Attributes.KeyRepeatMinimum.Rate = KEYBOARD_TYPEMATIC_RATE_MINIMUM;
    DeviceExtension->Attributes.KeyRepeatMinimum.Delay = KEYBOARD_TYPEMATIC_DELAY_MINIMUM;
    DeviceExtension->Attributes.KeyRepeatMaximum.Rate = KEYBOARD_TYPEMATIC_RATE_DEFAULT;
    DeviceExtension->Attributes.KeyRepeatMaximum.Delay = KEYBOARD_TYPEMATIC_DELAY_MAXIMUM;

    /* allocate irp */
    DeviceExtension->Irp = IoAllocateIrp(NextDeviceObject->StackSize, FALSE);//分配的Irp会循环使用吗？

    /* FIXME handle allocation error */
    ASSERT(DeviceExtension->Irp);

    /* set power state to D0 */ //加电！
    State.DeviceState =  PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, State);

    /* init device object */
    DeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    DeviceObject->Flags  &= ~DO_DEVICE_INITIALIZING;

    /* completed successfully */
    return STATUS_SUCCESS;
}

VOID
NTAPI
KbdHid_Unload(
    IN PDRIVER_OBJECT DriverObject)
{
    UNIMPLEMENTED
}

//注意没有DispatchRead，运行级别是IRQL = PASSIVE_LEVEL
//注意没有Cancel routine，运行级别是IRQL = DISPATCH_LEVEL 
NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegPath)
{
    /* initialize driver object */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = KbdHid_Create; //PASSIVE_LEVEL 
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = KbdHid_Close; //PASSIVE_LEVEL
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = KbdHid_Flush;//PASSIVE_LEVEL
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KbdHid_DeviceControl;//PASSIVE_LEVEL
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = KbdHid_InternalDeviceControl;//PASSIVE_LEVEL
    DriverObject->MajorFunction[IRP_MJ_POWER] = KbdHid_Power;//PASSIVE_LEVEL
    DriverObject->MajorFunction[IRP_MJ_PNP] = KbdHid_Pnp;//PASSIVE_LEVEL
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = KbdHid_SystemControl;//PASSIVE_LEVEL
    DriverObject->DriverUnload = KbdHid_Unload;//PASSIVE_LEVEL
    DriverObject->DriverExtension->AddDevice = KbdHid_AddDevice;//PASSIVE_LEVEL

    /* done */
    return STATUS_SUCCESS;
}
