
#ifndef _HIDCLASS_PCH_
#define _HIDCLASS_PCH_

#define _HIDPI_NO_FUNCTION_MACROS_
#include <wdm.h>
#include <hidpddi.h>
#include <stdio.h>
#include <hidport.h>

#define HIDCLASS_TAG 'CdiH'
/*
关于NextDeviceObject：
(1)minidriver FDO对应的设备对象之下层设备对象
(2)把本层minidriver不处理的irp传递到下层去处理
(3)minidriver的drvobj的派遣函数被HIDClass代替了
typedef struct _HID_DEVICE_EXTENSION { 
	PDEVICE_OBJECT PhysicalDeviceObject; 
	PDEVICE_OBJECT NextDeviceObject; 
	PVOID MiniDeviceExtension; 
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;
The HID_DEVICE_EXTENSION structure is used by a HID minidriver as its layout
for the device extension of a HIDClass device's functional device object.

*/
typedef struct //这是所有HID设备都遵循的驱动扩展，for minidriver，鸠占鹊巢，this is magic!
{
    PDRIVER_OBJECT DriverObject;//将指向minidriver的drvobj，比如hidusb的drvobj
    ULONG DeviceExtensionSize;//由于hidcalss的HidClassAddDevice代为创建fdo或者pdo的设备扩展，所以需要知道minidriver的设备扩展大小
    BOOLEAN DevicesArePolled;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    PDRIVER_ADD_DEVICE AddDevice;
    PDRIVER_UNLOAD DriverUnload;
    KSPIN_LOCK Lock;
} HIDCLASS_DRIVER_EXTENSION, *PHIDCLASS_DRIVER_EXTENSION;

typedef struct
{
    BOOLEAN IsFDO;
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;//通过IoGetDriverObjectExtension得到，是minidriver的驱动扩展
    HID_DEVICE_EXTENSION HidDeviceExtension;//在AddDevice中创建设备对象以后被设置
	
    //以下二行在fdo的StartDevcie中通过irp包查询到,为了方便使用，放在这里
    HIDP_DEVICE_DESC DeviceDescription;// device description，
    HID_DEVICE_ATTRIBUTES Attributes;// hid attributes
} HIDCLASS_COMMON_DEVICE_EXTENSION, *PHIDCLASS_COMMON_DEVICE_EXTENSION;

typedef struct
{
    //
    // parts shared by fdo and pdo
    //
    HIDCLASS_COMMON_DEVICE_EXTENSION Common;

    //以下三行在fdo的StartDevcie中通过irp包查询到
    DEVICE_CAPABILITIES Capabilities;
    HID_DESCRIPTOR HidDescriptor;
    PUCHAR ReportDescriptor; //FDO才会有，用于获取Collection

    //
    // device relations
    //
    PDEVICE_RELATIONS DeviceRelations;//FDO才会有，用于创建pdo

} HIDCLASS_FDO_EXTENSION, *PHIDCLASS_FDO_EXTENSION;

typedef struct
{
    HIDCLASS_COMMON_DEVICE_EXTENSION Common;
    
    //以下pdo才有
    DEVICE_CAPABILITIES Capabilities;
    ULONG CollectionNumber; //相当于pdo序号
    UNICODE_STRING DeviceInterface; //pdo独有
    PDEVICE_OBJECT FDODeviceObject; //pdo在HidClass_Read时要找到fdo，调用fdo的IRP_MJ_INTERNAL_DEVICE_CONTROL处理函数
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension; //pdo处理IRP_MN_REMOVE_DEVICE要用，主要是DeviceRelations这个pdo表

} HIDCLASS_PDO_DEVICE_EXTENSION, *PHIDCLASS_PDO_DEVICE_EXTENSION;

typedef struct __HIDCLASS_FILEOP_CONTEXT__
{
    //
    // device extension
    //
    PHIDCLASS_PDO_DEVICE_EXTENSION DeviceExtension;//注意是PDO的


    KSPIN_LOCK Lock; //保护下面的链表
    LIST_ENTRY ReadPendingIrpListHead;
    LIST_ENTRY IrpCompletedListHead; //为了可以重用irp

    BOOLEAN StopInProgress; //为了发信号要关闭文件
    KEVENT IrpReadComplete; //关闭文件时要等待最后一个irp完成

} HIDCLASS_FILEOP_CONTEXT, *PHIDCLASS_FILEOP_CONTEXT;

//每IRP
//以完成函数参数方式达到"每irp"的目的
typedef struct
{

    PIRP OriginalIrp;//原始irp先放一放

    PHIDCLASS_FILEOP_CONTEXT FileOp;//取Collection信息和report信息，还有完成后的同步问题，毕竟有链表，还有卸载时

    //
    // buffer for reading report
    //
    PVOID InputReportBuffer; //每次读的时候都把report读到这里，二传手
    ULONG InputReportBufferLength;

    //
    // work item
    //
    PIO_WORKITEM CompletionWorkItem; //未用

} HIDCLASS_IRP_CONTEXT, *PHIDCLASS_IRP_CONTEXT;

/* fdo.c */
NTSTATUS
HidClassFDO_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

NTSTATUS
HidClassFDO_DispatchRequest(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

NTSTATUS
HidClassFDO_DispatchRequestSynchronous(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

/* pdo.c */
NTSTATUS
HidClassPDO_CreatePDO(
    IN PDEVICE_OBJECT DeviceObject,
    OUT PDEVICE_RELATIONS *OutDeviceRelations);

NTSTATUS
HidClassPDO_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

PHIDP_COLLECTION_DESC
HidClassPDO_GetCollectionDescription(
    PHIDP_DEVICE_DESC DeviceDescription,
    ULONG CollectionNumber);

PHIDP_REPORT_IDS
HidClassPDO_GetReportDescription(
    PHIDP_DEVICE_DESC DeviceDescription,
    ULONG CollectionNumber);

#endif /* _HIDCLASS_PCH_ */
