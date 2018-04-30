/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/driver.c
 * PURPOSE:         Driver Object Management
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Filip Navara (navaraf@reactos.org)
 *                  Hervé Poussineau (hpoussin@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

LIST_ENTRY DriverReinitListHead;
KSPIN_LOCK DriverReinitListLock;
PLIST_ENTRY DriverReinitTailEntry;

PLIST_ENTRY DriverBootReinitTailEntry;
LIST_ENTRY DriverBootReinitListHead;
KSPIN_LOCK DriverBootReinitListLock;

UNICODE_STRING IopHardwareDatabaseKey =
   RTL_CONSTANT_STRING(L"\\REGISTRY\\MACHINE\\HARDWARE\\DESCRIPTION\\SYSTEM");

POBJECT_TYPE IoDriverObjectType = NULL;

#define TAG_RTLREGISTRY 'vrqR'

extern BOOLEAN ExpInTextModeSetup;
extern BOOLEAN PnpSystemInit;

USHORT IopGroupIndex;
PLIST_ENTRY IopGroupTable;

/*要查的注册表
L"Start";
L"ImagePath";//加载和卸载都要用
L"LowerFilters";
L"UpperFilters";
L"ClassGUID"; //加载类驱动时需要
L"Type";
*/
/* PRIVATE FUNCTIONS **********************************************************/

NTSTATUS NTAPI
IopInvalidDeviceRequest( //stub
   PDEVICE_OBJECT DeviceObject,
   PIRP Irp)
{
   Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
   Irp->IoStatus.Information = 0;
   IoCompleteRequest(Irp, IO_NO_INCREMENT);
   return STATUS_INVALID_DEVICE_REQUEST;
}

/*
此时 ，没有设备对象了，都删除了
1、找出当前DriverObject对应的DriverExtension，即DriverObject->DriverExtension下面的PIO_CLIENT_EXTENSION串串
把所有的PIO_CLIENT_EXTENSION结构都删除干净
2、把DriverObject->DriverSection所指的image映像unload
3、释放DriverExtension->ServiceKeyName
*/
VOID
NTAPI
IopDeleteDriver(IN PVOID ObjectBody)
{
    PDRIVER_OBJECT DriverObject = ObjectBody;
    PIO_CLIENT_EXTENSION DriverExtension, NextDriverExtension;
    PAGED_CODE();

    DPRINT1("Deleting driver object '%wZ'\n", &DriverObject->DriverName);

    /* There must be no device objects remaining at this point */
    ASSERT(!DriverObject->DeviceObject);

    /* Get the extension and loop them */
    DriverExtension = IoGetDrvObjExtension(DriverObject)->
                      ClientDriverExtension;
    while (DriverExtension)
    {
        /* Get the next one */
        NextDriverExtension = DriverExtension->NextExtension;
        ExFreePoolWithTag(DriverExtension, TAG_DRIVER_EXTENSION);

        /* Move on */
        DriverExtension = NextDriverExtension;
    }

    /* Check if the driver image is still loaded */
    if (DriverObject->DriverSection)
    {
        /* Unload it */
        MmUnloadSystemImage(DriverObject->DriverSection);
    }

    /* Check if it has a name */
    if (DriverObject->DriverName.Buffer)
    {
        /* Free it */
        ExFreePool(DriverObject->DriverName.Buffer);
    }

    /* Check if it has a service key name */
    if (DriverObject->DriverExtension->ServiceKeyName.Buffer)
    {
        /* Free it */
        ExFreePool(DriverObject->DriverExtension->ServiceKeyName.Buffer);
    }
}

//不是表面上看起来的Get
//ObReferenceObjectByName,对象名称为/"\Driver\$ServiceName"或者"\FileSystem\$ServiceName"
NTSTATUS FASTCALL
IopGetDriverObject(
   PDRIVER_OBJECT *DriverObject,
   PUNICODE_STRING ServiceName,
   BOOLEAN FileSystem)
{
   PDRIVER_OBJECT Object;
   WCHAR NameBuffer[MAX_PATH];
   UNICODE_STRING DriverName;
   NTSTATUS Status;

   DPRINT("IopGetDriverObject(%p '%wZ' %x)\n",
      DriverObject, ServiceName, FileSystem);

   *DriverObject = NULL;

   /* Create ModuleName string */
   if (ServiceName == NULL || ServiceName->Buffer == NULL)
      /* We don't know which DriverObject we have to open */
      return STATUS_INVALID_PARAMETER_2;

   DriverName.Buffer = NameBuffer;
   DriverName.Length = 0;
   DriverName.MaximumLength = sizeof(NameBuffer);

   if (FileSystem == TRUE)
      RtlAppendUnicodeToString(&DriverName, FILESYSTEM_ROOT_NAME);//L"\\FileSystem\\"
   else
      RtlAppendUnicodeToString(&DriverName, DRIVER_ROOT_NAME); //L"\\Driver\\"
   RtlAppendUnicodeStringToString(&DriverName, ServiceName);

   DPRINT("Driver name: '%wZ'\n", &DriverName);

   /* Open driver object */
   Status = ObReferenceObjectByName(
      &DriverName, //"\Driver\$ServiceName"或者"\FileSystem\$ServiceName"
      OBJ_OPENIF | OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, /* Attributes */
      NULL, /* PassedAccessState */
      0, /* DesiredAccess */
      IoDriverObjectType,
      KernelMode,
      NULL, /* ParseContext */
      (PVOID*)&Object);//输出
 
   if (!NT_SUCCESS(Status))
   {
      DPRINT("Failed to reference driver object, status=0x%08x\n", Status);
      return Status;
   }

   *DriverObject = Object;

   DPRINT("Driver Object: %p\n", Object);

   return STATUS_SUCCESS;
}

/*
 * RETURNS
 *  TRUE if String2 contains String1 as a suffix.
 */
BOOLEAN
NTAPI
IopSuffixUnicodeString(
    IN PCUNICODE_STRING String1, //比如：".bbb"
    IN PCUNICODE_STRING String2) //比如："aaaa.bbb"
{
    PWCHAR pc1;
    PWCHAR pc2;
    ULONG Length;

    if (String2->Length < String1->Length)
        return FALSE;

    Length = String1->Length / 2;
    pc1 = String1->Buffer;
    pc2 = &String2->Buffer[String2->Length / sizeof(WCHAR) - Length];

    if (pc1 && pc2)
    {
        while (Length--)
        {
            if( *pc1++ != *pc2++ )
                return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

/*
 * IopDisplayLoadingMessage
 *
 * Display 'Loading XXX...' message.
 */

VOID
FASTCALL
INIT_FUNCTION
IopDisplayLoadingMessage(PUNICODE_STRING ServiceName)
{
    CHAR TextBuffer[256];
    UNICODE_STRING DotSys = RTL_CONSTANT_STRING(L".SYS");

    if (ExpInTextModeSetup) return;
    if (!KeLoaderBlock) return;
    RtlUpcaseUnicodeString(ServiceName, ServiceName, FALSE);
    snprintf(TextBuffer, sizeof(TextBuffer),
            "%s%sSystem32\\Drivers\\%wZ%s\r\n",
            KeLoaderBlock->ArcBootDeviceName,
            KeLoaderBlock->NtBootPathName,
            ServiceName,
            IopSuffixUnicodeString(&DotSys, ServiceName) ? "" : ".SYS");
    HalDisplayString(TextBuffer);
}

/*
 * IopNormalizeImagePath
 *
 * Normalize an image path to contain complete path.
 *
 * Parameters
 *    ImagePath
 *       The input path and on exit the result path. ImagePath.Buffer
 *       must be allocated by ExAllocatePool on input. Caller is responsible
 *       for freeing the buffer when it's no longer needed.
 *
 *    ServiceName
 *       Name of the service that ImagePath belongs to.
 *
 * Return Value
 *    Status
 *
 * Remarks
 *    The input image path isn't freed on error.
 */
 
/*
构造ImagePath字符串:
当ImagePath没东西时，输出为 "\\SystemRoot\\system32\\drivers\\xxx.sys"
当ImagePath有西时，输出为 "\\SystemRoot\\"+ImagePath
*/
NTSTATUS
FASTCALL
IopNormalizeImagePath(
   _Inout_ _When_(return>=0, _At_(ImagePath->Buffer, _Post_notnull_ __drv_allocatesMem(Mem)))
        PUNICODE_STRING ImagePath,
   _In_ PUNICODE_STRING ServiceName)
{
   UNICODE_STRING InputImagePath;

   DPRINT("Normalizing image path '%wZ' for service '%wZ'\n", ImagePath, ServiceName);

   RtlCopyMemory(
      &InputImagePath,
      ImagePath,
      sizeof(UNICODE_STRING));

   if (InputImagePath.Length == 0)
   {
      ImagePath->Length = 0;
      ImagePath->MaximumLength =
          (33 * sizeof(WCHAR)) + ServiceName->Length + sizeof(UNICODE_NULL);
      ImagePath->Buffer = ExAllocatePool(NonPagedPool, ImagePath->MaximumLength);
      if (ImagePath->Buffer == NULL)
         return STATUS_NO_MEMORY;

      RtlAppendUnicodeToString(ImagePath, L"\\SystemRoot\\system32\\drivers\\");
      RtlAppendUnicodeStringToString(ImagePath, ServiceName);
      RtlAppendUnicodeToString(ImagePath, L".sys");
   } else
   if (InputImagePath.Buffer[0] != L'\\')
   {
      ImagePath->Length = 0;
      ImagePath->MaximumLength =
          12 * sizeof(WCHAR) + InputImagePath.Length + sizeof(UNICODE_NULL);
      ImagePath->Buffer = ExAllocatePool(NonPagedPool, ImagePath->MaximumLength);
      if (ImagePath->Buffer == NULL)
         return STATUS_NO_MEMORY;

      RtlAppendUnicodeToString(ImagePath, L"\\SystemRoot\\");
      RtlAppendUnicodeStringToString(ImagePath, &InputImagePath);

      /* Free caller's string */
      ExFreePoolWithTag(InputImagePath.Buffer, TAG_RTLREGISTRY);
   }

   DPRINT("Normalized image path is '%wZ' for service '%wZ'\n", ImagePath, ServiceName);

   return STATUS_SUCCESS;
}

/*
 * IopLoadServiceModule
 *
 * Load a module specified by registry settings for service.
 *
 * Parameters
 *    ServiceName
 *       Name of the service to load.
 *
 * Return Value
 *    Status
 */
/*
1.从注册表..\CurrentControlSet\Services\$Service找到驱动文件保存的路径（ImagePath）和启动方式（ServiceStart）
2.调用MmLoadSystemImage得到ModuleObject
注意：ServiceStart >= 4的不会装载
*/
NTSTATUS FASTCALL
IopLoadServiceModule(
   IN PUNICODE_STRING ServiceName,
   OUT PLDR_DATA_TABLE_ENTRY *ModuleObject)
{
   RTL_QUERY_REGISTRY_TABLE QueryTable[3];
   ULONG ServiceStart;
   UNICODE_STRING ServiceImagePath, CCSName;
   NTSTATUS Status;
   HANDLE CCSKey, ServiceKey;
   PVOID BaseAddress;

   DPRINT("IopLoadServiceModule(%wZ, 0x%p)\n", ServiceName, ModuleObject);

   /* FIXME: This check may be removed once the bug is fixed */
   if (ServiceName->Buffer == NULL)
   {
       DPRINT1("If you see this, please report to Fireball or hpoussin!\n");
      return STATUS_UNSUCCESSFUL;
   }

   if (ExpInTextModeSetup) //全局变量
   {
       /* We have no registry, but luckily we know where all the drivers are */

       /* ServiceStart < 4 is all that matters */
       ServiceStart = 0;

       /* IopNormalizeImagePath will do all of the work for us if we give it an empty string */
       ServiceImagePath.Length = ServiceImagePath.MaximumLength = 0;
       ServiceImagePath.Buffer = NULL;
   }
   else
   {
       /* Open CurrentControlSet */
       RtlInitUnicodeString(&CCSName,
                            L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services");
       Status = IopOpenRegistryKeyEx(&CCSKey, NULL, &CCSName, KEY_READ);
...
       /* Open service key */
       Status = IopOpenRegistryKeyEx(&ServiceKey, CCSKey, ServiceName, KEY_READ);
...

       /*
        * Get information about the service.
        (1)ServiceStart,关于如何启动驱动
        (2)ServiceImagePath，关于驱动映像文件在保存在什么地方
        */

       RtlZeroMemory(QueryTable, sizeof(QueryTable));

       RtlInitUnicodeString(&ServiceImagePath, NULL);

       QueryTable[0].Name = L"Start";
       QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
       QueryTable[0].EntryContext = &ServiceStart;//ULONG
       QueryTable[1].Name = L"ImagePath";
       QueryTable[1].Flags = RTL_QUERY_REGISTRY_DIRECT;
       QueryTable[1].EntryContext = &ServiceImagePath;

       Status = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
          (PWSTR)ServiceKey, QueryTable, NULL, NULL);

       ZwClose(ServiceKey);
       ZwClose(CCSKey);
...
   }

   /*
    * Normalize the image path for all later processing.
    */

   Status = IopNormalizeImagePath(&ServiceImagePath, ServiceName);//修正一下ServiceImagePath使其完整
...
   /*
    * Case for disabled drivers
    */

   if (ServiceStart >= 4)
   {
      /* We can't load this */
      Status = STATUS_DRIVER_UNABLE_TO_LOAD;
   }
   else
   {
      DPRINT("Loading module from %wZ\n", &ServiceImagePath);
      Status = MmLoadSystemImage(&ServiceImagePath, NULL, NULL, 0, (PVOID)ModuleObject/*输出*/, &BaseAddress/*输出*/);
      if (NT_SUCCESS(Status))
      {
          IopDisplayLoadingMessage(ServiceName);
      }
   }

   ExFreePool(ServiceImagePath.Buffer);

   /*
    * Now check if the module was loaded successfully.
    */

   if (!NT_SUCCESS(Status))
   {
      DPRINT("Module loading failed (Status %x)\n", Status);
   }

   DPRINT("Module loading (Status %x)\n", Status);

   return Status;
}

VOID
NTAPI
MmFreeDriverInitialization(IN PLDR_DATA_TABLE_ENTRY LdrEntry);


/*
 * IopInitializeDriverModule
 *
 * Initialize a loaded driver.
 *
 * Parameters
 *    DeviceNode
 *       Pointer to device node.
 *
 *    ModuleObject
 *       Module object representing the driver. It can be retrieve by
 *       IopLoadServiceModule.
 *
 *    ServiceName
 *       Name of the service (as in registry).
 *
 *    FileSystemDriver
 *       Set to TRUE for file system drivers.
 *
 *    DriverObject
 *       On successful return this contains the driver object representing
 *       the loaded driver.
 */
/*通过ModuleObject创建DriverObject
输入调用IopCreateDriver创建DriverObject对象
*/
NTSTATUS FASTCALL
IopInitializeDriverModule(
   IN PDEVICE_NODE DeviceNode,//带有->ServiceName,没啥用
   IN PLDR_DATA_TABLE_ENTRY ModuleObject,//可找到初始化函数即驱动入口DriverEntry
   IN PUNICODE_STRING ServiceName,
   IN BOOLEAN FileSystemDriver,
   OUT PDRIVER_OBJECT *DriverObject)
{
   const WCHAR ServicesKeyName[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
   WCHAR NameBuffer[MAX_PATH];
   UNICODE_STRING DriverName;
   UNICODE_STRING RegistryKey;
   PDRIVER_INITIALIZE DriverEntry;
   PDRIVER_OBJECT Driver;
   NTSTATUS Status;

   DriverEntry = ModuleObject->EntryPoint;//重要
   
   //注意RegistryKey原来是在此创建的
   if (ServiceName != NULL && ServiceName->Length != 0)
   {
      RegistryKey.Length = 0;
      RegistryKey.MaximumLength = sizeof(ServicesKeyName) + ServiceName->Length;
      RegistryKey.Buffer = ExAllocatePool(PagedPool, RegistryKey.MaximumLength);
...
      RtlAppendUnicodeToString(&RegistryKey, ServicesKeyName);
      RtlAppendUnicodeStringToString(&RegistryKey, ServiceName);//比如："i8042prt"
      //比如：现在RegistryKey="HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\services\i8042prt"
   }
   else
   {
      RtlInitUnicodeString(&RegistryKey, NULL);
   }

    //创建驱动名
   if (ServiceName && ServiceName->Length > 0)
   {
      if (FileSystemDriver == TRUE)
         wcscpy(NameBuffer, FILESYSTEM_ROOT_NAME);//L"\\FileSystem\\"
      else
         wcscpy(NameBuffer, DRIVER_ROOT_NAME);//L"\\Driver\\"

      RtlInitUnicodeString(&DriverName, NameBuffer);
      DriverName.MaximumLength = sizeof(NameBuffer);

      RtlAppendUnicodeStringToString(&DriverName, ServiceName);
      //比如：现在DriverName="\FileSystem\i8042prt"
      
      DPRINT("Driver name: '%wZ'\n", &DriverName);
   }
   else
      DriverName.Length = 0;

   //驱动名似乎都是"\FileSystem\$servername"或者"\Driver\$servername"的形式
   Status = IopCreateDriver(
       DriverName.Length > 0 ? &DriverName : NULL,//"\FileSystem\i8042prt"或者"\Driver\i8042prt"
       DriverEntry,//ModuleObject->EntryPoint
       &RegistryKey,//比如："HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\services\i8042prt"
       ServiceName,//比如："i8042prt"
       ModuleObject,//输入
       &Driver);//输出
   RtlFreeUnicodeString(&RegistryKey);

   *DriverObject = Driver;
   if (!NT_SUCCESS(Status))
   {
      DPRINT("IopCreateDriver() failed (Status 0x%08lx)\n", Status);
      return Status;
   }

   MmFreeDriverInitialization((PLDR_DATA_TABLE_ENTRY)Driver->DriverSection);//用不着了
   
   /* Set the driver as initialized */
   IopReadyDeviceObjects(Driver);// Set the driver and all devices as initialized
 
   if (PnpSystemInit) IopReinitializeDrivers();

   return STATUS_SUCCESS;
}

/*
 * IopAttachFilterDriversCallback
 *
 * Internal routine used by IopAttachFilterDrivers.
 */
/*
IopLoadServiceModule得到ModuleObject
IopInitializeDriverModule得到ModuleObject
IopInitializeDevice加载AddDevice
注意：可能有多个过滤驱动，按顺序循环加载它们
*/
NTSTATUS NTAPI
IopAttachFilterDriversCallback(
   PWSTR ValueName,
   ULONG ValueType,
   PVOID ValueData,
   ULONG ValueLength,
   PVOID Context,
   PVOID EntryContext)
{
   PDEVICE_NODE DeviceNode = Context;
   UNICODE_STRING ServiceName;
   PWCHAR Filters;
   PLDR_DATA_TABLE_ENTRY ModuleObject;
   PDRIVER_OBJECT DriverObject;
   NTSTATUS Status;

   /* No filter value present */
   if (ValueType == REG_NONE)
       return STATUS_SUCCESS;

   //可能有多个过滤驱动，按顺序循环加载它们
   for (Filters = ValueData;
        ((ULONG_PTR)Filters - (ULONG_PTR)ValueData) < ValueLength &&
        *Filters != 0;
        Filters += (ServiceName.Length / sizeof(WCHAR)) + 1)
   {
      DPRINT("Filter Driver: %S (%wZ)\n", Filters, &DeviceNode->InstancePath);

      ServiceName.Buffer = Filters;
      ServiceName.MaximumLength =
      ServiceName.Length = (USHORT)wcslen(Filters) * sizeof(WCHAR);

       Status = IopGetDriverObject(&DriverObject,//输出
                                   &ServiceName,//各个过滤驱动的名字
                                   FALSE);
       if (!NT_SUCCESS(Status))
       {
           /* Load and initialize the filter driver */
           Status = IopLoadServiceModule(&ServiceName, &ModuleObject/*输出*/);//得到ModuleObject
           if (!NT_SUCCESS(Status))
               return Status;

           Status = IopInitializeDriverModule(DeviceNode, ModuleObject, &ServiceName,
                                              FALSE, &DriverObject/*输出*/);//得到DriverObject
           if (!NT_SUCCESS(Status))
               return Status;
       }

       Status = IopInitializeDevice(DeviceNode, DriverObject);//调用过滤驱动的AddDevice
       /* Remove extra reference */
       ObDereferenceObject(DriverObject);

       if (!NT_SUCCESS(Status))
           return Status;
   }

   return STATUS_SUCCESS;
}

/*
 * IopAttachFilterDrivers
 *
 * Load filter drivers for specified device node.
 *
 * Parameters
 *    Lower
 *       Set to TRUE for loading lower level filters or FALSE for upper
 *       level filters.
 */

//1. 加载实例的过滤驱动,在Enum\下面找到
//2. 加载类驱动的过滤驱动，在Control\Class下面找到
NTSTATUS FASTCALL
IopAttachFilterDrivers(
   PDEVICE_NODE DeviceNode,
   BOOLEAN Lower)
{
   RTL_QUERY_REGISTRY_TABLE QueryTable[2] = { { NULL, 0, NULL, NULL, 0, NULL, 0 }, };
   UNICODE_STRING Class;
   WCHAR ClassBuffer[40];
   UNICODE_STRING EnumRoot = RTL_CONSTANT_STRING(ENUM_ROOT);
   HANDLE EnumRootKey,InstanceKey,ClassKey;
   NTSTATUS Status;

   /* Open enumeration root key */
   Status = IopOpenRegistryKeyEx(&EnumRootKey, NULL, &EnumRoot, KEY_READ);
...
   /* Open subkey */
   Status = IopOpenRegistryKeyEx(&InstanceKey, EnumRootKey, &DeviceNode->InstancePath, KEY_READ);
...

   /*
    * First load the device filters
    */
   QueryTable[0].QueryRoutine = IopAttachFilterDriversCallback;//加载驱动的回调
   if (Lower)
     QueryTable[0].Name = L"LowerFilters";
   else
     QueryTable[0].Name = L"UpperFilters";
   QueryTable[0].Flags = 0;
   QueryTable[0].DefaultType = REG_NONE;

   Status = RtlQueryRegistryValues(
      RTL_REGISTRY_HANDLE,
      (PWSTR)InstanceKey,
      QueryTable,
      DeviceNode,
      NULL);
...

   //到这里，过滤驱动已经加载了
   //取得ClassGUID保存在Class中
   Class.Length = 0;
   Class.MaximumLength = 40 * sizeof(WCHAR);
   Class.Buffer = ClassBuffer;
   QueryTable[0].QueryRoutine = NULL;
   QueryTable[0].Name = L"ClassGUID";
   QueryTable[0].EntryContext = &Class;
   QueryTable[0].Flags = RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_DIRECT;

   Status = RtlQueryRegistryValues(
      RTL_REGISTRY_HANDLE,
      (PWSTR)InstanceKey,
      QueryTable,
      DeviceNode,
      NULL);

   /* Close handles */
   ZwClose(InstanceKey);
   ZwClose(EnumRootKey);

   /*
    * 加载class filter driver
    */
   if (NT_SUCCESS(Status))
   {
       UNICODE_STRING ControlClass = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class");

       Status = IopOpenRegistryKeyEx(&EnumRootKey, NULL, &ControlClass, KEY_READ);
       if (!NT_SUCCESS(Status))
       {
           DPRINT1("ZwOpenKey() failed with Status %08X\n", Status);
           return Status;
       }

       /* Open subkey */
       Status = IopOpenRegistryKeyEx(&ClassKey, EnumRootKey, &Class, KEY_READ);
       if (!NT_SUCCESS(Status))
       {
           /* It's okay if there's no class key */
           DPRINT1("ZwOpenKey() failed with Status %08X\n", Status);
           ZwClose(EnumRootKey);
           return STATUS_SUCCESS;
       }

      QueryTable[0].QueryRoutine = IopAttachFilterDriversCallback;
      if (Lower)
         QueryTable[0].Name = L"LowerFilters";
      else
         QueryTable[0].Name = L"UpperFilters";
      QueryTable[0].EntryContext = NULL;
      QueryTable[0].Flags = 0;
      QueryTable[0].DefaultType = REG_NONE;

      Status = RtlQueryRegistryValues(
         RTL_REGISTRY_HANDLE,
         (PWSTR)ClassKey,
         QueryTable,
         DeviceNode,
         NULL);

      /* Clean up */
      ZwClose(ClassKey);
      ZwClose(EnumRootKey);

      if (!NT_SUCCESS(Status))
      {
         DPRINT1("Failed to load class %s filters: %08X\n",
            Lower ? "lower" : "upper", Status);
         ZwClose(ClassKey);
         ZwClose(EnumRootKey);
         return Status;
      }
   }

   return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MiResolveImageReferences(IN PVOID ImageBase,
                         IN PUNICODE_STRING ImageFileDirectory,
                         IN PUNICODE_STRING NamePrefix OPTIONAL,
                         OUT PCHAR *MissingApi,
                         OUT PWCHAR *MissingDriver,
                         OUT PLOAD_IMPORTS *LoadImports);

// 调用MiResolveImageReferences返回成功为成功
// Used for images already loaded (boot drivers)
// The import resolving job and dependency loading is done by MiResolveImageReferences API.
// *ModuleObject = LdrEntry;
NTSTATUS
NTAPI
INIT_FUNCTION
LdrProcessDriverModule(PLDR_DATA_TABLE_ENTRY LdrEntry,
                       PUNICODE_STRING FileName,
                       PLDR_DATA_TABLE_ENTRY *ModuleObject) //如成功将等于LdrEntry
{
    NTSTATUS Status;
    UNICODE_STRING BaseName, BaseDirectory;
    PLOAD_IMPORTS LoadedImports = (PVOID)-2;
    PCHAR MissingApiName, Buffer;
    PWCHAR MissingDriverName;
    PVOID DriverBase = LdrEntry->DllBase;

    /* Allocate a buffer we'll use for names */
    Buffer = ExAllocatePoolWithTag(NonPagedPool, MAX_PATH, TAG_LDR_WSTR);
...

    /* Check for a separator */
    if (FileName->Buffer[0] == OBJ_NAME_PATH_SEPARATOR)
    {
        PWCHAR p;
        ULONG BaseLength;

        /* Loop the path until we get to the base name */
        p = &FileName->Buffer[FileName->Length / sizeof(WCHAR)]; //指向最后
        while (*(p - 1) != OBJ_NAME_PATH_SEPARATOR) p--; 

        /* Get the length */
        BaseLength = (ULONG)(&FileName->Buffer[FileName->Length / sizeof(WCHAR)] - p);
        BaseLength *= sizeof(WCHAR);

        /* Setup the string */
        BaseName.Length = (USHORT)BaseLength;
        BaseName.Buffer = p;
    }
    else
    {
        /* Otherwise, we already have a base name */
        BaseName.Length = FileName->Length;
        BaseName.Buffer = FileName->Buffer;
    }

    /* Setup the maximum length */
    BaseName.MaximumLength = BaseName.Length;

    /* Now compute the base directory */
    BaseDirectory = *FileName;
    BaseDirectory.Length -= BaseName.Length;
    BaseDirectory.MaximumLength = BaseDirectory.Length;

   //上面把Filename解析完了，FileName = BaseDirectory + BaseName
   
    /* Resolve imports */
    MissingApiName = Buffer; //暂时没东西
    Status = MiResolveImageReferences(DriverBase, //LdrEntry->DllBase
                                      &BaseDirectory, //上面得到的
                                      NULL,
                                      &MissingApiName,
                                      &MissingDriverName,
                                      &LoadedImports);

    /* Free the temporary buffer */
    ExFreePoolWithTag(Buffer, TAG_LDR_WSTR);

    /* Check the result of the imports resolution */
    if (!NT_SUCCESS(Status)) return Status;

    /* Return */
    *ModuleObject = LdrEntry;
    return STATUS_SUCCESS;
}

/*
 * IopInitializeBuiltinDriver
 *
 * Initialize a driver that is already loaded in memory.
 */

NTSTATUS
NTAPI
INIT_FUNCTION
IopInitializeBuiltinDriver(IN PLDR_DATA_TABLE_ENTRY BootLdrEntry)
{
    PDEVICE_NODE DeviceNode;
    PDRIVER_OBJECT DriverObject;
    NTSTATUS Status;
    PWCHAR FileNameWithoutPath;
    LPWSTR FileExtension;
    PUNICODE_STRING ModuleName = &BootLdrEntry->BaseDllName;//模块名来自于此！
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY NextEntry;
    UNICODE_STRING ServiceName;

   /*
    * Display 'Loading XXX...' message
    */
   IopDisplayLoadingMessage(ModuleName);
   InbvIndicateProgress();

   /*
    * Generate filename without path (not needed by freeldr)
    */
   FileNameWithoutPath = wcsrchr(ModuleName->Buffer, L'\\');
   if (FileNameWithoutPath == NULL)
   {
      FileNameWithoutPath = ModuleName->Buffer;
   }
   else
   {
      FileNameWithoutPath++;
   }

   /*
    * Strip the file extension from ServiceName
    */
   RtlCreateUnicodeString(&ServiceName, FileNameWithoutPath);
   FileExtension = wcsrchr(ServiceName.Buffer, '.');
   if (FileExtension != NULL)
   {
      ServiceName.Length -= (USHORT)wcslen(FileExtension) * sizeof(WCHAR);
      FileExtension[0] = 0;
   }

   /*
    * Determine the right device object
    */
   /* Use IopRootDeviceNode for now */
   Status = IopCreateDeviceNode(IopRootDeviceNode, NULL, &ServiceName/*上面构造的*/, &DeviceNode/*输出*/);
...

   /* Lookup the new Ldr entry in PsLoadedModuleList */
   //验证一下名称为ModuleName的LdrEntry存在于全局PsLoadedModuleList中
   NextEntry = PsLoadedModuleList.Flink;
   while (NextEntry != &PsLoadedModuleList)
   {
      LdrEntry = CONTAINING_RECORD(NextEntry,
                                   LDR_DATA_TABLE_ENTRY,
                                   InLoadOrderLinks);
      if (RtlEqualUnicodeString(ModuleName, &LdrEntry->BaseDllName, TRUE))
      {
            break;
      }

      NextEntry = NextEntry->Flink;
   }
   NT_ASSERT(NextEntry != &PsLoadedModuleList);

   /*
    * Initialize the driver
    */
   Status = IopInitializeDriverModule(DeviceNode, LdrEntry,
      &DeviceNode->ServiceName, FALSE, &DriverObject);//创建DriverObject
...

   Status = IopInitializeDevice(DeviceNode, DriverObject);//如果传统驱动，将不会做什么，只是设置标志DNF_ADDED + DNF_STARTED
   if (NT_SUCCESS(Status))
   {
      Status = IopStartDevice(DeviceNode);//启动设备：指定资源，发送IRP_MN_START_DEVICE
   }

   /* Remove extra reference from IopInitializeDriverModule */
   ObDereferenceObject(DriverObject);

   return Status;
}

/*
 * IopInitializeBootDrivers
 *
 * Initialize boot drivers and free memory for boot files.
 *
 * Parameters
 *    None
 *
 * Return Value
 *    None
 */
//实质构造全局变量IopGroupTable并加载那些驱动
//
VOID
FASTCALL
INIT_FUNCTION
IopInitializeBootDrivers(VOID)
{
    PLIST_ENTRY ListHead, NextEntry, NextEntry2;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PDEVICE_NODE DeviceNode;
    PDRIVER_OBJECT DriverObject;
    LDR_DATA_TABLE_ENTRY ModuleObject;
    NTSTATUS Status;
    UNICODE_STRING DriverName;
    ULONG i, Index;
    PDRIVER_INFORMATION DriverInfo, DriverInfoTag;
    HANDLE KeyHandle;
    PBOOT_DRIVER_LIST_ENTRY BootEntry;
    DPRINT("IopInitializeBootDrivers()\n");

    /* Use IopRootDeviceNode for now */
    Status = IopCreateDeviceNode(IopRootDeviceNode, NULL, NULL, &DeviceNode);
    if (!NT_SUCCESS(Status)) return;

    /* Setup the module object for the RAW FS Driver */
    ModuleObject.DllBase = NULL;
    ModuleObject.SizeOfImage = 0;
    ModuleObject.EntryPoint = RawFsDriverEntry;
    RtlInitUnicodeString(&DriverName, L"RAW");

    /* Initialize it */
    Status = IopInitializeDriverModule(DeviceNode,
                                       &ModuleObject,
                                       &DriverName,
                                       TRUE,
                                       &DriverObject);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        IopFreeDeviceNode(DeviceNode);
        return;
    }

    /* Now initialize the associated device */
    Status = IopInitializeDevice(DeviceNode, DriverObject);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        IopFreeDeviceNode(DeviceNode);
        ObDereferenceObject(DriverObject);
        return;
    }

    /* Start it up */
    Status = IopStartDevice(DeviceNode);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        IopFreeDeviceNode(DeviceNode);
        ObDereferenceObject(DriverObject);
        return;
    }

    /* Get highest group order index */
    IopGroupIndex = PpInitGetGroupOrderIndex(NULL);//全局变量PiInitGroupOrderTableCount + 1
    if (IopGroupIndex == 0xFFFF) ASSERT(FALSE);

    /* Allocate the group table */
    //注意IopGroupTable为全局变量
    IopGroupTable = ExAllocatePoolWithTag(PagedPool,
                                          IopGroupIndex * sizeof(LIST_ENTRY),//每一项都是一个独立的子串
                                          TAG_IO);
...

    /* Initialize the group table lists */
    for (i = 0; i < IopGroupIndex; i++) InitializeListHead(&IopGroupTable[i]);

    /* 遍历1：Loop the boot modules——LoadOrderListHead */
    ListHead = &KeLoaderBlock->LoadOrderListHead;//全局变量KeLoaderBlock
    NextEntry = ListHead->Flink;
    while (ListHead != NextEntry)
    {
        /* Get the entry */
        LdrEntry = CONTAINING_RECORD(NextEntry, //取下一个完整的LDR_DATA_TABLE_ENTRY结构
                                     LDR_DATA_TABLE_ENTRY,
                                     InLoadOrderLinks);

        /* Check if the DLL needs to be initialized */
        if (LdrEntry->Flags & LDRP_DRIVER_DEPENDENT_DLL)
        {
            /* Call its entrypoint */
            MmCallDllInitialize(LdrEntry, NULL);
        }

        /* Go to the next driver */
        NextEntry = NextEntry->Flink;
    }

    /* 遍历2：Loop the boot drivers——BootDriverListHead 
    把BootDriverListHead里面的BOOT_DRIVER_LIST_ENTRY一个一个地抽出来，换成DRIVER_INFORMATION
    结构，然后插入到全局变量IopGroupTable的某个节点代表的子串上面
    */
    ListHead = &KeLoaderBlock->BootDriverListHead;//全局变量KeLoaderBlock
    NextEntry = ListHead->Flink;
    while (ListHead != NextEntry)
    {
        /* Get the entry */
        BootEntry = CONTAINING_RECORD(NextEntry,
                                      BOOT_DRIVER_LIST_ENTRY,//看下面结构字段
                                      Link);
/*
typedef struct _BOOT_DRIVER_LIST_ENTRY
{
    LIST_ENTRY Link;
    UNICODE_STRING FilePath;
    UNICODE_STRING RegistryPath;//已经存在
    struct _LDR_DATA_TABLE_ENTRY *LdrEntry;
} BOOT_DRIVER_LIST_ENTRY, *PBOOT_DRIVER_LIST_ENTRY;
*/

        /* Get the driver loader entry */
        LdrEntry = BootEntry->LdrEntry;
       
        //下面把LdrEntry改变为更易处理的DRIVER_INFORMATION
 /*
// Boot Driver List Entry
typedef struct _DRIVER_INFORMATION
{
    LIST_ENTRY Link; //被链到全局变量IopGroupTable的某个节点（子串）
    PDRIVER_OBJECT DriverObject;//=0
    PBOOT_DRIVER_LIST_ENTRY DataTableEntry; //KeLoaderBlock->BootDriverListHead所串起来的
    HANDLE ServiceHandle;//打开以后赋值,是HKEY_LOCAL_MACHINE\SYSTEM\ControlSetXXX\services下面的那些key
    USHORT TagPosition;//通过PipGetDriverTagPriority(ServiceHandle)获得
    ULONG Failed;//=0
    ULONG Processed;//=0
    NTSTATUS Status;//=0
} DRIVER_INFORMATION, *PDRIVER_INFORMATION;
*/
        /* Allocate our internal accounting structure */
        DriverInfo = ExAllocatePoolWithTag(PagedPool,
                                           sizeof(DRIVER_INFORMATION),
                                           TAG_IO);
        if (DriverInfo)
        {
            /* Zero it and initialize it */
            RtlZeroMemory(DriverInfo, sizeof(DRIVER_INFORMATION));
            InitializeListHead(&DriverInfo->Link);
            DriverInfo->DataTableEntry = BootEntry;

            /* Open the registry key */
            Status = IopOpenRegistryKeyEx(&KeyHandle,
                                          NULL,
                                          &BootEntry->RegistryPath,
                                          KEY_READ);
            if ((NT_SUCCESS(Status)) || /* ReactOS HACK for SETUPLDR */
                ((KeLoaderBlock->SetupLdrBlock) && ((KeyHandle = (PVOID)1)))) // yes, it's an assignment!
            {
                /* Save the handle */
                DriverInfo->ServiceHandle = KeyHandle;//ServiceHandle是...\ControlSetXXX\services下面的那些key

                /* Get the group oder index */
                Index = PpInitGetGroupOrderIndex(KeyHandle);//找出这个驱动在grouporderlist中的索引(基于0)
                /* Get the tag position */
                DriverInfo->TagPosition = PipGetDriverTagPriority(KeyHandle);

                /* Insert it into the list, at the right place */
                ASSERT(Index < IopGroupIndex);
                NextEntry2 = IopGroupTable[Index].Flink;
                while (NextEntry2 != &IopGroupTable[Index])
                {
                    /* Get the driver info */
                    DriverInfoTag = CONTAINING_RECORD(NextEntry2,
                                                      DRIVER_INFORMATION,
                                                      Link);

                    /* Check if we found the right tag position */
                    if (DriverInfoTag->TagPosition > DriverInfo->TagPosition)
                    {
                        /* We're done */
                        break;
                    }

                    /* Next entry */
                    NextEntry2 = NextEntry2->Flink;
                }

                /* Insert us right before the next entry */
                NextEntry2 = NextEntry2->Blink;
                InsertHeadList(NextEntry2, &DriverInfo->Link);
            }
        }

        /* Go to the next driver */
        NextEntry = NextEntry->Flink;
    }

    /* 循环3：实质操作！装载那些驱动，Loop each group index */
    for (i = 0; i < IopGroupIndex; i++)
    {
        /* Loop each group table */
        NextEntry = IopGroupTable[i].Flink;
        while (NextEntry != &IopGroupTable[i])
        {
            /* Get the entry */
            DriverInfo = CONTAINING_RECORD(NextEntry,
                                           DRIVER_INFORMATION,
                                           Link);

            /* Get the driver loader entry */
            LdrEntry = DriverInfo->DataTableEntry->LdrEntry;

            /* Initialize it */
           //================================================
            IopInitializeBuiltinDriver(LdrEntry);//实质操作！！
           //================================================
           
            /* Next entry */
            NextEntry = NextEntry->Flink;
        }
    }

    /* In old ROS, the loader list became empty after this point. Simulate. */
    InitializeListHead(&KeLoaderBlock->LoadOrderListHead);
}

//调用CmGetSystemDriverList得到驱动表DriverList，然后顺序地调用ZwLoadDriver装载驱动
VOID
FASTCALL
INIT_FUNCTION
IopInitializeSystemDrivers(VOID)
{
    PUNICODE_STRING *DriverList, *SavedList;

    /* No system drivers on the boot cd */
    if (KeLoaderBlock->SetupLdrBlock) return;

    /* Get the driver list */
    SavedList = DriverList = CmGetSystemDriverList();
    ASSERT(DriverList);

    /* Loop it */
    while (*DriverList)
    {
        /* Load the driver */
        ZwLoadDriver(*DriverList);

        /* Free the entry */
        RtlFreeUnicodeString(*DriverList);
        ExFreePool(*DriverList);

        /* Next entry */
        InbvIndicateProgress();
        DriverList++;
    }

    /* Free the list */
    ExFreePool(SavedList);
}

/*
 * IopUnloadDriver
 *
 * Unloads a device driver.
 *
 * Parameters
 *    DriverServiceName
 *       Name of the service to unload (registry key).
 *
 *    UnloadPnpDrivers
 *       Whether to unload Plug & Plug or only legacy drivers. If this
 *       parameter is set to FALSE, the routine will unload only legacy
 *       drivers.
 *
 * Return Value
 *    Status
 *
 * To do
 *    Guard the whole function by SEH.
 */
//unload的条件：DeviceObject->ReferenceCount==0，DeviceObject->AttachedDevice=NULL
NTSTATUS NTAPI
IopUnloadDriver(PUNICODE_STRING DriverServiceName,  //注册表键：\Registry\Machine\System\CurrentControlSet\Services\XXXX
                BOOLEAN UnloadPnpDrivers)
{
   RTL_QUERY_REGISTRY_TABLE QueryTable[2];
   UNICODE_STRING ImagePath;
   UNICODE_STRING ServiceName;
   UNICODE_STRING ObjectName;
   PDRIVER_OBJECT DriverObject;
   PDEVICE_OBJECT DeviceObject;
   PEXTENDED_DEVOBJ_EXTENSION DeviceExtension;
   LOAD_UNLOAD_PARAMS LoadParams;
   NTSTATUS Status;
   LPWSTR Start;
   BOOLEAN SafeToUnload = TRUE;

   DPRINT("IopUnloadDriver('%wZ', %u)\n", DriverServiceName, UnloadPnpDrivers);

   PAGED_CODE();

   /*
    * Get the service name from the registry key name
    */

   Start = wcsrchr(DriverServiceName->Buffer, L'\\');
   if (Start == NULL)
      Start = DriverServiceName->Buffer;
   else
      Start++;

   RtlInitUnicodeString(&ServiceName, Start);

   /*
    * Construct the driver object name
    */

   ObjectName.Length = ((USHORT)wcslen(Start) + 8) * sizeof(WCHAR);
   ObjectName.MaximumLength = ObjectName.Length + sizeof(WCHAR);
   ObjectName.Buffer = ExAllocatePool(PagedPool, ObjectName.MaximumLength);
   if (!ObjectName.Buffer) return STATUS_INSUFFICIENT_RESOURCES;
   wcscpy(ObjectName.Buffer, DRIVER_ROOT_NAME); //"\Driver\"
   memcpy(ObjectName.Buffer + 8, Start, ObjectName.Length - 8 * sizeof(WCHAR));
   ObjectName.Buffer[ObjectName.Length/sizeof(WCHAR)] = 0;

   /*
    * Find the driver object
    */
   Status = ObReferenceObjectByName(&ObjectName, //驱动名称为："\Driver\XXX"
                                    0,
                                    0,
                                    0,
                                    IoDriverObjectType,
                                    KernelMode,
                                    0,
                                    (PVOID*)&DriverObject);

   if (!NT_SUCCESS(Status))
   {
      DPRINT1("Can't locate driver object for %wZ\n", &ObjectName);
      ExFreePool(ObjectName.Buffer);
      return Status;
   }

   /*
    * Free the buffer for driver object name
    */
   ExFreePool(ObjectName.Buffer);

   /* Check that driver is not already unloading */
   if (DriverObject->Flags & DRVO_UNLOAD_INVOKED)
   {
       DPRINT1("Driver deletion pending\n");
       ObDereferenceObject(DriverObject);
       return STATUS_DELETE_PENDING;
   }

   /*
    * Get path of service...
    */

   RtlZeroMemory(QueryTable, sizeof(QueryTable));

   RtlInitUnicodeString(&ImagePath, NULL);

   QueryTable[0].Name = L"ImagePath";
   QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
   QueryTable[0].EntryContext = &ImagePath;

   Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
       DriverServiceName->Buffer, QueryTable, NULL, NULL);

   if (!NT_SUCCESS(Status))
   {
      DPRINT1("RtlQueryRegistryValues() failed (Status %x)\n", Status);
      ObDereferenceObject(DriverObject);
      return Status;
   }

   /*
    * Normalize the image path for all later processing.
    */

   Status = IopNormalizeImagePath(&ImagePath, &ServiceName);

   if (!NT_SUCCESS(Status))
   {
      DPRINT1("IopNormalizeImagePath() failed (Status %x)\n", Status);
      ObDereferenceObject(DriverObject);
      return Status;
   }

   /*
    * Free the service path
    */

   ExFreePool(ImagePath.Buffer);

   /*
    * Unload the module and release the references to the device object
    */

    /* Call the load/unload routine, depending on current process */
   if (DriverObject->DriverUnload && DriverObject->DriverSection &&
       (UnloadPnpDrivers || (DriverObject->Flags & DRVO_LEGACY_DRIVER)))
   {
      /* Loop through each device object of the driver
         and set DOE_UNLOAD_PENDING flag */
      DeviceObject = DriverObject->DeviceObject;
      while (DeviceObject)
      {
         /* Set the unload pending flag for the device */
         DeviceExtension = IoGetDevObjExtension(DeviceObject);
         DeviceExtension->ExtensionFlags |= DOE_UNLOAD_PENDING;

         /* Make sure there are no attached devices or no reference counts */
         if ((DeviceObject->ReferenceCount) || (DeviceObject->AttachedDevice))
         {
            /* Not safe to unload */
            DPRINT1("Drivers device object is referenced or has attached devices\n");

            SafeToUnload = FALSE;
         }

         DeviceObject = DeviceObject->NextDevice;
      }

      /* If not safe to unload, then return success */
      if (!SafeToUnload)
      {
         ObDereferenceObject(DriverObject);
         return STATUS_SUCCESS;
      }

      DPRINT1("Unloading driver '%wZ' (manual)\n", &DriverObject->DriverName);

      /* Set the unload invoked flag */
      DriverObject->Flags |= DRVO_UNLOAD_INVOKED;

      if (PsGetCurrentProcess() == PsInitialSystemProcess)
      {
         /* Just call right away */
         (*DriverObject->DriverUnload)(DriverObject); //调用unload回调
      }
      else
      {
         /* Load/Unload must be called from system process */

         /* Prepare parameters block */
         LoadParams.DriverObject = DriverObject;
         KeInitializeEvent(&LoadParams.Event, NotificationEvent, FALSE);

         ExInitializeWorkItem(&LoadParams.WorkItem,
             (PWORKER_THREAD_ROUTINE)IopLoadUnloadDriver,
             (PVOID)&LoadParams);

         /* Queue it */
         ExQueueWorkItem(&LoadParams.WorkItem, DelayedWorkQueue);

         /* And wait when it completes */
         KeWaitForSingleObject(&LoadParams.Event, UserRequest, KernelMode,
             FALSE, NULL);
      }

      /* Mark the driver object temporary, so it could be deleted later */
      ObMakeTemporaryObject(DriverObject);

      /* Dereference it 2 times */
      ObDereferenceObject(DriverObject);
      ObDereferenceObject(DriverObject);

      return STATUS_SUCCESS;
   }
   else
   {
      DPRINT1("No DriverUnload function! '%wZ' will not be unloaded!\n", &DriverObject->DriverName);

      /* Dereference one time (refd inside this function) */
      ObDereferenceObject(DriverObject);

      /* Return unloading failure */
      return STATUS_INVALID_DEVICE_REQUEST;
   }
}

//一一执行DriverReinitListHead所串起来的回调结构DRIVER_REINIT_ITEM
VOID
NTAPI
IopReinitializeDrivers(VOID)
{
    PDRIVER_REINIT_ITEM ReinitItem;
    PLIST_ENTRY Entry;

    /* Get the first entry and start looping */
    Entry = ExInterlockedRemoveHeadList(&DriverReinitListHead,
                                        &DriverReinitListLock);
    while (Entry)
    {
        /* Get the item*/
        ReinitItem = CONTAINING_RECORD(Entry, DRIVER_REINIT_ITEM, ItemEntry);

        /* Increment reinitialization counter */
        ReinitItem->DriverObject->DriverExtension->Count++;

        /* Remove the device object flag */
        ReinitItem->DriverObject->Flags &= ~DRVO_REINIT_REGISTERED;

        /* Call the routine */
        ReinitItem->ReinitRoutine(ReinitItem->DriverObject,
                                  ReinitItem->Context,
                                  ReinitItem->DriverObject->
                                  DriverExtension->Count);

        /* Free the entry */
        ExFreePool(Entry);

        /* Move to the next one */
        Entry = ExInterlockedRemoveHeadList(&DriverReinitListHead,
                                            &DriverReinitListLock);
    }
}


//一一执行DriverBootReinitListHead所串起来的回调结构DRIVER_REINIT_ITEM
VOID
NTAPI
IopReinitializeBootDrivers(VOID)
{
    PDRIVER_REINIT_ITEM ReinitItem;
    PLIST_ENTRY Entry;

    /* Get the first entry and start looping */
    Entry = ExInterlockedRemoveHeadList(&DriverBootReinitListHead,
                                        &DriverBootReinitListLock);
    while (Entry)
    {
        /* Get the item*/
        ReinitItem = CONTAINING_RECORD(Entry, DRIVER_REINIT_ITEM, ItemEntry);

        /* Increment reinitialization counter */
        ReinitItem->DriverObject->DriverExtension->Count++;

        /* Remove the device object flag */
        ReinitItem->DriverObject->Flags &= ~DRVO_BOOTREINIT_REGISTERED;

        /* Call the routine */
        ReinitItem->ReinitRoutine(ReinitItem->DriverObject,
                                  ReinitItem->Context,
                                  ReinitItem->DriverObject->
                                  DriverExtension->Count);

        /* Free the entry */
        ExFreePool(Entry);

        /* Move to the next one */
        Entry = ExInterlockedRemoveHeadList(&DriverBootReinitListHead,
                                            &DriverBootReinitListLock);
    }
}

/*
创建一个Object，最后变成了pDriverObject
如果没有给定驱动名称，则通过时钟生成一个名字
ModuleObject的用途是获得驱动的入口(DriverStart->DriverStart)和大小
会调用驱动的DriverEntry（就是InitializationFunction）函数，如果在这个函数中创建了deviceobject,那么是一个传统驱动
实际通过创建对象（ObCreateObject）来实现的
*/
NTSTATUS
NTAPI
IopCreateDriver(IN PUNICODE_STRING DriverName OPTIONAL,
                IN PDRIVER_INITIALIZE InitializationFunction, //初始化回调
                IN PUNICODE_STRING RegistryPath, //初始化回调的参数，传统的驱动不需要这个参数
                IN PCUNICODE_STRING ServiceName,
                PLDR_DATA_TABLE_ENTRY ModuleObject,//传统驱动不需要这个参数
                OUT PDRIVER_OBJECT *DriverStart)
{
    WCHAR NameBuffer[100];
    USHORT NameLength;
    UNICODE_STRING LocalDriverName;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    ULONG ObjectSize;
    PDRIVER_OBJECT DriverObject;
    UNICODE_STRING ServiceKeyName;
    HANDLE hDriver;
    ULONG i, RetryCount = 0;

try_again:
    /* First, create a unique name for the driver if we don't have one */
    if (!DriverName)
    {
        /* Create a random name and set up the string*/
        NameLength = (USHORT)swprintf(NameBuffer,
                                      DRIVER_ROOT_NAME L"%08u",
                                      KeTickCount);
        LocalDriverName.Length = NameLength * sizeof(WCHAR);
        LocalDriverName.MaximumLength = LocalDriverName.Length + sizeof(UNICODE_NULL);
        LocalDriverName.Buffer = NameBuffer;
    }
    else
    {
        /* So we can avoid another code path, use a local var */
        LocalDriverName = *DriverName;
    }
   
   //无论如何我们有了一个驱动的名字了
   
    /* Initialize the Attributes */
    ObjectSize = sizeof(DRIVER_OBJECT) + sizeof(EXTENDED_DRIVER_EXTENSION);
    InitializeObjectAttributes(&ObjectAttributes,
                               &LocalDriverName,
                               OBJ_PERMANENT | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    /* Create the Object */
    Status = ObCreateObject(KernelMode,
                            IoDriverObjectType,//枚举
                            &ObjectAttributes,
                            KernelMode,
                            NULL,
                            ObjectSize,//驱动对象和驱动扩展两者加起来的长度
                            0,
                            0,
                            (PVOID*)&DriverObject);//输出
    if (!NT_SUCCESS(Status)) return Status;

    DPRINT("IopCreateDriver(): created DO %p\n", DriverObject);

    /* Set up the Object */
    RtlZeroMemory(DriverObject, ObjectSize);
    DriverObject->Type = IO_TYPE_DRIVER;//IO性质的驱动
    DriverObject->Size = sizeof(DRIVER_OBJECT);
    DriverObject->Flags = DRVO_LEGACY_DRIVER; //先指定为传统驱动再说，到底是不是，下面会修正
    DriverObject->DriverExtension = (PDRIVER_EXTENSION)(DriverObject + 1);//PDRIVER_EXTENSION紧跟在DriverObject后面
    DriverObject->DriverExtension->DriverObject = DriverObject;
    DriverObject->DriverInit = InitializationFunction;//DriverEntry函数
    DriverObject->DriverSection = ModuleObject;//什么意思？
    /* Loop all Major Functions */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        /* Invalidate each function */
        DriverObject->MajorFunction[i] = IopInvalidDeviceRequest;//stub
    }

    /* Set up the service key name buffer */
    ServiceKeyName.MaximumLength = ServiceName->Length + sizeof(UNICODE_NULL);
    ServiceKeyName.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                  ServiceKeyName.MaximumLength,
                                                  TAG_IO);
    if (!ServiceKeyName.Buffer)
    {
        /* Fail */
        ObMakeTemporaryObject(DriverObject);
        ObDereferenceObject(DriverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Copy the name and set it in the driver extension */
    RtlCopyUnicodeString(&ServiceKeyName,
                         ServiceName);
    DriverObject->DriverExtension->ServiceKeyName = ServiceKeyName;

    /* Make a copy of the driver name to store in the driver object */
    DriverObject->DriverName.MaximumLength = LocalDriverName.Length;
    DriverObject->DriverName.Buffer = ExAllocatePoolWithTag(PagedPool,
                                                            DriverObject->DriverName.MaximumLength,
                                                            TAG_IO);
    if (!DriverObject->DriverName.Buffer)
    {
        /* Fail */
        ObMakeTemporaryObject(DriverObject);
        ObDereferenceObject(DriverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&DriverObject->DriverName,
                         &LocalDriverName);

    /* Add the Object and get its handle */
    Status = ObInsertObject(DriverObject,
                            NULL,
                            FILE_READ_DATA,
                            0,
                            NULL,
                            &hDriver);//输出，获得DriverObject的句柄
   
    /* Eliminate small possibility when this function is called more than
       once in a row, and KeTickCount doesn't get enough time to change */
    if (!DriverName && (Status == STATUS_OBJECT_NAME_COLLISION) && (RetryCount < 100))
    {
        RetryCount++;
        goto try_again;
    }

    if (!NT_SUCCESS(Status)) return Status;

    /* Now reference it */
    Status = ObReferenceObjectByHandle(hDriver,//上面刚刚获得
                                       0,
                                       IoDriverObjectType,//指明为驱动对象
                                       KernelMode,
                                       (PVOID*)&DriverObject,
                                       NULL);

    /* Close the extra handle */
    ZwClose(hDriver);

    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        ObMakeTemporaryObject(DriverObject);
        ObDereferenceObject(DriverObject);
        return Status;
    }

    DriverObject->HardwareDatabase = &IopHardwareDatabaseKey; //\REGISTRY\MACHINE\HARDWARE\DESCRIPTION\SYSTEM
    DriverObject->DriverStart = ModuleObject ? ModuleObject->DllBase : 0;
    DriverObject->DriverSize = ModuleObject ? ModuleObject->SizeOfImage : 0;

    /* Finally, call its init function */
    DPRINT("RegistryKey: %wZ\n", RegistryPath);
    DPRINT("Calling driver entrypoint at %p\n", InitializationFunction);
    Status = (*InitializationFunction)(DriverObject, RegistryPath);//调用初始化回调函数
    if (!NT_SUCCESS(Status))
    {
        /* If it didn't work, then kill the object */
        DPRINT1("'%wZ' initialization failed, status (0x%08lx)\n", DriverName, Status);
        DriverObject->DriverSection = NULL;
        ObMakeTemporaryObject(DriverObject);
        ObDereferenceObject(DriverObject);
        return Status;
    }
    else
    {
        /* Returns to caller the object */
        *pDriverObject = DriverObject;
    }

    /* We're going to say if we don't have any DOs from DriverEntry, then we're not legacy.
     * Other parts of the I/O manager depend on this behavior */
    if (!DriverObject->DeviceObject) DriverObject->Flags &= ~DRVO_LEGACY_DRIVER;

    /* Loop all Major Functions */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        /*
         * Make sure the driver didn't set any dispatch entry point to NULL!
         * Doing so is illegal; drivers shouldn't touch entry points they
         * do not implement.
         */

        /* Check if it did so anyway */
        if (!DriverObject->MajorFunction[i])
        {
            /* Print a warning in the debug log */
            DPRINT1("Driver <%wZ> set DriverObject->MajorFunction[%lu] to NULL!\n",
                    &DriverObject->DriverName, i);

            /* Fix it up */
            DriverObject->MajorFunction[i] = IopInvalidDeviceRequest;//stub
        }
    }

    /* Return the Status */
    return Status;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
//传统驱动才能调用这个函数，因为没有RegistryPath和ModuleObject参数
NTSTATUS
NTAPI
IoCreateDriver(IN PUNICODE_STRING DriverName OPTIONAL,
               IN PDRIVER_INITIALIZE InitializationFunction)
{
   PDRIVER_OBJECT DriverObject;
   return IopCreateDriver(DriverName, InitializationFunction, NULL/*RegistryPath*/, DriverName, NULL/*ModuleObject*/, &DriverObject);
}

/*
 * @implemented
 */
VOID
NTAPI
IoDeleteDriver(IN PDRIVER_OBJECT DriverObject)
{
    /* Simply dereference the Object */
    ObDereferenceObject(DriverObject);
}

/*
 * @implemented
 */
//分配DRIVER_REINIT_ITEM结构串在DriverBootReinitListHead为头的串串中
VOID
NTAPI
IoRegisterBootDriverReinitialization(IN PDRIVER_OBJECT DriverObject,
                                     IN PDRIVER_REINITIALIZE ReinitRoutine,
                                     IN PVOID Context)
{
    PDRIVER_REINIT_ITEM ReinitItem;

    /* Allocate the entry */
    ReinitItem = ExAllocatePoolWithTag(NonPagedPool,
                                       sizeof(DRIVER_REINIT_ITEM),
                                       TAG_REINIT);
    if (!ReinitItem) return;

    /* Fill it out */
    ReinitItem->DriverObject = DriverObject;
    ReinitItem->ReinitRoutine = ReinitRoutine;
    ReinitItem->Context = Context;

    /* Set the Driver Object flag and insert the entry into the list */
    DriverObject->Flags |= DRVO_BOOTREINIT_REGISTERED;
    ExInterlockedInsertTailList(&DriverBootReinitListHead,
                                &ReinitItem->ItemEntry,
                                &DriverBootReinitListLock);
}

/*
 * @implemented
 */
//分配DRIVER_REINIT_ITEM结构串在DriverReinitListHead为头的串串中
VOID
NTAPI
IoRegisterDriverReinitialization(IN PDRIVER_OBJECT DriverObject,
                                 IN PDRIVER_REINITIALIZE ReinitRoutine,
                                 IN PVOID Context)
{
    PDRIVER_REINIT_ITEM ReinitItem;

    /* Allocate the entry */
    ReinitItem = ExAllocatePoolWithTag(NonPagedPool,
                                       sizeof(DRIVER_REINIT_ITEM),
                                       TAG_REINIT);
    if (!ReinitItem) return;

    /* Fill it out */
    ReinitItem->DriverObject = DriverObject;
    ReinitItem->ReinitRoutine = ReinitRoutine;
    ReinitItem->Context = Context;

    /* Set the Driver Object flag and insert the entry into the list */
    DriverObject->Flags |= DRVO_REINIT_REGISTERED;
    ExInterlockedInsertTailList(&DriverReinitListHead,
                                &ReinitItem->ItemEntry,
                                &DriverReinitListLock);
}

/*
 * 创建驱动对象的扩展，因为每个驱动的要求的驱动扩展是不一样大小的，所以要指定其大小
 * 从流程看，一个驱动对象可以有好几个驱动扩展（分饰不同角色？），但通常一个是足够的吧
 */
NTSTATUS
NTAPI
IoAllocateDriverObjectExtension(IN PDRIVER_OBJECT DriverObject,
                                IN PVOID ClientIdentificationAddress,//DriverObjectExtension的识别码,通常为DriverObject地址
                                IN ULONG DriverObjectExtensionSize,
                                OUT PVOID *DriverObjectExtension)
{
    KIRQL OldIrql;
    PIO_CLIENT_EXTENSION DriverExtensions, NewDriverExtension;
    BOOLEAN Inserted = FALSE;

    /* Assume failure */
    *DriverObjectExtension = NULL;

    /* Allocate the extension */
    NewDriverExtension = ExAllocatePoolWithTag(NonPagedPool,
                                               sizeof(IO_CLIENT_EXTENSION) +
                                               DriverObjectExtensionSize,
                                               TAG_DRIVER_EXTENSION);
    if (!NewDriverExtension) return STATUS_INSUFFICIENT_RESOURCES;

    /* Clear the extension for teh caller */
    RtlZeroMemory(NewDriverExtension,
                  sizeof(IO_CLIENT_EXTENSION) + DriverObjectExtensionSize);

    /* Acqure lock */
    OldIrql = KeRaiseIrqlToDpcLevel();

    /* Fill out the extension */
    NewDriverExtension->ClientIdentificationAddress = ClientIdentificationAddress; //识别码
   
    /* Loop the current extensions */
    DriverExtensions = IoGetDrvObjExtension(DriverObject)->
                       ClientDriverExtension;
    while (DriverExtensions)
    {
        /* Check if the identifier matches */
        if (DriverExtensions->ClientIdentificationAddress ==
            ClientIdentificationAddress)
        {
            /* We have a collision, break out */
            break;
        }

        /* Go to the next one */
        DriverExtensions = DriverExtensions->NextExtension;
    }

    /* Check if we didn't collide */
    if (!DriverExtensions)
    {
        /* Link this one in */
        NewDriverExtension->NextExtension =
            IoGetDrvObjExtension(DriverObject)->ClientDriverExtension;
        IoGetDrvObjExtension(DriverObject)->ClientDriverExtension =
            NewDriverExtension;
        Inserted = TRUE;
    }

    /* Release the lock */
    KeLowerIrql(OldIrql);

    /* Check if insertion failed */
    if (!Inserted)
    {
        /* Free the entry and fail */
        ExFreePoolWithTag(NewDriverExtension, TAG_DRIVER_EXTENSION);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    /* Otherwise, return the pointer */
    *DriverObjectExtension = NewDriverExtension + 1;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
PVOID
NTAPI
IoGetDriverObjectExtension(IN PDRIVER_OBJECT DriverObject,
                           IN PVOID ClientIdentificationAddress)
{
    KIRQL OldIrql;
    PIO_CLIENT_EXTENSION DriverExtensions;

    /* Acquire lock */
    OldIrql = KeRaiseIrqlToDpcLevel();

    /* Loop the list until we find the right one */
    DriverExtensions = IoGetDrvObjExtension(DriverObject)->ClientDriverExtension;
    while (DriverExtensions)
    {
        /* Check for a match */
        if (DriverExtensions->ClientIdentificationAddress ==
            ClientIdentificationAddress)
        {
            /* Break out */
            break;
        }

        /* Keep looping */
        DriverExtensions = DriverExtensions->NextExtension;
    }

    /* Release lock */
    KeLowerIrql(OldIrql);

    /* Return nothing or the extension */
    if (!DriverExtensions) return NULL;
    return DriverExtensions + 1;
}

//既可以装载驱动，也可以卸载驱动
// LoadParams->DriverObject存在当然卸载驱动（调用DriverUnload）了，否则装载驱动
// 从注册表找到驱动程序在哪儿，然后装载之
//1.MmLoadSystemImage得到ModuleObject
//2.IopCreateDeviceNode得到DeviceNode
//3.IopInitializeDriverModule得到DriverObject，会创建RegistryKey
//4.IopInitializeDevice初始化（AddDevice和attach）和启动设备
VOID NTAPI
IopLoadUnloadDriver(PLOAD_UNLOAD_PARAMS LoadParams)
{
    RTL_QUERY_REGISTRY_TABLE QueryTable[3];
    UNICODE_STRING ImagePath;
    UNICODE_STRING ServiceName;
    NTSTATUS Status;
    ULONG Type;
    PDEVICE_NODE DeviceNode;
    PDRIVER_OBJECT DriverObject;
    PLDR_DATA_TABLE_ENTRY ModuleObject;
    PVOID BaseAddress;
    WCHAR *cur;

    /* Check if it's an unload request */
    if (LoadParams->DriverObject)
    {
        (*LoadParams->DriverObject->DriverUnload)(LoadParams->DriverObject);

        /* Return success and signal the event */
        LoadParams->Status = STATUS_SUCCESS;
        KeSetEvent(&LoadParams->Event, 0, FALSE);//需要释放信号
        return;
    }

    RtlInitUnicodeString(&ImagePath, NULL);

    /*
     * Get the service name from the registry key name.
     */
    ASSERT(LoadParams->ServiceName->Length >= sizeof(WCHAR));

    ServiceName = *LoadParams->ServiceName;
    cur = LoadParams->ServiceName->Buffer +
         (LoadParams->ServiceName->Length / sizeof(WCHAR)) - 1;
    while (LoadParams->ServiceName->Buffer != cur)
    {
        if (*cur == L'\\')
        {
            ServiceName.Buffer = cur + 1;
            ServiceName.Length = LoadParams->ServiceName->Length -
                                 (USHORT)((ULONG_PTR)ServiceName.Buffer -
                                          (ULONG_PTR)LoadParams->ServiceName->Buffer);
            break;
        }
        cur--;
    }

    /*
    * Get service type.
    */

    RtlZeroMemory(&QueryTable, sizeof(QueryTable));

    RtlInitUnicodeString(&ImagePath, NULL);

    QueryTable[0].Name = L"Type";
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;
    QueryTable[0].EntryContext = &Type;

    QueryTable[1].Name = L"ImagePath";
    QueryTable[1].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[1].EntryContext = &ImagePath;

    Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                    LoadParams->ServiceName->Buffer,
                                    QueryTable, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("RtlQueryRegistryValues() failed (Status %lx)\n", Status);
        if (ImagePath.Buffer) ExFreePool(ImagePath.Buffer);
        LoadParams->Status = Status;
        KeSetEvent(&LoadParams->Event, 0, FALSE);
        return;
    }

    /*
    * Normalize the image path for all later processing.
    */

    Status = IopNormalizeImagePath(&ImagePath, &ServiceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("IopNormalizeImagePath() failed (Status %x)\n", Status);
        LoadParams->Status = Status;
        KeSetEvent(&LoadParams->Event, 0, FALSE);
        return;
    }

    DPRINT("FullImagePath: '%wZ'\n", &ImagePath);
    DPRINT("Type: %lx\n", Type);

    /*
     * Get existing DriverObject pointer (in case the driver
     * has already been loaded and initialized).
     */
    Status = IopGetDriverObject(&DriverObject,
                                &ServiceName,
                                (Type == 2 /* SERVICE_FILE_SYSTEM_DRIVER */ ||
                                 Type == 8 /* SERVICE_RECOGNIZER_DRIVER */));

    if (!NT_SUCCESS(Status))
    {
        /*
         * Load the driver module
         */

        DPRINT("Loading module from %wZ\n", &ImagePath);
        Status = MmLoadSystemImage(&ImagePath, NULL, NULL, 0, (PVOID)&ModuleObject, &BaseAddress);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("MmLoadSystemImage() failed (Status %lx)\n", Status);
            LoadParams->Status = Status;
            KeSetEvent(&LoadParams->Event, 0, FALSE);
            return;
        }

        /*
         * Initialize the driver module if it's loaded for the first time
         */
        Status = IopCreateDeviceNode(IopRootDeviceNode, NULL, &ServiceName, &DeviceNode);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("IopCreateDeviceNode() failed (Status %lx)\n", Status);
            MmUnloadSystemImage(ModuleObject);
            LoadParams->Status = Status;
            KeSetEvent(&LoadParams->Event, 0, FALSE);
            return;
        }

        IopDisplayLoadingMessage(&DeviceNode->ServiceName);

        Status = IopInitializeDriverModule(DeviceNode,
                                           ModuleObject,
                                           &DeviceNode->ServiceName,
                                           (Type == 2 /* SERVICE_FILE_SYSTEM_DRIVER */ ||
                                            Type == 8 /* SERVICE_RECOGNIZER_DRIVER */),
                                           &DriverObject);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("IopInitializeDriverModule() failed (Status %lx)\n", Status);
            MmUnloadSystemImage(ModuleObject);
            IopFreeDeviceNode(DeviceNode);
            LoadParams->Status = Status;
            KeSetEvent(&LoadParams->Event, 0, FALSE);
            return;
        }

        /* Initialize and start device */
        IopInitializeDevice(DeviceNode, DriverObject);
        Status = IopStartDevice(DeviceNode);
    }
    else
    {
        DPRINT("DriverObject already exist in ObjectManager\n");
        Status = STATUS_IMAGE_ALREADY_LOADED;

        /* IopGetDriverObject references the DriverObject, so dereference it */
        ObDereferenceObject(DriverObject);
    }

    /* Pass status to the caller and signal the event */
    LoadParams->Status = Status;
    KeSetEvent(&LoadParams->Event, 0, FALSE);
}

/*
 * NtLoadDriver
 *
 * Loads a device driver.
 *
 * Parameters
 *    DriverServiceName
 *       Name of the service to load (registry key).
 *
 * Return Value
 *    Status
 *
 * Status
 *    implemented
 */
NTSTATUS NTAPI
NtLoadDriver(IN PUNICODE_STRING DriverServiceName)
{
    UNICODE_STRING CapturedDriverServiceName = { 0, 0, NULL };
    KPROCESSOR_MODE PreviousMode;
    LOAD_UNLOAD_PARAMS LoadParams;
    NTSTATUS Status;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    /*
     * Check security privileges
     */

    /* FIXME: Uncomment when privileges will be correctly implemented. */
#if 0
    if (!SeSinglePrivilegeCheck(SeLoadDriverPrivilege, PreviousMode))
    {
        DPRINT("Privilege not held\n");
        return STATUS_PRIVILEGE_NOT_HELD;
    }
#endif

    Status = ProbeAndCaptureUnicodeString(&CapturedDriverServiceName,
                                          PreviousMode,
                                          DriverServiceName);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    DPRINT("NtLoadDriver('%wZ')\n", &CapturedDriverServiceName);

    LoadParams.ServiceName = &CapturedDriverServiceName;
    LoadParams.DriverObject = NULL;
    KeInitializeEvent(&LoadParams.Event, NotificationEvent, FALSE);

    /* Call the load/unload routine, depending on current process */
    if (PsGetCurrentProcess() == PsInitialSystemProcess)
    {
        /* Just call right away */
        IopLoadUnloadDriver(&LoadParams);
    }
    else
    {
        /* Load/Unload must be called from system process */
        ExInitializeWorkItem(&LoadParams.WorkItem,
                             (PWORKER_THREAD_ROUTINE)IopLoadUnloadDriver,
                             (PVOID)&LoadParams);

        /* Queue it */
        ExQueueWorkItem(&LoadParams.WorkItem, DelayedWorkQueue);

        /* And wait when it completes */
        KeWaitForSingleObject(&LoadParams.Event, UserRequest, KernelMode,
            FALSE, NULL);
    }

    ReleaseCapturedUnicodeString(&CapturedDriverServiceName,
                                 PreviousMode);

    return LoadParams.Status;
}

/*
 * NtUnloadDriver
 *
 * Unloads a legacy device driver.
 *
 * Parameters
 *    DriverServiceName
 *       Name of the service to unload (registry key).
 *
 * Return Value
 *    Status
 *
 * Status
 *    implemented
 */

NTSTATUS NTAPI
NtUnloadDriver(IN PUNICODE_STRING DriverServiceName)
{
   return IopUnloadDriver(DriverServiceName, FALSE);
}

/* EOF */
