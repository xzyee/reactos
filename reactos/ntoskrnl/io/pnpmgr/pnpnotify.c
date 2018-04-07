/*
 * PROJECT:         ReactOS Kernel
 * COPYRIGHT:       GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/pnpmgr/pnpnotify.c
 * PURPOSE:         Plug & Play notification functions
 * PROGRAMMERS:     Filip Navara (xnavara@volny.cz)
 *                  Hervé Poussineau (hpoussin@reactos.org)
 *                  Pierre Schweitzer
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES *******************************************************************/

typedef struct _PNP_NOTIFY_ENTRY
{
    LIST_ENTRY PnpNotifyList; //腰带，要点是放在最前面
    IO_NOTIFICATION_EVENT_CATEGORY EventCategory;//先比较这个
    PVOID Context;
    UNICODE_STRING Guid;//再比较这个
    PFILE_OBJECT FileObject; //值得注意有fileobject对象，注册时填入
    PDRIVER_OBJECT DriverObject;//驱动对象的目的在于防止卸载，而不是用其携带的信息
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE PnpNotificationProc;
} PNP_NOTIFY_ENTRY, *PPNP_NOTIFY_ENTRY;

KGUARDED_MUTEX PnpNotifyListLock;
LIST_ENTRY PnpNotifyListHead;

/* FUNCTIONS *****************************************************************/

VOID
IopNotifyPlugPlayNotification(
	IN PDEVICE_OBJECT DeviceObject,
	IN IO_NOTIFICATION_EVENT_CATEGORY EventCategory,
	IN LPCGUID Event,
	IN PVOID EventCategoryData1,
	IN PVOID EventCategoryData2)
{
	PPNP_NOTIFY_ENTRY ChangeEntry;
	PLIST_ENTRY ListEntry;
	PVOID NotificationStructure;
	BOOLEAN CallCurrentEntry;
	UNICODE_STRING GuidString;
	NTSTATUS Status;
	PDEVICE_OBJECT EntryDeviceObject = NULL;

	ASSERT(DeviceObject);

	KeAcquireGuardedMutex(&PnpNotifyListLock);
	if (IsListEmpty(&PnpNotifyListHead))
	{
		KeReleaseGuardedMutex(&PnpNotifyListLock);
		return;
	}

	switch (EventCategory) //本质上是重新解释参数并重构调用参数的过程
	{
		case EventCategoryDeviceInterfaceChange:
		{
			PDEVICE_INTERFACE_CHANGE_NOTIFICATION NotificationInfos;//造一个调用需要的参数结构
			NotificationStructure = NotificationInfos = ExAllocatePoolWithTag(
				PagedPool,
				sizeof(DEVICE_INTERFACE_CHANGE_NOTIFICATION),
				TAG_PNP_NOTIFY);
			if (!NotificationInfos)
			{
				KeReleaseGuardedMutex(&PnpNotifyListLock);
				return;
			}
			NotificationInfos->Version = 1;
			NotificationInfos->Size = sizeof(DEVICE_INTERFACE_CHANGE_NOTIFICATION);
			RtlCopyMemory(&NotificationInfos->Event, Event/*来自参数*/, sizeof(GUID));
			RtlCopyMemory(&NotificationInfos->InterfaceClassGuid, EventCategoryData1/*来自参数*/, sizeof(GUID));
			NotificationInfos->SymbolicLinkName = (PUNICODE_STRING)EventCategoryData2/*来自参数*/;
			Status = RtlStringFromGUID(&NotificationInfos->InterfaceClassGuid, &GuidString/*输出*/);
			if (!NT_SUCCESS(Status))
			{
				KeReleaseGuardedMutex(&PnpNotifyListLock);
				ExFreePoolWithTag(NotificationStructure, TAG_PNP_NOTIFY);
				return;
			}
			break;
		}
		case EventCategoryHardwareProfileChange:
		{
			PHWPROFILE_CHANGE_NOTIFICATION NotificationInfos;//造一个调用需要的参数结构
			NotificationStructure = NotificationInfos = ExAllocatePoolWithTag(
				PagedPool,
				sizeof(HWPROFILE_CHANGE_NOTIFICATION),
				TAG_PNP_NOTIFY);
			if (!NotificationInfos)
			{
				KeReleaseGuardedMutex(&PnpNotifyListLock);
				return;
			}
			NotificationInfos->Version = 1;
			NotificationInfos->Size = sizeof(HWPROFILE_CHANGE_NOTIFICATION);
			RtlCopyMemory(&NotificationInfos->Event, Event/*来自参数*/, sizeof(GUID));
			break;
		}
		case EventCategoryTargetDeviceChange:
		{
			if (Event != &GUID_PNP_CUSTOM_NOTIFICATION)
			{
				PTARGET_DEVICE_REMOVAL_NOTIFICATION NotificationInfos;//造一个调用需要的参数结构
				NotificationStructure = NotificationInfos = ExAllocatePoolWithTag(
					PagedPool,
					sizeof(TARGET_DEVICE_REMOVAL_NOTIFICATION),
					TAG_PNP_NOTIFY);
				if (!NotificationInfos)
				{
					KeReleaseGuardedMutex(&PnpNotifyListLock);
					return;
				}
				NotificationInfos->Version = 1;
				NotificationInfos->Size = sizeof(TARGET_DEVICE_REMOVAL_NOTIFICATION);
				RtlCopyMemory(&NotificationInfos->Event, Event/*来自参数*/, sizeof(GUID));
			}
			else
			{
				PTARGET_DEVICE_CUSTOM_NOTIFICATION NotificationInfos;
				NotificationStructure = NotificationInfos = ExAllocatePoolWithTag(
					PagedPool,
					sizeof(TARGET_DEVICE_CUSTOM_NOTIFICATION),
					TAG_PNP_NOTIFY);
				if (!NotificationInfos)
				{
					KeReleaseGuardedMutex(&PnpNotifyListLock);
					return;
				}
				RtlCopyMemory(NotificationInfos, EventCategoryData1/*来自参数*/, sizeof(TARGET_DEVICE_CUSTOM_NOTIFICATION));
			}
			break;
		}
		default:
		{
			DPRINT1("IopNotifyPlugPlayNotification(): unknown EventCategory 0x%x UNIMPLEMENTED\n", EventCategory);
			KeReleaseGuardedMutex(&PnpNotifyListLock);
			return;
		}
	}

	/* Loop through procedures registred in PnpNotifyListHead
	 * list to find those that meet some criteria.
	 */
	ListEntry = PnpNotifyListHead.Flink; //全局变量
	while (ListEntry != &PnpNotifyListHead)
	{
		ChangeEntry = CONTAINING_RECORD(ListEntry, PNP_NOTIFY_ENTRY, PnpNotifyList);
		CallCurrentEntry = FALSE;

		if (ChangeEntry->EventCategory != EventCategory)
		{
			ListEntry = ListEntry->Flink;
			continue;
		}

		switch (EventCategory)
		{
			case EventCategoryDeviceInterfaceChange:
			{
				if (RtlCompareUnicodeString(&ChangeEntry->Guid, &GuidString, FALSE) == 0)
				{
					CallCurrentEntry = TRUE;
				}
				break;
			}
			case EventCategoryHardwareProfileChange:
			{
				CallCurrentEntry = TRUE;
				break;
			}
			case EventCategoryTargetDeviceChange:
			{
				Status = IoGetRelatedTargetDevice(ChangeEntry->FileObject, &EntryDeviceObject);
				if (NT_SUCCESS(Status))
				{
					if (DeviceObject == EntryDeviceObject)
					{
						if (Event == &GUID_PNP_CUSTOM_NOTIFICATION)
						{
							((PTARGET_DEVICE_CUSTOM_NOTIFICATION)NotificationStructure)->FileObject = ChangeEntry->FileObject;
						}
						else
						{
							((PTARGET_DEVICE_REMOVAL_NOTIFICATION)NotificationStructure)->FileObject = ChangeEntry->FileObject;
						}
						CallCurrentEntry = TRUE;
					}
				}
				break;
			}
			default:
			{
				DPRINT1("IopNotifyPlugPlayNotification(): unknown EventCategory 0x%x UNIMPLEMENTED\n", EventCategory);
				break;
			}
		}

		/* Move to the next element now, as callback may unregister itself */
		ListEntry = ListEntry->Flink;
		/* FIXME: If ListEntry was the last element and that callback registers
		 * new notifications, those won't be checked... */

		if (CallCurrentEntry)
		{
			/* Call entry into new allocated memory */
			DPRINT("IopNotifyPlugPlayNotification(): found suitable callback %p\n",
				ChangeEntry);

			KeReleaseGuardedMutex(&PnpNotifyListLock);
			(ChangeEntry->PnpNotificationProc)( //调用回调！！！
				NotificationStructure,
				ChangeEntry->Context);
			KeAcquireGuardedMutex(&PnpNotifyListLock);
		}

	}
	KeReleaseGuardedMutex(&PnpNotifyListLock);
	ExFreePoolWithTag(NotificationStructure, TAG_PNP_NOTIFY);
	if (EventCategory == EventCategoryDeviceInterfaceChange)
		RtlFreeUnicodeString(&GuidString);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @unimplemented
 */
ULONG
NTAPI
IoPnPDeliverServicePowerNotification(ULONG VetoedPowerOperation OPTIONAL,
                                     ULONG PowerNotification,
                                     ULONG Unknown OPTIONAL,
                                     BOOLEAN Synchronous)
{
    UNIMPLEMENTED;
    return 0;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoRegisterPlugPlayNotification(IN IO_NOTIFICATION_EVENT_CATEGORY EventCategory,
                               IN ULONG EventCategoryFlags,
                               IN PVOID EventCategoryData OPTIONAL,
                               IN PDRIVER_OBJECT DriverObject,
                               IN PDRIVER_NOTIFICATION_CALLBACK_ROUTINE CallbackRoutine,
                               IN PVOID Context,
                               OUT PVOID *NotificationEntry)
{
    PPNP_NOTIFY_ENTRY Entry;
    PWSTR SymbolicLinkList;
    NTSTATUS Status;
    PAGED_CODE();

    DPRINT("%s(EventCategory 0x%x, EventCategoryFlags 0x%lx, DriverObject %p) called.\n",
           __FUNCTION__,
           EventCategory,
           EventCategoryFlags,
           DriverObject);

    ObReferenceObject(DriverObject);

    /* Try to allocate entry for notification before sending any notification */
    Entry = ExAllocatePoolWithTag(NonPagedPool,
                                  sizeof(PNP_NOTIFY_ENTRY),
                                  TAG_PNP_NOTIFY);

    if (!Entry)
    {
        DPRINT("ExAllocatePool() failed\n");
        ObDereferenceObject(DriverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES的意思是现在就要调用
    if (EventCategory == EventCategoryDeviceInterfaceChange	&&
        EventCategoryFlags & PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES)
    {
        DEVICE_INTERFACE_CHANGE_NOTIFICATION NotificationInfos;
        UNICODE_STRING SymbolicLinkU;
        PWSTR SymbolicLink;

        Status = IoGetDeviceInterfaces((LPGUID)EventCategoryData,
                                       NULL, /* PhysicalDeviceObject OPTIONAL */
                                       0, /* Flags */
                                       &SymbolicLinkList);
        if (NT_SUCCESS(Status))
        {
            /* Enumerate SymbolicLinkList */
            NotificationInfos.Version = 1;
            NotificationInfos.Size = sizeof(DEVICE_INTERFACE_CHANGE_NOTIFICATION);
            RtlCopyMemory(&NotificationInfos.Event,
                          &GUID_DEVICE_INTERFACE_ARRIVAL,
                          sizeof(GUID));
            RtlCopyMemory(&NotificationInfos.InterfaceClassGuid,
                          EventCategoryData,
                          sizeof(GUID));
            NotificationInfos.SymbolicLinkName = &SymbolicLinkU;

            for (SymbolicLink = SymbolicLinkList;
                 *SymbolicLink;
                 SymbolicLink += wcslen(SymbolicLink) + 1)
            {
                RtlInitUnicodeString(&SymbolicLinkU, SymbolicLink);
                DPRINT("Calling callback routine for %S\n", SymbolicLink);
                (*CallbackRoutine)(&NotificationInfos, Context);
            }

            ExFreePool(SymbolicLinkList);
        }
    }

    Entry->PnpNotificationProc = CallbackRoutine;
    Entry->EventCategory = EventCategory;
    Entry->Context = Context;
    Entry->DriverObject = DriverObject;
    switch (EventCategory)
    {
        case EventCategoryDeviceInterfaceChange:
        {
            Status = RtlStringFromGUID(EventCategoryData, &Entry->Guid);
            if (!NT_SUCCESS(Status))
            {
                ExFreePoolWithTag(Entry, TAG_PNP_NOTIFY);
                ObDereferenceObject(DriverObject);
                return Status;
            }
            break;
        }
        case EventCategoryHardwareProfileChange:
        {
            /* nothing to do */
           break;
        }
        case EventCategoryTargetDeviceChange:
        {
            Entry->FileObject = (PFILE_OBJECT)EventCategoryData;
            break;
        }
        default:
        {
            DPRINT1("%s: unknown EventCategory 0x%x UNIMPLEMENTED\n",
                    __FUNCTION__, EventCategory);
            break;
        }
    }

    KeAcquireGuardedMutex(&PnpNotifyListLock);
    InsertHeadList(&PnpNotifyListHead,
                   &Entry->PnpNotifyList);
    KeReleaseGuardedMutex(&PnpNotifyListLock);

    DPRINT("%s returns NotificationEntry %p\n", __FUNCTION__, Entry);

    *NotificationEntry = Entry;

    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoUnregisterPlugPlayNotification(IN PVOID NotificationEntry)
{
    PPNP_NOTIFY_ENTRY Entry;
    PAGED_CODE();

    Entry = (PPNP_NOTIFY_ENTRY)NotificationEntry;
    DPRINT("%s(NotificationEntry %p) called\n", __FUNCTION__, Entry);

    KeAcquireGuardedMutex(&PnpNotifyListLock);
    RemoveEntryList(&Entry->PnpNotifyList);
    KeReleaseGuardedMutex(&PnpNotifyListLock);

    RtlFreeUnicodeString(&Entry->Guid);

    ObDereferenceObject(Entry->DriverObject);

    ExFreePoolWithTag(Entry, TAG_PNP_NOTIFY);

    return STATUS_SUCCESS;
}
