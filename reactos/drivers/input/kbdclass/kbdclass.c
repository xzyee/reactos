/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Keyboard class driver
 * FILE:            drivers/kbdclass/kbdclass.c
 * PURPOSE:         Keyboard class driver
 *
 * PROGRAMMERS:     Hervé Poussineau (hpoussin@reactos.org)
 */

#include "kbdclass.h"

#include <stdio.h>
#include <pseh/pseh2.h>
#include <kbdmou.h>
#include <debug.h>

static DRIVER_UNLOAD DriverUnload;
static DRIVER_DISPATCH ClassCreate;
static DRIVER_DISPATCH ClassClose;
static DRIVER_DISPATCH ClassCleanup;
static DRIVER_DISPATCH ClassRead;
static DRIVER_DISPATCH ClassDeviceControl;
static DRIVER_DISPATCH IrpStub;
static DRIVER_ADD_DEVICE ClassAddDevice;
static DRIVER_STARTIO ClassStartIo;
static DRIVER_CANCEL ClassCancelRoutine;
static NTSTATUS
HandleReadIrp(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp,
	BOOLEAN IsInStartIo);

static VOID NTAPI
DriverUnload(IN PDRIVER_OBJECT DriverObject)
{
	// nothing to do here yet
}

static NTSTATUS NTAPI
ClassCreate(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	TRACE_(CLASS_NAME, "IRP_MJ_CREATE\n");

	if (!((PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->IsClassDO)
		return ForwardIrpAndForget(DeviceObject, Irp);

	/* FIXME: open all associated Port devices */
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
ClassClose(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	TRACE_(CLASS_NAME, "IRP_MJ_CLOSE\n");

	if (!((PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->IsClassDO)
		return ForwardIrpAndForget(DeviceObject, Irp);

	/* FIXME: close all associated Port devices */
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
ClassCleanup(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	TRACE_(CLASS_NAME, "IRP_MJ_CLEANUP\n");

	if (!((PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->IsClassDO)
		return ForwardIrpAndForget(DeviceObject, Irp);

	/* FIXME: cleanup all associated Port devices */
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
ClassRead(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	PCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	KIRQL OldIrql;
	NTSTATUS Status;

	TRACE_(CLASS_NAME, "IRP_MJ_READ\n");

	ASSERT(DeviceExtension->Common.IsClassDO);

	if (!((PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->IsClassDO)
		return ForwardIrpAndForget(DeviceObject, Irp);

	if (IoGetCurrentIrpStackLocation(Irp)->Parameters.Read.Length < sizeof(KEYBOARD_INPUT_DATA))
	{
		Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);

		return STATUS_BUFFER_TOO_SMALL;
	}

	KeAcquireSpinLock(&DeviceExtension->SpinLock, &OldIrql);
	Status = HandleReadIrp(DeviceObject, Irp, FALSE);
	KeReleaseSpinLock(&DeviceExtension->SpinLock, OldIrql);
	return Status;
}

static NTSTATUS NTAPI
ClassDeviceControl(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	//PCLASS_DEVICE_EXTENSION DeviceExtension;
	NTSTATUS Status = STATUS_NOT_SUPPORTED;

	TRACE_(CLASS_NAME, "IRP_MJ_DEVICE_CONTROL\n");

	if (!((PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->IsClassDO)
		return ForwardIrpAndForget(DeviceObject, Irp);

	//DeviceExtension = (PCLASS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	switch (IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode)
	{
		case IOCTL_KEYBOARD_QUERY_ATTRIBUTES:
		case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION:
		case IOCTL_KEYBOARD_QUERY_INDICATORS:
		case IOCTL_KEYBOARD_QUERY_TYPEMATIC:
		{
			/* FIXME: We hope that all devices will return the same result.
			 * Ask only the first one */
			PLIST_ENTRY Head = &((PCLASS_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->ListHead;
			if (Head->Flink != Head)
			{
				/* We have at least one device */
				PPORT_DEVICE_EXTENSION DevExt = CONTAINING_RECORD(Head->Flink, PORT_DEVICE_EXTENSION, ListEntry);
				IoGetCurrentIrpStackLocation(Irp)->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
				IoSkipCurrentIrpStackLocation(Irp);
				return IoCallDriver(DevExt->DeviceObject, Irp);
			}
			break;
		}
		case IOCTL_KEYBOARD_SET_INDICATORS:
		case IOCTL_KEYBOARD_SET_TYPEMATIC: /* not in MSDN, would seem logical */
		{
			/* Send it to all associated Port devices */
			PLIST_ENTRY Head = &((PCLASS_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->ListHead;
			PLIST_ENTRY Entry = Head->Flink;
			Status = STATUS_SUCCESS;
			while (Entry != Head)
			{
				PPORT_DEVICE_EXTENSION DevExt = CONTAINING_RECORD(Entry, PORT_DEVICE_EXTENSION, ListEntry);
				NTSTATUS IntermediateStatus;

				IoGetCurrentIrpStackLocation(Irp)->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
				IntermediateStatus = ForwardIrpAndWait(DevExt->DeviceObject, Irp);
				if (!NT_SUCCESS(IntermediateStatus))
					Status = IntermediateStatus;
				Entry = Entry->Flink;
			}
			break;
		}
		default:
			WARN_(CLASS_NAME, "IRP_MJ_DEVICE_CONTROL / unknown I/O control code 0x%lx\n",
				IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode);
			ASSERT(FALSE);
			break;
	}

	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return Status;
}

static NTSTATUS NTAPI
IrpStub(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	NTSTATUS Status = STATUS_NOT_SUPPORTED;

	if (!((PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->IsClassDO)
	{
		/* Forward some IRPs to lower device */
		switch (IoGetCurrentIrpStackLocation(Irp)->MajorFunction)
		{
			case IRP_MJ_PNP:
			case IRP_MJ_INTERNAL_DEVICE_CONTROL:
				return ForwardIrpAndForget(DeviceObject, Irp);
			default:
			{
				ERR_(CLASS_NAME, "Port DO stub for major function 0x%lx\n",
					IoGetCurrentIrpStackLocation(Irp)->MajorFunction);
				ASSERT(FALSE);
			}
		}
	}
	else
	{
		ERR_(CLASS_NAME, "Class DO stub for major function 0x%lx\n",
			IoGetCurrentIrpStackLocation(Irp)->MajorFunction);
		ASSERT(FALSE);
	}

	Irp->IoStatus.Status = Status;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Status;
}

static NTSTATUS
ReadRegistryEntries(
	IN PUNICODE_STRING RegistryPath,
	IN PCLASS_DRIVER_EXTENSION DriverExtension)
{
	UNICODE_STRING ParametersRegistryKey;
	RTL_QUERY_REGISTRY_TABLE Parameters[4];
	NTSTATUS Status;

	/* HACK: We don't support multiple devices with this disabled */
	ULONG DefaultConnectMultiplePorts = 1;
	ULONG DefaultDataQueueSize = 0x64;
	PCWSTR DefaultDeviceBaseName = L"KeyboardClass";

	ParametersRegistryKey.Length = 0;
	ParametersRegistryKey.MaximumLength = RegistryPath->Length + sizeof(L"\\Parameters") + sizeof(UNICODE_NULL);
	ParametersRegistryKey.Buffer = ExAllocatePoolWithTag(PagedPool, ParametersRegistryKey.MaximumLength, CLASS_TAG);
	if (!ParametersRegistryKey.Buffer)
	{
		WARN_(CLASS_NAME, "ExAllocatePoolWithTag() failed\n");
		return STATUS_NO_MEMORY;
	}
	RtlCopyUnicodeString(&ParametersRegistryKey, RegistryPath);
	RtlAppendUnicodeToString(&ParametersRegistryKey, L"\\Parameters");
	ParametersRegistryKey.Buffer[ParametersRegistryKey.Length / sizeof(WCHAR)] = UNICODE_NULL;

	RtlZeroMemory(Parameters, sizeof(Parameters));

	Parameters[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_REGISTRY_OPTIONAL;
	Parameters[0].Name = L"ConnectMultiplePorts";
	Parameters[0].EntryContext = &DriverExtension->ConnectMultiplePorts;
	Parameters[0].DefaultType = REG_DWORD;
	Parameters[0].DefaultData = &DefaultConnectMultiplePorts;
	Parameters[0].DefaultLength = sizeof(ULONG);

	Parameters[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_REGISTRY_OPTIONAL;
	Parameters[1].Name = L"KeyboardDataQueueSize";
	Parameters[1].EntryContext = &DriverExtension->DataQueueSize;
	Parameters[1].DefaultType = REG_DWORD;
	Parameters[1].DefaultData = &DefaultDataQueueSize;
	Parameters[1].DefaultLength = sizeof(ULONG);

	Parameters[2].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_REGISTRY_OPTIONAL;
	Parameters[2].Name = L"KeyboardDeviceBaseName";
	Parameters[2].EntryContext = &DriverExtension->DeviceBaseName;
	Parameters[2].DefaultType = REG_SZ;
	Parameters[2].DefaultData = (PVOID)DefaultDeviceBaseName;
	Parameters[2].DefaultLength = 0;

	Status = RtlQueryRegistryValues(
		RTL_REGISTRY_ABSOLUTE,
		ParametersRegistryKey.Buffer,
		Parameters,
		NULL,
		NULL);

	if (NT_SUCCESS(Status))
	{
		/* Check values */
		if (DriverExtension->ConnectMultiplePorts != 0
			&& DriverExtension->ConnectMultiplePorts != 1)
		{
			DriverExtension->ConnectMultiplePorts = DefaultConnectMultiplePorts;
		}
		if (DriverExtension->DataQueueSize == 0)
		{
			DriverExtension->DataQueueSize = DefaultDataQueueSize;
		}
	}
	else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
	{
		/* Registry path doesn't exist. Set defaults */
		DriverExtension->ConnectMultiplePorts = DefaultConnectMultiplePorts;
		DriverExtension->DataQueueSize = DefaultDataQueueSize;
		if (RtlCreateUnicodeString(&DriverExtension->DeviceBaseName, DefaultDeviceBaseName))
			Status = STATUS_SUCCESS;
		else
			Status = STATUS_NO_MEMORY;
	}

	ExFreePoolWithTag(ParametersRegistryKey.Buffer, CLASS_TAG);
	return Status;
}

static NTSTATUS
CreateClassDeviceObject(
	IN PDRIVER_OBJECT DriverObject,
	OUT PDEVICE_OBJECT *ClassDO OPTIONAL)
{
	PCLASS_DRIVER_EXTENSION DriverExtension;
	ULONG DeviceId = 0;
	ULONG PrefixLength;
	UNICODE_STRING DeviceNameU;
	PWSTR DeviceIdW = NULL; /* Pointer into DeviceNameU.Buffer */
	PDEVICE_OBJECT Fdo;
	PCLASS_DEVICE_EXTENSION DeviceExtension;
	NTSTATUS Status;

	TRACE_(CLASS_NAME, "CreateClassDeviceObject(0x%p)\n", DriverObject);

	/* Create new device object */
	DriverExtension = IoGetDriverObjectExtension(DriverObject, DriverObject);
	DeviceNameU.Length = 0;
	DeviceNameU.MaximumLength =
		wcslen(L"\\Device\\") * sizeof(WCHAR)    /* "\Device\" */
		+ DriverExtension->DeviceBaseName.Length /* "KeyboardClass" */
		+ 4 * sizeof(WCHAR)                      /* Id between 0 and 9999 */
		+ sizeof(UNICODE_NULL);                  /* Final NULL char */
	DeviceNameU.Buffer = ExAllocatePoolWithTag(PagedPool, DeviceNameU.MaximumLength, CLASS_TAG);
	if (!DeviceNameU.Buffer)
	{
		WARN_(CLASS_NAME, "ExAllocatePoolWithTag() failed\n");
		return STATUS_NO_MEMORY;
	}
	Status = RtlAppendUnicodeToString(&DeviceNameU, L"\\Device\\");
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "RtlAppendUnicodeToString() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}
	Status = RtlAppendUnicodeStringToString(&DeviceNameU, &DriverExtension->DeviceBaseName);
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "RtlAppendUnicodeStringToString() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}
	PrefixLength = DeviceNameU.MaximumLength - 4 * sizeof(WCHAR) - sizeof(UNICODE_NULL);
	DeviceIdW = &DeviceNameU.Buffer[PrefixLength / sizeof(WCHAR)];
	while (DeviceId < 9999)
	{
		DeviceNameU.Length = (USHORT)(PrefixLength + swprintf(DeviceIdW, L"%lu", DeviceId) * sizeof(WCHAR));
		Status = IoCreateDevice(
			DriverObject,
			sizeof(CLASS_DEVICE_EXTENSION),
			&DeviceNameU,
			FILE_DEVICE_KEYBOARD,
			FILE_DEVICE_SECURE_OPEN,
			FALSE,
			&Fdo);
		if (NT_SUCCESS(Status))
			goto cleanup;
		else if (Status != STATUS_OBJECT_NAME_COLLISION)
		{
			WARN_(CLASS_NAME, "IoCreateDevice() failed with status 0x%08lx\n", Status);
			goto cleanup;
		}
		DeviceId++;
	}
	WARN_(CLASS_NAME, "Too many devices starting with '\\Device\\%wZ'\n", &DriverExtension->DeviceBaseName);
	Status = STATUS_TOO_MANY_NAMES;
cleanup:
	if (!NT_SUCCESS(Status))
	{
		ExFreePoolWithTag(DeviceNameU.Buffer, CLASS_TAG);
		return Status;
	}

	DeviceExtension = (PCLASS_DEVICE_EXTENSION)Fdo->DeviceExtension;
	RtlZeroMemory(DeviceExtension, sizeof(CLASS_DEVICE_EXTENSION));
	DeviceExtension->Common.IsClassDO = TRUE;
	DeviceExtension->DriverExtension = DriverExtension;//上面刚创建的
	InitializeListHead(&DeviceExtension->ListHead);
	KeInitializeSpinLock(&DeviceExtension->ListSpinLock);
	KeInitializeSpinLock(&DeviceExtension->SpinLock);
	DeviceExtension->InputCount = 0;
	//下面一行充分说命了所有port(对multi而言)或者对单个port而言，
	//保存KEYBOARD_INPUT_DATA队列的地方在classdeviceobject中
	DeviceExtension->PortData = ExAllocatePoolWithTag(NonPagedPool, DeviceExtension->DriverExtension->DataQueueSize * sizeof(KEYBOARD_INPUT_DATA), CLASS_TAG);
	if (!DeviceExtension->PortData)
	{
		ExFreePoolWithTag(DeviceNameU.Buffer, CLASS_TAG);
		return STATUS_NO_MEMORY;
	}
	DeviceExtension->DeviceName = DeviceNameU.Buffer;
	Fdo->Flags |= DO_POWER_PAGABLE;
	Fdo->Flags |= DO_BUFFERED_IO; /* FIXME: Why is it needed for 1st stage setup? */
	Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

	/* Add entry entry to HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\[DeviceBaseName] */
	RtlWriteRegistryValue(
		RTL_REGISTRY_DEVICEMAP, //HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\
		DriverExtension->DeviceBaseName.Buffer,
		DeviceExtension->DeviceName,
		REG_SZ,
		DriverExtension->RegistryPath.Buffer,
		DriverExtension->RegistryPath.MaximumLength);

	if (ClassDO)
		*ClassDO = Fdo;

	return STATUS_SUCCESS;
}

static NTSTATUS
FillEntries(
	IN PDEVICE_OBJECT ClassDeviceObject,
	IN PIRP Irp,
	IN PKEYBOARD_INPUT_DATA DataStart,
	IN SIZE_T NumberOfEntries)
{
	NTSTATUS Status = STATUS_SUCCESS;

	if (ClassDeviceObject->Flags & DO_BUFFERED_IO)
	{
		RtlCopyMemory(
			Irp->AssociatedIrp.SystemBuffer, //buffered IO
			DataStart,
			NumberOfEntries * sizeof(KEYBOARD_INPUT_DATA));
	}
	else if (ClassDeviceObject->Flags & DO_DIRECT_IO)
	{
		PVOID DestAddress = MmGetSystemAddressForMdlSafe(Irp->MdlAddress/*direct io*/, NormalPagePriority);
		if (DestAddress)
		{
			RtlCopyMemory(
				DestAddress,
				DataStart,
				NumberOfEntries * sizeof(KEYBOARD_INPUT_DATA));
		}
		else
			Status = STATUS_UNSUCCESSFUL;
	}
	else
	{
		_SEH2_TRY
		{
			RtlCopyMemory(
				Irp->UserBuffer,
				DataStart,
				NumberOfEntries * sizeof(KEYBOARD_INPUT_DATA));
		}
		_SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
		{
			Status = _SEH2_GetExceptionCode();
		}
		_SEH2_END;
	}

	return Status;
}

static BOOLEAN NTAPI
ClassCallback(
	IN PDEVICE_OBJECT ClassDeviceObject, //具体某classdeviceobject将被传递到kbdhid
	IN OUT PKEYBOARD_INPUT_DATA DataStart,
	IN PKEYBOARD_INPUT_DATA DataEnd,
	IN OUT PULONG ConsumedCount)
{
	PCLASS_DEVICE_EXTENSION ClassDeviceExtension = ClassDeviceObject->DeviceExtension;
	KIRQL OldIrql;
	SIZE_T InputCount = DataEnd - DataStart;
	SIZE_T ReadSize;

	TRACE_(CLASS_NAME, "ClassCallback()\n");

	ASSERT(ClassDeviceExtension->Common.IsClassDO);

	KeAcquireSpinLock(&ClassDeviceExtension->SpinLock, &OldIrql);//在完成函数中执行，因此拿spin锁
	if (InputCount > 0)
	{
		if (ClassDeviceExtension->InputCount + InputCount > ClassDeviceExtension->DriverExtension->DataQueueSize)
		{
			/*
			 * We're exceeding the buffer, and data will be thrown away...
			 * FIXME: What could we do, as we are at DISPATCH_LEVEL?
			 */
			ReadSize = ClassDeviceExtension->DriverExtension->DataQueueSize - ClassDeviceExtension->InputCount;
		}
		else
			ReadSize = InputCount;

		/*
		 * Move the input data from the port data queue to our class data
		 * queue.
		 */
		RtlCopyMemory(
			&ClassDeviceExtension->PortData[ClassDeviceExtension->InputCount],
			(PCHAR)DataStart,
			sizeof(KEYBOARD_INPUT_DATA) * ReadSize);

		/* Move the counter up */
		ClassDeviceExtension->InputCount += ReadSize;//修改InputCount，这是要拿锁的原因
		(*ConsumedCount) += (ULONG)ReadSize;

		/* Complete pending IRP (if any)，很重要 */
		if (ClassDeviceExtension->PendingIrp) //PendingIrp也可能被修改为NULL，这也是要拿锁的原因
			HandleReadIrp(ClassDeviceObject, ClassDeviceExtension->PendingIrp, FALSE);
	}
	KeReleaseSpinLock(&ClassDeviceExtension->SpinLock, OldIrql);

	TRACE_(CLASS_NAME, "Leaving ClassCallback()\n");
	return TRUE;
}

/* Send IOCTL_INTERNAL_*_CONNECT to port */
static NTSTATUS
ConnectPortDriver(
	IN PDEVICE_OBJECT PortDO,//向这个地方发送，不过实际CONNECT_DATA不在PortDO落脚，而是在kbdhid落脚
	IN PDEVICE_OBJECT ClassDO)//PortDO将挂在此ClassDO保存以归属ClassDO管理
{
	KEVENT Event;
	PIRP Irp;
	IO_STATUS_BLOCK IoStatus;
	CONNECT_DATA ConnectData;
	NTSTATUS Status;

	TRACE_(CLASS_NAME, "Connecting PortDO %p to ClassDO %p\n", PortDO, ClassDO);

	KeInitializeEvent(&Event, NotificationEvent, FALSE);

	ConnectData.ClassDeviceObject = ClassDO;
	ConnectData.ClassService      = ClassCallback;

        //创建一个irp把工作干完了
	Irp = IoBuildDeviceIoControlRequest(
		IOCTL_INTERNAL_KEYBOARD_CONNECT,
		PortDO, // next-lower driver's device object, which represents the target device
		&ConnectData, sizeof(CONNECT_DATA),
		NULL, 0, //OutputBuffer and OutputBufferLength
		TRUE, &Event, &IoStatus); //是InternalDeviceIoControl
	if (!Irp)
		return STATUS_INSUFFICIENT_RESOURCES;

	Status = IoCallDriver(PortDO, Irp);//发给PortDO

	if (Status == STATUS_PENDING)
		KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
	else
		IoStatus.Status = Status;

	if (NT_SUCCESS(IoStatus.Status)) //学习这种写法
	{
		ObReferenceObject(PortDO); //在时detach时去引用
		
		//下面把PortDO加入到ClassDO的链表中
		ExInterlockedInsertTailList(
			&((PCLASS_DEVICE_EXTENSION)ClassDO->DeviceExtension)->ListHead,
			&((PPORT_DEVICE_EXTENSION)PortDO->DeviceExtension)->ListEntry,
			&((PCLASS_DEVICE_EXTENSION)ClassDO->DeviceExtension)->ListSpinLock);
		
		if (ClassDO->StackSize <= PortDO->StackSize)
		{
			/* Increase the stack size, in case we have to
			 * forward some IRPs to the port device object
			 */
			ClassDO->StackSize = PortDO->StackSize + 1;//很有必要
		}
	}

	return IoStatus.Status;
}

/* Send IOCTL_INTERNAL_*_DISCONNECT to port + destroy the Port DO */
static VOID
DestroyPortDriver(
	IN PDEVICE_OBJECT PortDO)
{
	PPORT_DEVICE_EXTENSION DeviceExtension;
	PCLASS_DEVICE_EXTENSION ClassDeviceExtension;
	PCLASS_DRIVER_EXTENSION DriverExtension;
	KEVENT Event;
	PIRP Irp;
	IO_STATUS_BLOCK IoStatus;
	KIRQL OldIrql;
	NTSTATUS Status;

	TRACE_(CLASS_NAME, "Destroying PortDO %p\n", PortDO);

	DeviceExtension = (PPORT_DEVICE_EXTENSION)PortDO->DeviceExtension;
	ClassDeviceExtension = DeviceExtension->ClassDO->DeviceExtension;
	DriverExtension = IoGetDriverObjectExtension(PortDO->DriverObject, PortDO->DriverObject);

	/* Send IOCTL_INTERNAL_*_DISCONNECT */
	KeInitializeEvent(&Event, NotificationEvent, FALSE);
	Irp = IoBuildDeviceIoControlRequest(
		IOCTL_INTERNAL_KEYBOARD_DISCONNECT,
		PortDO, //目的地
		NULL, 0, //不用到输入buffer
		NULL, 0, //不用到输出buffer
		TRUE, &Event, &IoStatus);//是内部控制命令
	if (Irp)
	{
		Status = IoCallDriver(PortDO, Irp);//发给PortDO
		if (Status == STATUS_PENDING)
			KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
	}

	/* Remove from ClassDeviceExtension->ListHead list */
	KeAcquireSpinLock(&ClassDeviceExtension->ListSpinLock, &OldIrql);
	RemoveEntryList(&DeviceExtension->ListEntry);
	KeReleaseSpinLock(&ClassDeviceExtension->ListSpinLock, OldIrql);

	/* Remove entry from HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\[DeviceBaseName] */
	RtlDeleteRegistryValue(
		RTL_REGISTRY_DEVICEMAP,
		DriverExtension->DeviceBaseName.Buffer,
		ClassDeviceExtension->DeviceName);

	if (DeviceExtension->LowerDevice)
		IoDetachDevice(DeviceExtension->LowerDevice); //PortDO的DeviceExtension，注意是PortDO和下层设备对象连着的
	ObDereferenceObject(PortDO); //在ConnectPortDriver时拿了一个引用
 
	//当没有multi时，显然应该吧ClassDO一块删掉
	if (!DriverExtension->ConnectMultiplePorts && DeviceExtension->ClassDO)
	{
		ExFreePoolWithTag(ClassDeviceExtension->PortData, CLASS_TAG); //释放给队列分配的内存
		ExFreePoolWithTag((PVOID)ClassDeviceExtension->DeviceName, CLASS_TAG);
		IoDeleteDevice(DeviceExtension->ClassDO);
	}

	IoDeleteDevice(PortDO);
}

static NTSTATUS NTAPI
ClassAddDevice(
	IN PDRIVER_OBJECT DriverObject,
	IN PDEVICE_OBJECT Pdo)
{
	PCLASS_DRIVER_EXTENSION DriverExtension;
	PDEVICE_OBJECT Fdo = NULL;
	PPORT_DEVICE_EXTENSION DeviceExtension = NULL;
	NTSTATUS Status;

	TRACE_(CLASS_NAME, "ClassAddDevice called. Pdo = 0x%p\n", Pdo);

	DriverExtension = IoGetDriverObjectExtension(DriverObject, DriverObject);//想知道multi多少
	if (Pdo == NULL)
		/* We may get a NULL Pdo at the first call as we're a legacy driver. Ignore it */
		return STATUS_SUCCESS;

	/* Create new device object */
	Status = IoCreateDevice(
		DriverObject,
		sizeof(PORT_DEVICE_EXTENSION), //这是看出是创建PortDO
		NULL,
		Pdo->DeviceType,//必须为pdo的设备类型
		Pdo->Characteristics & FILE_DEVICE_SECURE_OPEN ? FILE_DEVICE_SECURE_OPEN : 0,
		FALSE,
		&Fdo);//输出
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "IoCreateDevice() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}
	IoSetStartIoAttributes(Fdo, TRUE, TRUE);

	DeviceExtension = (PPORT_DEVICE_EXTENSION)Fdo->DeviceExtension;
	RtlZeroMemory(DeviceExtension, sizeof(PORT_DEVICE_EXTENSION));
	DeviceExtension->Common.IsClassDO = FALSE;//是PortDO
	DeviceExtension->DeviceObject = Fdo;//刚创建的
	DeviceExtension->PnpState = dsStopped;
	Status = IoAttachDeviceToDeviceStackSafe(Fdo, Pdo, &DeviceExtension->LowerDevice);//可见PortDO与kbdhid相连
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "IoAttachDeviceToDeviceStackSafe() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}
	
	//下面可见刚创建的PortDO是过滤驱动程序
	if (DeviceExtension->LowerDevice->Flags & DO_POWER_PAGABLE) //等于低层设备对象的flag
		Fdo->Flags |= DO_POWER_PAGABLE;
	if (DeviceExtension->LowerDevice->Flags & DO_BUFFERED_IO)//等于低层设备对象的flag
		Fdo->Flags |= DO_BUFFERED_IO;
	if (DeviceExtension->LowerDevice->Flags & DO_DIRECT_IO)//等于低层设备对象的flag
		Fdo->Flags |= DO_DIRECT_IO;

	if (DriverExtension->ConnectMultiplePorts)
		DeviceExtension->ClassDO = DriverExtension->MainClassDeviceObject;//mainClassDO已经创建了，DriverEntry中
	else
	{
		/* We need a new class device object for this Fdo */
		//一对一，一个ClassDO对应一个PortDO
		Status = CreateClassDeviceObject(
			DriverObject,
			&DeviceExtension->ClassDO);//创建后也没怎么样，就是下面connect一下，算是把PortDO和ClassDO联系起来了
		if (!NT_SUCCESS(Status))
		{
			WARN_(CLASS_NAME, "CreateClassDeviceObject() failed with status 0x%08lx\n", Status);
			goto cleanup;
		}
	}
	Status = ConnectPortDriver(Fdo, DeviceExtension->ClassDO);//算是把PortDO和ClassDO联系起来了
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "ConnectPortDriver() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}
	Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

	/* Register interface ; ignore the error (if any) as having
	 * a registred interface is not so important... */
	Status = IoRegisterDeviceInterface( //注册接口并创建一个该类接口的实例，接下来可被使能
		Pdo,//IO manager传来我们需要的
		&GUID_DEVINTERFACE_KEYBOARD,//键盘接口类
		NULL,
		&DeviceExtension->InterfaceName);//输出一个接口名，保存起来，在接下来PortDO收到IRP_MN_START_DEVICE时再打开用
	if (!NT_SUCCESS(Status))
		DeviceExtension->InterfaceName.Length = 0;

	return STATUS_SUCCESS;

cleanup:
	if (Fdo)
		DestroyPortDriver(Fdo);
	return Status;
}

static VOID NTAPI
ClassCancelRoutine(
	IN PDEVICE_OBJECT DeviceObject,//ClassDO，因为PendingIrp挂在ClassDO的设备扩展上
	IN PIRP Irp)
{
	PCLASS_DEVICE_EXTENSION ClassDeviceExtension = DeviceObject->DeviceExtension;
	KIRQL OldIrql;
	BOOLEAN wasQueued = FALSE;

	TRACE_(CLASS_NAME, "ClassCancelRoutine(DeviceObject %p, Irp %p)\n", DeviceObject, Irp);

	ASSERT(ClassDeviceExtension->Common.IsClassDO);//此时必须ClassDO拥有irp
	IoReleaseCancelSpinLock(Irp->CancelIrql); //在某个竞争环境中已经拿了spin锁，所以这里要释放锁
	KeAcquireSpinLock(&ClassDeviceExtension->SpinLock, &OldIrql);

	if (ClassDeviceExtension->PendingIrp == Irp)
	{
		ClassDeviceExtension->PendingIrp = NULL;//将被直接以STATUS_CANCELLED的方式完成，不会再有挂着未完成的irp了
		wasQueued = TRUE;
	}
	KeReleaseSpinLock(&ClassDeviceExtension->SpinLock, OldIrql);

	if (wasQueued)
	{
		Irp->IoStatus.Status = STATUS_CANCELLED;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);//所谓的cancle就是以STATUS_CANCELLED的方式完成
	}
	else
	{
		DPRINT1("Cancelled IRP is not pending. Race condition?\n");
	}
}

static NTSTATUS
HandleReadIrp(
	IN PDEVICE_OBJECT DeviceObject, //ClassPDO负责场面上的事情（读，取消）
	IN PIRP Irp,
	BOOLEAN IsInStartIo)
{
	PCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	NTSTATUS Status;
	KIRQL OldIrql;

	TRACE_(CLASS_NAME, "HandleReadIrp(DeviceObject %p, Irp %p)\n", DeviceObject, Irp);

	ASSERT(DeviceExtension->Common.IsClassDO);

	if (DeviceExtension->InputCount > 0) //缓冲区有键盘输入（KEYBOARD_INPUT_DATA）
	{
		SIZE_T NumberOfEntries;

		NumberOfEntries = MIN(
			DeviceExtension->InputCount,
			IoGetCurrentIrpStackLocation(Irp)->Parameters.Read.Length / sizeof(KEYBOARD_INPUT_DATA));

		Status = FillEntries(
			DeviceObject,
			Irp,
			DeviceExtension->PortData,
			NumberOfEntries);

		if (NT_SUCCESS(Status))
		{
			if (DeviceExtension->InputCount > NumberOfEntries)
			{
				RtlMoveMemory(
					&DeviceExtension->PortData[0],
					&DeviceExtension->PortData[NumberOfEntries],
					(DeviceExtension->InputCount - NumberOfEntries) * sizeof(KEYBOARD_INPUT_DATA));
			}

			DeviceExtension->InputCount -= NumberOfEntries;

			Irp->IoStatus.Information = NumberOfEntries * sizeof(KEYBOARD_INPUT_DATA);
		}

		/* Go to next packet and complete this request */
		Irp->IoStatus.Status = Status;

		(VOID)IoSetCancelRoutine(Irp, NULL);//irp不可取消了，此时应当是拿着spin锁的
		IoCompleteRequest(Irp, IO_KEYBOARD_INCREMENT);//成功地完成了一次键盘输入
		DeviceExtension->PendingIrp = NULL;
	}
	else
	{
		//缓冲区空空的，IRP只能挂在那里了
		IoAcquireCancelSpinLock(&OldIrql); // synchronizes cancelable-state transitions for IRPs 
		                                   //in a multiprocessor-safe way.
		if (Irp->Cancel) //注意这是个布尔变量
		{
			DeviceExtension->PendingIrp = NULL;
			Status = STATUS_CANCELLED;
		}
		else
		{
			IoMarkIrpPending(Irp);
			DeviceExtension->PendingIrp = Irp;
			(VOID)IoSetCancelRoutine(Irp, ClassCancelRoutine);
			Status = STATUS_PENDING;
		}
		IoReleaseCancelSpinLock(OldIrql);
	}
	return Status;
}

static NTSTATUS NTAPI
ClassPnp(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	PPORT_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	OBJECT_ATTRIBUTES ObjectAttributes;
	IO_STATUS_BLOCK Iosb;
	NTSTATUS Status;
	
	switch (IrpSp->MinorFunction)
	{
		case IRP_MN_START_DEVICE:
		    Status = ForwardIrpAndWait(DeviceObject, Irp);
			if (NT_SUCCESS(Status))
			{
				InitializeObjectAttributes(&ObjectAttributes,
										   &DeviceExtension->InterfaceName,
										   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
										   NULL,
										   NULL);

				Status = ZwOpenFile(&DeviceExtension->FileHandle, //输出
									FILE_READ_DATA,
									&ObjectAttributes,
									&Iosb,
									0,
									0);
				if (!NT_SUCCESS(Status))
					DeviceExtension->FileHandle = NULL;
			}
			else
				DeviceExtension->FileHandle = NULL;
			Irp->IoStatus.Status = Status;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			return Status;
			
		case IRP_MN_STOP_DEVICE:
			if (DeviceExtension->FileHandle)
			{
				ZwClose(DeviceExtension->FileHandle);
				DeviceExtension->FileHandle = NULL;
			}
			Status = STATUS_SUCCESS;
			break;
            
        case IRP_MN_REMOVE_DEVICE:
            if (DeviceExtension->FileHandle)
			{
				ZwClose(DeviceExtension->FileHandle);
				DeviceExtension->FileHandle = NULL;
			}
            IoSkipCurrentIrpStackLocation(Irp);
		    Status = IoCallDriver(DeviceExtension->LowerDevice, Irp);
            DestroyPortDriver(DeviceObject);
			return Status;

		default:
			Status = Irp->IoStatus.Status;
			break;
	}

	Irp->IoStatus.Status = Status;
	if (NT_SUCCESS(Status) || Status == STATUS_NOT_SUPPORTED)
	{
		IoSkipCurrentIrpStackLocation(Irp);
		return IoCallDriver(DeviceExtension->LowerDevice, Irp);
	}
	else
	{
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return Status;
	}
}

static VOID NTAPI
ClassStartIo(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp)
{
	PCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
	KIRQL OldIrql;

	TRACE_(CLASS_NAME, "ClassStartIo(DeviceObject %p, Irp %p)\n", DeviceObject, Irp);

	ASSERT(DeviceExtension->Common.IsClassDO);

	KeAcquireSpinLock(&DeviceExtension->SpinLock, &OldIrql);
	HandleReadIrp(DeviceObject, Irp, TRUE);
	KeReleaseSpinLock(&DeviceExtension->SpinLock, OldIrql);
}

static VOID NTAPI
SearchForLegacyDrivers(
	IN PDRIVER_OBJECT DriverObject,
	IN PVOID Context, /* PCLASS_DRIVER_EXTENSION */
	IN ULONG Count)
{
	UNICODE_STRING DeviceMapKeyU = RTL_CONSTANT_STRING(L"\\REGISTRY\\MACHINE\\HARDWARE\\DEVICEMAP");
	PCLASS_DRIVER_EXTENSION DriverExtension;
	UNICODE_STRING PortBaseName = { 0, 0, NULL };
	PKEY_VALUE_BASIC_INFORMATION KeyValueInformation = NULL;
	OBJECT_ATTRIBUTES ObjectAttributes;
	HANDLE hDeviceMapKey = (HANDLE)-1;
	HANDLE hPortKey = (HANDLE)-1;
	ULONG Index = 0;
	ULONG Size, ResultLength;
	NTSTATUS Status;

	TRACE_(CLASS_NAME, "SearchForLegacyDrivers(%p %p %lu)\n",
		DriverObject, Context, Count);

	if (Count != 1)
		return;
	DriverExtension = (PCLASS_DRIVER_EXTENSION)Context;

	/* Create port base name, by replacing Class by Port at the end of the class base name */
	Status = DuplicateUnicodeString(
		RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
		&DriverExtension->DeviceBaseName,
		&PortBaseName);
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "DuplicateUnicodeString() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}
	PortBaseName.Length -= (sizeof(L"Class") - sizeof(UNICODE_NULL));
	RtlAppendUnicodeToString(&PortBaseName, L"Port");

	/* Allocate memory */
	Size = sizeof(KEY_VALUE_BASIC_INFORMATION) + MAX_PATH;
	KeyValueInformation = ExAllocatePoolWithTag(PagedPool, Size, CLASS_TAG);
	if (!KeyValueInformation)
	{
		WARN_(CLASS_NAME, "ExAllocatePoolWithTag() failed\n");
		Status = STATUS_NO_MEMORY;
		goto cleanup;
	}

	/* Open HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP */
	InitializeObjectAttributes(&ObjectAttributes, &DeviceMapKeyU, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
	Status = ZwOpenKey(&hDeviceMapKey, 0, &ObjectAttributes);
	if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
	{
		INFO_(CLASS_NAME, "HKLM\\HARDWARE\\DEVICEMAP is non-existent\n");
		Status = STATUS_SUCCESS;
		goto cleanup;
	}
	else if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "ZwOpenKey() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}

	/* Open sub key */
	InitializeObjectAttributes(&ObjectAttributes, &PortBaseName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, hDeviceMapKey, NULL);
	Status = ZwOpenKey(&hPortKey, KEY_QUERY_VALUE, &ObjectAttributes);
	if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
	{
		INFO_(CLASS_NAME, "HKLM\\HARDWARE\\DEVICEMAP\\%wZ is non-existent\n", &PortBaseName);
		Status = STATUS_SUCCESS;
		goto cleanup;
	}
	else if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "ZwOpenKey() failed with status 0x%08lx\n", Status);
		goto cleanup;
	}

	/* Read each value name */
	while (ZwEnumerateValueKey(hPortKey, Index++, KeyValueBasicInformation, KeyValueInformation, Size, &ResultLength) == STATUS_SUCCESS)
	{
		UNICODE_STRING PortName;
		PDEVICE_OBJECT PortDeviceObject = NULL;
		PFILE_OBJECT FileObject = NULL;

		PortName.Length = PortName.MaximumLength = (USHORT)KeyValueInformation->NameLength;
		PortName.Buffer = KeyValueInformation->Name;

		/* Open the device object pointer */
		Status = IoGetDeviceObjectPointer(&PortName, FILE_READ_ATTRIBUTES, &FileObject, &PortDeviceObject);
		if (!NT_SUCCESS(Status))
		{
			WARN_(CLASS_NAME, "IoGetDeviceObjectPointer(%wZ) failed with status 0x%08lx\n", &PortName, Status);
			continue;
		}
		INFO_(CLASS_NAME, "Legacy driver found\n");

		Status = ClassAddDevice(DriverObject, PortDeviceObject);
		if (!NT_SUCCESS(Status))
		{
			/* FIXME: Log the error */
			WARN_(CLASS_NAME, "ClassAddDevice() failed with status 0x%08lx\n", Status);
		}

		ObDereferenceObject(FileObject);
	}

cleanup:
	if (KeyValueInformation != NULL)
		ExFreePoolWithTag(KeyValueInformation, CLASS_TAG);
	if (hDeviceMapKey != (HANDLE)-1)
		ZwClose(hDeviceMapKey);
	if (hPortKey != (HANDLE)-1)
		ZwClose(hPortKey);
}

/*
 * Standard DriverEntry method.
 */
NTSTATUS NTAPI
DriverEntry(
	IN PDRIVER_OBJECT DriverObject,
	IN PUNICODE_STRING RegistryPath)
{
	PCLASS_DRIVER_EXTENSION DriverExtension;
	ULONG i;
	NTSTATUS Status;

	Status = IoAllocateDriverObjectExtension(
		DriverObject,
		DriverObject,
		sizeof(CLASS_DRIVER_EXTENSION),
		(PVOID*)&DriverExtension);
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "IoAllocateDriverObjectExtension() failed with status 0x%08lx\n", Status);
		return Status;
	}
	RtlZeroMemory(DriverExtension, sizeof(CLASS_DRIVER_EXTENSION));

	Status = DuplicateUnicodeString(
		RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
		RegistryPath,
		&DriverExtension->RegistryPath);
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "DuplicateUnicodeString() failed with status 0x%08lx\n", Status);
		return Status;
	}

	Status = ReadRegistryEntries(RegistryPath, DriverExtension);
	if (!NT_SUCCESS(Status))
	{
		WARN_(CLASS_NAME, "ReadRegistryEntries() failed with status 0x%08lx\n", Status);
		return Status;
	}

	if (DriverExtension->ConnectMultiplePorts == 1)
	{
		Status = CreateClassDeviceObject(
			DriverObject,
			&DriverExtension->MainClassDeviceObject);
		if (!NT_SUCCESS(Status))
		{
			WARN_(CLASS_NAME, "CreateClassDeviceObject() failed with status 0x%08lx\n", Status);
			return Status;
		}
	}

	DriverObject->DriverExtension->AddDevice = ClassAddDevice;
	DriverObject->DriverUnload = DriverUnload;

	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		DriverObject->MajorFunction[i] = IrpStub;

	DriverObject->MajorFunction[IRP_MJ_CREATE]         = ClassCreate;
	DriverObject->MajorFunction[IRP_MJ_CLOSE]          = ClassClose;
	DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = ClassCleanup;
	DriverObject->MajorFunction[IRP_MJ_READ]           = ClassRead;
	DriverObject->MajorFunction[IRP_MJ_PNP]            = ClassPnp;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ClassDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = ForwardIrpAndForget;
	DriverObject->DriverStartIo                        = ClassStartIo;

	/* We will detect the legacy devices later */
	IoRegisterDriverReinitialization(
		DriverObject,
		SearchForLegacyDrivers,
		DriverExtension);

	return STATUS_SUCCESS;
}
