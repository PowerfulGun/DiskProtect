#include	<ntifs.h>
#include	<windef.h>
#include	<mountmgr.h>
#include	<mountdev.h>
#include	<ntddvol.h>
#include	<ntstrsafe.h>
#include	"DiskProtect.h"


//当找到需要保护的卷设备时，将该指针指向其过滤设备的设备扩展
PDEVICE_EXTENSION	g_pFilterDevExt = NULL;


NTSTATUS	DriverEntry(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PUNICODE_STRING	_pRegistryPath
)
{
	KdPrint( ("[DriverEntry\n]") );

	ULONG	i;	//用来做循环控制变量

	for ( i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
	{
		//初始化这个驱动所有的分发函数
		_pDriverObject->MajorFunction[i] = _DefaultDispatch;
	}
	//将特殊的分发函数重新赋值为自己的函数
	_pDriverObject->MajorFunction[IRP_MJ_POWER] = _PowerDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_PNP] = _PnpDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = _DeviceControlDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_READ] = _ReadWriteDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_WRITE] = _ReadWriteDispatch;
	//设置驱动的AddDevice函数
	_pDriverObject->DriverExtension->AddDevice = _AddDevice;
	//设置驱动的Unload函数
	_pDriverObject->DriverUnload = _UnloadDispatch;

	//注册一个boot驱动结束回调，这个回调函数会在所有boot驱动运行完毕后执行
	IoRegisterBootDriverReinitialization(
		_pDriverObject ,
		_BootReinitializationRoutine ,
		NULL );

	//返回成功
	return	STATUS_SUCCESS;
}

//当有新的磁盘卷设备建立时，pnp管理起会调用AddDevice函数
NTSTATUS	_AddDevice(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PDEVICE_OBJECT	_pPhysicalDeviceObject
)
{
	KdPrint( ("[_AddDevice]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	//用来指向过滤设备的设备扩展的指针
	PDEVICE_EXTENSION	pFilterDevExt = NULL;
	//下层设备的指针
	PDEVICE_OBJECT	pLowerDeviceObject = NULL;
	//过滤设备的指针
	PDEVICE_OBJECT	pFilterDeviceObject = NULL;
	//过滤设备的处理线程句柄
	HANDLE	hThreadHandle = NULL;
	
	__try
	{
	//建立一个过滤设备，这个设备时FILE DEVICE DISK类型的设备，有自定义的设备扩展
		status = IoCreateDevice(
			_pDriverObject ,
			sizeof( DEVICE_EXTENSION ) ,
			NULL ,
			FILE_DEVICE_DISK ,
			FILE_DEVICE_SECURE_OPEN ,
			FALSE ,
			&pFilterDeviceObject );
		if (!NT_SUCCESS( status ))
		{
			KdPrint( ("_AddDevice.IoCreateDevice fail ,status = %u" , status) );
			__leave;
		}
		//设置过滤设备扩展指针
		pFilterDevExt = pFilterDeviceObject->DeviceExtension;
		//清空过滤设备的设备扩展
		RtlZeroMemory( pFilterDevExt , sizeof( DEVICE_EXTENSION ) );
		//将过滤设备附加到卷设备的物理设备上
		pLowerDeviceObject = IoAttachDeviceToDeviceStack( pFilterDeviceObject , _pPhysicalDeviceObject );
		if (pLowerDeviceObject == NULL)
		{
			KdPrint( ("_AddDeivce.ioAttachDeviceToDeviceStack fail ,status = %u" , status) );
			status = STATUS_NO_SUCH_DEVICE;
			__leave;
		}
		//初始化这个卷设备的分页路径计数事件
		KeInitializeEvent(
			&pFilterDevExt->PagingPathCountEvent ,
			SynchronizationEvent ,
			TRUE );
		//过滤设备的属性应该和下层设备相同
		pFilterDeviceObject->Flags = pLowerDeviceObject->Flags;
		//给过滤设备加上电源可分页的属性
		pFilterDeviceObject->Flags |= DO_POWER_PAGABLE;
		//过滤设备初始化完毕
		pFilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

		//设置过滤设备设备扩展中的相应成员
		pFilterDevExt->pFilterDeviceObject = pFilterDeviceObject;
		pFilterDevExt->pPhysicalDeviceObject = _pPhysicalDeviceObject;
		pFilterDevExt->pLowerDeviceObject = pLowerDeviceObject;
		//初始化请求处理队列
		InitializeListHead( &pFilterDevExt->RequestList );
		//初始化请求处理队列的自旋锁
		KeInitializeSpinLock( &pFilterDevExt->RequestSpinLock );
		//初始化请求队列的同步事件
		KeInitializeEvent(
			&pFilterDevExt->RequestEvent ,
			SynchronizationEvent ,
			FALSE );
		//初始化线程终止标志
		pFilterDevExt->ThreadExitFlag = FALSE;
		//建立用来处理卷设备请求的线程，传入的参数是设备扩展
		status = PsCreateSystemThread(
			&hThreadHandle ,
			(ACCESS_MASK)0L ,
			NULL ,
			NULL ,
			NULL ,
			_ReadWriteThreadEntry ,
			pFilterDevExt );
		if (!NT_SUCCESS( status ))
		{
			KdPrint( ("_AddDevice.PsCreateSystemThread fail ,status = %u" , status) );
			__leave;
		}
		//获取处理线程的线程对象
		status = ObReferenceObjectByHandle(
			hThreadHandle ,
			THREAD_ALL_ACCESS ,
			NULL ,
			KernelMode ,
			&pFilterDevExt->hThreadHandle ,
			NULL );
		if (!NT_SUCCESS( status ))
		{
			KdPrint( ("_AddDevice.ObReferenceObjectByHandle fail ,status = %u" , status) );
			pFilterDevExt->ThreadExitFlag = TRUE;
			KeSetEvent( &pFilterDevExt->RequestEvent , 0 , FALSE );
			__leave;
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{

	}
	ERROR_OUT:
	if (!NT_SUCCESS( status ))
	{
		//如果有不成功的地方，就要解除可能存在的设备附加
		if (pLowerDeviceObject != NULL)
		{
			IoDetachDevice( pLowerDeviceObject );
			pFilterDevExt->pLowerDeviceObject = NULL;
		}
		//删除可能建立的过滤设备
		if (pFilterDeviceObject != NULL)
		{
			IoDeleteDevice( pFilterDeviceObject );
			pFilterDevExt->pFilterDeviceObject = NULL;
		}
	}

	//关闭线程句柄，所有对线程的引用都通过线程对象进行
	if (hThreadHandle != NULL)
	{
		ZwClose( hThreadHandle );
	}
	return	status;
}

//pnp请求的处理
NTSTATUS	_PnpDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	KdPrint( ("[_PnpDispatch]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	//获取设备扩展
	PDEVICE_EXTENSION	pDevExt = _pDeviceObject->DeviceExtension;
	//获取当前IRP栈单元
	PIO_STACK_LOCATION	pIrpStack = IoGetCurrentIrpStackLocation( _pIrp );

	switch (pIrpStack->MinorFunction)
	{
		case IRP_MN_REMOVE_DEVICE:
			//如果pnp管理器发过来的是移除设备的irp，将进入这里处理,做一些清理工作
			{
				//如果处理线程还在运行，需要通知其停止
				if (pDevExt->ThreadExitFlag != TRUE && pDevExt->hThreadHandle != NULL)
				{
					pDevExt->ThreadExitFlag = TRUE;
					KeSetEvent( &pDevExt->RequestEvent , 0 , FALSE );
					//等待线程结束
					KeWaitForSingleObject(
						pDevExt->hThreadHandle ,	//线程句柄也可以当作同步对象被等待
						Executive ,
						KernelMode ,
						FALSE ,
						NULL );	//永久等待，不设置超时
					//解除线程对象引用
					ObDereferenceObject( pDevExt->hThreadHandle );
				}
				//如果还有位图要释放
				if (pDevExt->pBitmap)
				{
					_BitmapFree( pDevExt->pBitmap );
				}
				//如果存在下层设备，要去掉附加
				if (pDevExt->pLowerDeviceObject)
				{
					IoDetachDevice( pDevExt->pLowerDeviceObject );
				}
				//如果存在过滤设备，就要删除
				if (pDevExt->pFilterDeviceObject)
				{
					IoDeleteDevice( pDevExt->pFilterDeviceObject );
				}

				//下发到下层设备，所以break
				break;
			}

		case IRP_MN_DEVICE_USAGE_NOTIFICATION:
			//第二个要处理的是设备使用通告请求，Windows在建立或者删除特殊文件的时候会向存储设备发出这个iRP请求，过滤设备自然也会
			//接受到，特殊文件包活 页面文件、休眠文件和dump文件
			//IrpStack中的Parameters UsageNotification Type域会说明是哪种文件
			//IrpStack中的Parameters UsageNotification InPath域说明这个请求是询问设备是否可以建立这个文件，还是在删除了这个文件
			//之后对这个设备的通知.
			//驱动比较关心的是对页面文件的处理，如果有页面文件在卷上，那么应该清除过滤设备的DO_POWER_PAGABLE位，反之加上这个位
			{
				BOOLEAN	SetPagable;

				//如果是询问是否支持休眠文件和dump文件，则直接下发给下层设备去处理
				if (pIrpStack->Parameters.UsageNotification.Type != DeviceUsageTypePaging)
				{
					//跳过当前irpstack
					IoSkipCurrentIrpStackLocation( _pIrp );
					status = IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );
					return	status;
				}

				//为了同步处理页面文件的请求，这里需要等待分页计数事件
				status = KeWaitForSingleObject(
					&pDevExt->PagingPathCountEvent ,
					Executive ,
					KernelMode ,
					FALSE ,
					NULL );
				//SetPageble初始化为假，没有设置过DO_POWER_PAGABLE位
				SetPagable = FALSE;

				//如果InPath域为假，代表Pnp管理器通知我们删去分页文件，并且当目前只剩下最后一个分页文件的时候
				if (!(pIrpStack->Parameters.UsageNotification.InPath) && pDevExt->PagingPathCount == 1)
				{
					//Windows Vista之后的版本设备对象的标志位中 DO_POWER_INRUSH和DO_POWER_PAGABLE可以同时存在，不需要判断
					//if(_pDeviceObject->Flags & DO_POWER_INRUSH)
					//说明没有分页文件在这个设备上了，需要设置DO_POWER_PAGABLE位
					_pDeviceObject->Flags |= DO_POWER_PAGABLE;
					SetPagable = TRUE;

				}

				//到这里，就是关于分页文件的是否可以建立查询 或者删除的通知，交给下层设备处理
				//需要同步方式发送给下层设备，等待其返回
				status = _ForwardIrpSync( pDevExt->pLowerDeviceObject , _pIrp );
				if (NT_SUCCESS( status ))
				{
					//如果发给下层的请求成功了，说明下层设备支持这个操作，
					//在成功的条件下需要改变计数值，记录现在这个设备上到底有多少个分页文件
					IoAdjustPagingPathCount(
						&pDevExt->PagingPathCount ,
						pIrpStack->Parameters.UsageNotification.InPath );

					if (pIrpStack->Parameters.UsageNotification.InPath && pDevExt->PagingPathCount == 1)
					{
						//如果这个请求是一个建立分页文件的查询请求，并且下层设备支持，并且这是第一个在这个设备上的分页文件，
						//就需要清楚过滤设备的DO_POWER_PAGABLE位
						_pDeviceObject->Flags &= DO_POWER_PAGABLE;
					}
				}
				else
				{
					//到这里说明下层不支持这个请求，需要把之前做过的操作还原，清除DO_POWER_PAGABLE位
					if (SetPagable == TRUE)
					{
						_pDeviceObject->Flags &= DO_POWER_PAGABLE;
						SetPagable = FALSE;
					}
				}

				//设置分页计数事件为有信号，下一个等待的操作才会执行，用于串行操作
				KeSetEvent(
					&pDevExt->PagingPathCountEvent ,
					IO_NO_INCREMENT ,
					FALSE );

				//完成Irp请求
				IoCompleteRequest( _pIrp , IO_NO_INCREMENT );
				return	status;
			}

		default:
			//其他的pnp请求直接下发到下层设备
			break;
	}

	//下发到下层设备
	IoSkipCurrentIrpStackLocation( _pIrp );
	return	IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );
}

NTSTATUS	_ForwardIrpSync(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	KdPrint( ("[_ForwardIrpSync]\n") );

	NTSTATUS	status;
	//用于等待的事件
	KEVENT	WaitEvent;


	KeInitializeEvent(
		&WaitEvent ,
		NotificationEvent ,
		FALSE );

	//为下层创建io栈单元,选用复制操作
	IoCopyCurrentIrpStackLocationToNext( _pIrp );
	//设置完成irp函数，当下层处理完irp时回调
	IoSetCompletionRoutine(
		_pIrp ,
		_PnpIrpCompletionRoutine ,
		&WaitEvent ,
		TRUE ,
		TRUE ,
		TRUE );

	//将Irp发送到下层
	status = IoCallDriver( _pDeviceObject , _pIrp );
	if (status == STATUS_PENDING)
	{
		//等待事件
		KeWaitForSingleObject(
			&WaitEvent ,
			Executive ,
			KernelMode ,
			NULL ,
			NULL );

		//返回之后获取irp操作状态
		status = _pIrp->IoStatus.Status;
	}

	//返回状态
	return	status;
}


NTSTATUS	_PnpIrpCompletionRoutine(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp ,
	IN	PVOID	_Context
)
{
	KdPrint( ("[_PnpIrpCompletionRoutine]") );

	PKEVENT	WaitEvent = (PKEVENT)_Context;

	//设置等待对象为有信号，使等待它的进程继续运行，_ForwardIrpSync中的代码
	//由于驱动会重新完成该Irp，所以返回STATUS_MORE_PROCESSING_REQUIRED
	return	STATUS_MORE_PROCESSING_REQUIRED;
}


//电源处理，如果是Windows Vista以前的版本，需要使用特殊函数下发该Irp，如果是之后的版本，可以直接下发给底层设备
NTSTATUS	_PowerDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	KdPrint( ("[_PowerDispatch]\n") );

	PDEVICE_EXTENSION	pDevExt = _pDeviceObject->DeviceExtension;

#if (NTDDI_VERSION < NTDDI_VISTA)

	PoStartNextPowerIrp( _pIrp );
	IoSkipCurrentIrpStackLocation( _pIrp );
	return	PoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );

#else

	IoSkipCurrentIrpStackLocation( _pIrp );
	return	IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );

#endif // (NTDDI_VERSION < NTDDI_VISTA)

}


//需要在DeviceIoControl中获取IOCTL_VOLUME_ONLINE，这是由操作系统发出的，目标卷设备只有接受到这个，才会处于在线状态，才能获取其参数
//先下发给目标卷设备，在读取其参数
NTSTATUS	_DeviceControlDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	//KdPrint( ("[_DeviceControlDispatch]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	PDEVICE_EXTENSION	pDevExt = _pDeviceObject->DeviceExtension;
	PIO_STACK_LOCATION	pIrpStack = IoGetCurrentIrpStackLocation( _pIrp );
	KEVENT	WaitEvent;
	//用来传给IOCTL_VOLUME_ONLINE的完成函数的上下文
	VOLUME_ONLINE_CONTEXT	Context;


	switch (pIrpStack->Parameters.DeviceIoControl.IoControlCode)
	{
		case IOCTL_VOLUME_ONLINE:
			{
				//先初始化一个事件来同步
				KeInitializeEvent( &WaitEvent , NotificationEvent , FALSE );

				//初始化传给完成函数的参数
				Context.pDevExt = pDevExt;
				Context.pWaitEvent = &WaitEvent;

				IoCopyCurrentIrpStackLocationToNext( _pIrp );
				//设置完成函数
				IoSetCompletionRoutine(
					_pIrp ,
					_VolumeOnlineCompletionRoutine ,
					&Context ,
					TRUE ,
					TRUE ,
					TRUE );

				//发给下层设备
				status = IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );

				//等待下层设备结束这个irp
				KeWaitForSingleObject(
					&WaitEvent ,
					Executive ,
					KernelMode ,
					FALSE ,
					NULL );

				return	status;
			}

		default:
			//其他DeviceIoControl，一律发送给下层设备
			break;
	}

	IoSkipCurrentIrpStackLocation( _pIrp );
	return	IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );
}


NTSTATUS	_VolumeOnlineCompletionRoutine(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp,
	IN	PVOLUME_ONLINE_CONTEXT	_pVolumeContext
)
{
	KdPrint( ("[_VolumeOnlineCompletionRoutine]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	//这个卷的盘符
	UNICODE_STRING	DiskName = { 0 };


	ASSERT( _pVolumeContext != NULL );

	__try
	{
		//调用函数获取物理设备的名字，就是盘符
		status = IoVolumeDeviceToDosName( _pVolumeContext->pDevExt->pPhysicalDeviceObject , &DiskName );
		if (!NT_SUCCESS( status ))
			__leave;

		KdPrint( ("The current disk name is %C" , DiskName.Buffer[0]) );

		_pVolumeContext->pDevExt->VolumeLetter = DiskName.Buffer[0];

		////将名称变成大写
		//if (_pVolumeContext->pDevExt->VolumeLetter > L'Z')
		//	_pVolumeContext->pDevExt->VolumeLetter -= (L'a' - L'A');

		//只保护D盘
		if (_pVolumeContext->pDevExt->VolumeLetter == L'D' || _pVolumeContext->pDevExt->VolumeLetter == L'd')
		{
			//获取这个卷的基本信息
			status = _QueryVolumeInformation(
				_pVolumeContext->pDevExt->pPhysicalDeviceObject ,
				&(_pVolumeContext->pDevExt->liVolumeTotalSize) ,
				&(_pVolumeContext->pDevExt->dwPerClusterSize) ,
				&(_pVolumeContext->pDevExt->dwPerSectorSize) );
			if (!NT_SUCCESS( status ))
				__leave;

			//建立这个卷对应的位图结构
			status = _BitmapInit(
				&(_pVolumeContext->pDevExt->pBitmap) ,
				_pVolumeContext->pDevExt->dwPerSectorSize ,
				8 ,
				25600 ,
				(DWORD)(_pVolumeContext->pDevExt->liVolumeTotalSize.QuadPart /
				(LONGLONG)(25600 * 8 * _pVolumeContext->pDevExt->dwPerSectorSize)) + 1 );
			if (!NT_SUCCESS( status ))
				__leave;

			//找到要保护的设备了，赋值给全局变量
			g_pFilterDevExt = _pVolumeContext->pDevExt;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{

	}

	if (!NT_SUCCESS( status ))
	{
		if (_pVolumeContext->pDevExt->pBitmap)
		{
			_BitmapFree( _pVolumeContext->pDevExt->pBitmap );
		}

		if (_pVolumeContext->pDevExt->hTempFile)
		{
			ZwClose( _pVolumeContext->pDevExt->hTempFile );
		}
	}

	if (DiskName.Buffer != NULL);
	{
		ExFreePool( DiskName.Buffer );
	}

	//设置等待同步事件，让DeviceIoControl继续执行
	KeSetEvent( _pVolumeContext->pWaitEvent , 0 , FALSE );

	return	STATUS_SUCCESS;
}


NTSTATUS	_DefaultDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	PDEVICE_EXTENSION	pDevExt = _pDeviceObject->DeviceExtension;

	IoSkipCurrentIrpStackLocation( _pIrp );
	return	IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );
}


VOID	_UnloadDispatch(
	IN	PDRIVER_OBJECT	_pDriverObject
)
{
	//什么都不做，驱动会运行到关机
	return;
}


NTSTATUS	_QueryVolumeInformation(
	PDEVICE_OBJECT	_pDeviceObject ,
	PLARGE_INTEGER	_liTotalSize ,
	PDWORD	_pdwPerClusterSize ,
	PDWORD	_pdwPerSectorSize
)
{
	KdPrint( ("[_QueryVolumeInformation]\n") );

#define	FAT16_SIG_OFFSET	54	//定义FAT16文件系统签名的偏移量
#define	FAT32_SIG_OFFSET	82	//FAT32文件系统签名的偏移量
#define	NTFS_SIG_OFFSET		3	//NTFS文件系统签名的偏移量

	//FAT16文件系统标志
	UCHAR	FAT16Flag[4] = { 'F','A','T','1' };
	//FAT32文件系统标志
	UCHAR	FAT32Flag[4] = { 'F','A','T','3' };
	//NTFS文件系统标志
	UCHAR	NTFSFlag[4] = { 'N','T','F','S' };

	NTSTATUS	status = STATUS_SUCCESS;
	//用来读取DBR扇区的数据缓冲区
	BYTE	DBR[512] = { 0 };
	ULONG	DBRSize = 512;

	//以下三个指针统一指向DBR缓冲区
	//但是分别代表NTFS、FAT32和FAT16的DBR结构
	PNTFS_BOOT_SECTOR	pNtfsBootSector =
		(PNTFS_BOOT_SECTOR)DBR;
	PFAT32_BOOT_SECTOR	pFat32BootSector =
		(PFAT32_BOOT_SECTOR)DBR;
	PFAT16_BOOT_SECTOR	pFat16BootSector =
		(PFAT16_BOOT_SECTOR)DBR;

	//读取的偏移量，对于DBR来说是卷的起始位置，偏移量为0
	LARGE_INTEGER	liReadOffset = { 0 };
	//记录IO操作状态
	IO_STATUS_BLOCK	IoStatusBlock;

	//为了同步读取设置的同步事件
	KEVENT	WaitEvent;
	//要构建的IRP指针
	PIRP	pIrp = NULL;


	__try
	{
		//下面我们首先从指定的卷设备上读取偏移量为0的一个扇区，也就是这个卷的DBR扇区，准备加以分析
		//因为我们要同步读取，所以先初始化一个为了同步读取设置的事件
		KeInitializeEvent( &WaitEvent , NotificationEvent , FALSE );

		//构造一个irp用来发给卷设备读取信息
		pIrp = IoBuildAsynchronousFsdRequest(
			IRP_MJ_READ ,
			_pDeviceObject ,
			DBR ,
			DBRSize ,
			&liReadOffset ,
			&IoStatusBlock );
		if (pIrp == NULL)
		{
			__leave;
		}

		//设置完成函数，将同步事件作为参数传入
		IoSetCompletionRoutine(
			pIrp ,
			_QueryVolumeInformationCompletionRoutine ,
			&WaitEvent ,
			TRUE ,
			TRUE ,
			TRUE );

		status = IoCallDriver( _pDeviceObject , pIrp );
		if (status == STATUS_PENDING)
		{
			//如果下层设备一时完成不了就等等
			KeWaitForSingleObject(
				&WaitEvent ,
				Executive ,
				KernelMode ,
				FALSE ,
				NULL );

			status = pIrp->IoStatus.Status;
			if(!NT_SUCCESS(status))
			{
				__leave;
			}
		}

		if (*(DWORD*)NTFSFlag == *(DWORD*)&DBR[NTFS_SIG_OFFSET])
		{
			//通过比较标志发现这个卷是一个ntfs文件系统的卷，下面根据ntfs卷的DBR定义来对各种需要获取的值进行赋值操作
			*_pdwPerSectorSize = (DWORD)(pNtfsBootSector->BytesPerSector);
			*_pdwPerClusterSize = (*_pdwPerSectorSize) * (DWORD)(pNtfsBootSector->SectorsPerCluster);
			_liTotalSize->QuadPart = (LONGLONG)(*_pdwPerSectorSize) * (LONGLONG)pNtfsBootSector->TotalSectors;
		}
		else if (*(DWORD*)FAT32Flag == *(DWORD*)&DBR[FAT32_SIG_OFFSET])
		{
			//通过比较标志发现这个卷是一个ntfs文件系统的卷，下面根据ntfs卷的DBR定义来对各种需要获取的值进行赋值操作
			*_pdwPerSectorSize = (DWORD)(pFat32BootSector->BytesPerSector);
			*_pdwPerClusterSize = (*_pdwPerSectorSize) * (DWORD)(pFat32BootSector->SectorsPerCluster);
			_liTotalSize->QuadPart = (LONGLONG)(*_pdwPerSectorSize) *
				(LONGLONG)(pFat32BootSector->LargeSectors + pFat32BootSector->Sectors);
		}
		else if (*(DWORD*)FAT16Flag == *(DWORD*)&DBR[FAT16_SIG_OFFSET])
		{
			//通过比较标志发现这个卷是一个ntfs文件系统的卷，下面根据ntfs卷的DBR定义来对各种需要获取的值进行赋值操作
			*_pdwPerSectorSize = (DWORD)(pFat16BootSector->BytesPerSector);
			*_pdwPerClusterSize = (*_pdwPerSectorSize) * (DWORD)(pFat16BootSector->SectorsPerCluster);
			_liTotalSize->QuadPart = (LONGLONG)(*_pdwPerSectorSize) *
				(LONGLONG)(pFat16BootSector->LargeSectors + pFat16BootSector->Sectors);
		}
		else
		{
			//走到这里，可能是其它任何文件系统，但是不是windows认识的文件系统，我们统一返回错
			status = STATUS_UNSUCCESSFUL;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{

	}

ERROR_OUT:

	if (pIrp != NULL)
	{
		IoFreeIrp( pIrp );
	}
	return	status;
}


NTSTATUS	_QueryVolumeInformationCompletionRoutine(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp ,
	IN	PVOID	_Context
)
{
	KdPrint( ("[_QueryVolumeInformationCompletionRoutine]\n") );

	KeSetEvent(
		(PKEVENT)_Context ,
		IO_NO_INCREMENT ,
		FALSE );
	
	//该Irp需要被释放,要再次获得操作
	return	STATUS_MORE_PROCESSING_REQUIRED;
}


VOID	_BootReinitializationRoutine(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PVOID	_Context ,
	IN	ULONG	Count
)
{
	KdPrint( ("[_BootReinitializationRoutine]") );

	NTSTATUS	status;
	//E盘的稀疏文件名
	WCHAR	SparseFileName[] = L"\\??\\E:\\temp.dat";
	UNICODE_STRING	strSparseFileName;
	IO_STATUS_BLOCK	IoStatusBlock = { 0 };
	//建立文件时的对象属性变量
	OBJECT_ATTRIBUTES	ObjectAttr = { 0 };
	//设置文件大小的时候使用的文件结尾描述符
	FILE_END_OF_FILE_INFORMATION	FileEndInfo = { 0 };


	__try
	{
		//初始化要打开的文件名
		RtlInitUnicodeString( &strSparseFileName , SparseFileName );
		//初始化文件名对应的对象名，这里需要将其初始化为内核对象，并且大小写不敏感
		InitializeObjectAttributes(
			&ObjectAttr ,
			&strSparseFileName ,
			OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE ,
			NULL ,
			NULL );

		//建立文件，这里需要注意的是，要加入FILE_NO_INTERMEDIATE_BUFFERING选项，避免文件系统再缓存这个文件
		status = ZwCreateFile(
			&g_pFilterDevExt->hTempFile ,
			GENERIC_READ | GENERIC_WRITE ,
			&ObjectAttr ,
			&IoStatusBlock ,
			NULL ,
			FILE_ATTRIBUTE_NORMAL ,
			0 ,
			FILE_OVERWRITE_IF ,
			FILE_NON_DIRECTORY_FILE |
			FILE_RANDOM_ACCESS |
			FILE_SYNCHRONOUS_IO_NONALERT |
			FILE_NO_INTERMEDIATE_BUFFERING ,
			NULL ,
			0 );
		if (!NT_SUCCESS( status ))
		{
			__leave;
		}

		//设置这个文件为稀疏文件
		status = ZwFsControlFile(
			g_pFilterDevExt->hTempFile ,
			NULL ,
			NULL ,
			NULL ,
			&IoStatusBlock ,
			FSCTL_SET_SPARSE ,
			NULL ,
			0 ,
			NULL ,
			0 );
		if (!NT_SUCCESS( status ))
		{
			__leave;
		}

		//设置这个文件大小为D盘的大小并且留出10m的保护空间
		FileEndInfo.EndOfFile.QuadPart =
			g_pFilterDevExt->liVolumeTotalSize.QuadPart + 10 * 1024 * 1024;
		status = ZwSetInformationFile(
			g_pFilterDevExt->hTempFile ,
			&IoStatusBlock ,
			&FileEndInfo ,
			sizeof( FILE_END_OF_FILE_INFORMATION ) ,
			FileEndOfFileInformation );
		if (!NT_SUCCESS( status ))
		{
			__leave;
		}

		//如果成功初始化就将这个卷的保护标志设置为在保护状态
		g_pFilterDevExt->bProtect = TRUE;

		return;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{

	}
ERROR_OUT:
	KdPrint( ("Error create temp file\n") );
	return;
}


NTSTATUS	_BitmapInit(
	OUT	PBITMAP*	_ppBitmap ,
	IN	ULONG	_PerSectorSize ,
	IN	ULONG	_Bits_Of_Byte ,
	IN	ULONG	_Bytes_Of_Region ,
	IN	ULONG	_RegionNumber
)
{
	KdPrint( ("[_BitmaoInit]") );

	ULONG	i = 0;
	NTSTATUS	status = STATUS_SUCCESS;
	PBITMAP	pBitmap = NULL;


	//检查参数合法
	if (_ppBitmap == NULL || _PerSectorSize == 0 ||
		_Bits_Of_Byte == 0 || _Bytes_Of_Region == 0 ||
		_RegionNumber == 0)
	{
		return	STATUS_UNSUCCESSFUL;
	}

	__try
	{
		//分配一个Bitmap结构，这是一定要分配的，相当于一个handle
		pBitmap = ExAllocatePoolWithTag(
			NonPagedPool ,
			sizeof( BITMAP ) ,
			'hqsb' );
		if (pBitmap == NULL)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}
		//清空结构
		RtlZeroMemory( pBitmap , sizeof( BITMAP ) );

		//根据参数对结构中的成员进行赋值
		pBitmap->PerSectorSize = _PerSectorSize;
		pBitmap->Bits_Of_Byte = _Bits_Of_Byte;
		pBitmap->Bytes_Of_Region = _Bytes_Of_Region;
		pBitmap->RegionNumber = _RegionNumber;
		pBitmap->PerRegionSizeMapped =
			_Bytes_Of_Region * _Bits_Of_Byte * _PerSectorSize;
		pBitmap->BitmapSizeMapped =
			pBitmap->PerRegionSizeMapped * _RegionNumber;

		//分配一个指针数组，有RegionNumber个元素
		pBitmap->bitmap = ExAllocatePoolWithTag(
			NonPagedPool ,
			sizeof( PUCHAR ) * _RegionNumber ,
			'hqsb' );
		if (pBitmap->bitmap == NULL)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}
		//清空指针数组
		RtlZeroMemory( pBitmap->bitmap , sizeof( PUCHAR )*_RegionNumber );

		//将分配好的Bitmap回传给调用者
		*_ppBitmap = pBitmap;
		status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		status = STATUS_UNSUCCESSFUL;
	}

	//如果有操作失败，要释放资源
	if (!NT_SUCCESS( status ))
	{
		if (pBitmap != NULL)
		{
			_BitmapFree( pBitmap );
		}
		*_ppBitmap = NULL;
	}

	return status;
}


//用来释放Bitmap占用的内存的函数
VOID	_BitmapFree(
	IN	PBITMAP	_pBitmap
)
{
	KdPrint( ("[_BitmapFree]\n") );

	ULONG	i;


	if (_pBitmap != NULL)
	{
		//先释放结构中bitmap占用的内存
		if (_pBitmap->bitmap != NULL)
		{
			//遍历指针数组中的每个元素
			for (i = 0; i < _pBitmap->RegionNumber; i++)
			{
				//如果这个元素有值，代表其指向一块内存，就需要释放
				if (*(_pBitmap->bitmap + i) != NULL)
				{
					ExFreePoolWithTag(
						*(_pBitmap->bitmap + i) ,
						'hqsb' );
				}
			}

			//释放完每个元素的内容，就要释放这个指针数组占用的内存
			ExFreePoolWithTag(
				_pBitmap->bitmap ,
				'hqsb' );
		}

		//释放完结构中的bitmap，就释放整个结构体占用的内存
		ExFreePoolWithTag(
			_pBitmap ,
			'hqsb' );
	}
}


NTSTATUS	_BitmapSet(
	IN OUT	PBITMAP	_pBitmap ,
	IN	LARGE_INTEGER	_liOffset ,
	IN	ULONG	_Length
)
{
	KdPrint( ("[_BitmapSet]\n") );

	LONGLONG	i = 0;
	NTSTATUS	status = STATUS_SUCCESS;
	ULONG	RegionStart = 0 , RegionEnd = 0;
	ULONG	OffsetStartInRegion = 0 , OffsetEndInRegion = 0;
	ULONG	ByteStartInRegion = 0 , ByteEndInRegion = 0;
	ULONG	BitOffsetInByte = 0;
	LARGE_INTEGER	SetStart = { 0 } , SetEnd = { 0 };


	__try
	{
		//检查参数
		if (_pBitmap == NULL || _liOffset.QuadPart < 0)
		{
			KdPrint( ("_BitmapSet:\n\t\
				The _pBitmap or Offset is invalid") );
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		//操作的大小必须是扇区的倍数
		if (_liOffset.QuadPart%_pBitmap->PerSectorSize != 0 ||
			_Length%_pBitmap->PerSectorSize != 0)
		{
			KdPrint( ("_BitmapSet:\n\t\
				The Offset or Length is invalid") );
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		//根据要设置的偏移量和长度来计算要使用哪些Region，
		//如果需要的话，就分配指向的空间
		RegionStart = _liOffset.QuadPart / _pBitmap->PerRegionSizeMapped;
		RegionEnd = (_liOffset.QuadPart + _Length) / _pBitmap->PerRegionSizeMapped;
		for (i = RegionStart; i <= RegionEnd; ++i)
		{
			//指针为空代表还没有分配内存,要先分配内存
			if (*(_pBitmap->bitmap + i) == NULL)
			{
				*(_pBitmap->bitmap + i) = 
					ExAllocatePoolWithTag(
					NonPagedPool ,
					sizeof( UCHAR )*_pBitmap->PerRegionSizeMapped ,
					'hqsb' );
				if (*(_pBitmap->bitmap + i) == NULL)
				{
					KdPrint( ("_BitmapSet.ExAllocatePoolWithTag fail") );
					status = STATUS_INSUFFICIENT_RESOURCES;
					__leave;
				}
				else
				{
					RtlZeroMemory( 
						*(_pBitmap->bitmap + i) , 
						sizeof( UCHAR )*_pBitmap->PerRegionSizeMapped );
				}
			}
		}

		//开始设置bitmap，即将其中的位置1
		//先将要设置的区域按照byte对齐，没有字节对齐的区域先手工按位设置
		for (i = _liOffset.QuadPart; i < _liOffset.QuadPart + _Length; i += _pBitmap->PerSectorSize)
		{
			//算出起始块，即指针数组中元素的位置
			RegionStart = i / _pBitmap->PerRegionSizeMapped;
			//余下的在块中的映射的偏移，根据这个值可以算出在块中的起始字节
			OffsetStartInRegion = i % _pBitmap->PerRegionSizeMapped;
			//计算要设置的在块中的起始字节位置
			ByteStartInRegion =
				OffsetStartInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			//计算要设置的起始位的位置，这个位是在起始字节当中
			BitOffsetInByte =
				(OffsetStartInRegion / _pBitmap->PerSectorSize) % _pBitmap->Bits_Of_Byte;

			if (BitOffsetInByte == 0)
			{
				//如果起始位的位置是0，
				//代表要设置的起始位置是按照字节对齐的
				//不用手工设置
				//要按字节设置的起始位置就是i值
				SetStart.QuadPart = i;	
				break;
			}
			//找到起始位在字节中的偏移了，要手工设置这个位
			*(*(_pBitmap->bitmap + RegionStart) + ByteStartInRegion)
				|= BitMask[BitOffsetInByte];

			//进入下一次循环，退出循环有2个情况
			//直到BitOffsetInByte为0，即找到字节对齐的位置
			//或者i的值等于Offset + Length，即总共要设置的都不到一个
			//字节，会全部手工设置好
		}
		//如果i等于大于offset + Length，即已经全部手工设置完成
		//可以返回了
		if (i >= _liOffset.QuadPart + _Length)
		{
			KdPrint( ("The set bits are less than one byte ,\
				had set by manual"));
			status = STATUS_SUCCESS;
			__leave;
		}

		//还要保证尾部也是按照字节对齐的
		for (
			i = _liOffset.QuadPart + _Length - _pBitmap->PerSectorSize;
			i >= _liOffset.QuadPart;
			i -= _pBitmap->PerSectorSize
			)
		{
			RegionEnd = i / _pBitmap->PerRegionSizeMapped;
			OffsetEndInRegion = i % _pBitmap->PerRegionSizeMapped;
			ByteEndInRegion =
				OffsetEndInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			BitOffsetInByte =
				(OffsetEndInRegion / _pBitmap->PerSectorSize) % _pBitmap->Bits_Of_Byte;
			if (BitOffsetInByte == 7)
			{
				SetEnd.QuadPart = i;
				break;
			}
			*(*(_pBitmap->bitmap + RegionEnd) + ByteEndInRegion)
				|= BitMask[BitOffsetInByte];
		}

		//判断是否已经全部设置完毕
		if (i < _liOffset.QuadPart ||
			SetEnd.QuadPart == SetStart.QuadPart)
		{
			status = STATUS_SUCCESS;
			__leave;
		}

		//要设置的末尾块，即指针数组索引的结束位置
		RegionEnd = SetEnd.QuadPart / _pBitmap->PerRegionSizeMapped;
		//按字节设置位
		for (i = SetStart.QuadPart; i <= SetEnd.QuadPart;)
		{
			RegionStart = i / _pBitmap->PerRegionSizeMapped;
			OffsetStartInRegion =
				i % _pBitmap->PerRegionSizeMapped;
			ByteStartInRegion =
				OffsetStartInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			//如果要设置的位没有跨越两个Region，只要memset按字节设置
			if (RegionStart == RegionEnd)
			{
				OffsetEndInRegion = SetEnd.QuadPart % _pBitmap->PerRegionSizeMapped;
				ByteEndInRegion =
					OffsetEndInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
				memset(
					*(_pBitmap->bitmap + RegionStart) + ByteStartInRegion ,
					0xff ,
					ByteEndInRegion - ByteStartInRegion + 1 );
				break;
			}
			//如果我们设置的位跨越了两个region，需要递增i的大小
			else
			{
				OffsetEndInRegion = _pBitmap->PerRegionSizeMapped;
				ByteEndInRegion =
					OffsetEndInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
				memset(
					*(_pBitmap->bitmap + RegionStart) + ByteStartInRegion ,
					0xff ,
					ByteEndInRegion - ByteStartInRegion );
				i += (ByteEndInRegion - ByteStartInRegion) * _pBitmap->PerSectorSize * _pBitmap->Bits_Of_Byte;
			}
		}
		status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		status = STATUS_UNSUCCESSFUL;
	}

	return	status;

}


LONG	_BitmapTest(
	PBITMAP	_pBitmap ,
	LARGE_INTEGER	_liOffset ,
	ULONG	_Length
)
{
	KdPrint( ("[_BitmapTest]\n") );

	LONGLONG	i = 0;
	LONG	Result = 0;
	ULONG	RegionStart = 0;
	ULONG	OffsetStartInRegion = 0;
	ULONG	ByteStartInRegion = 0;
	ULONG	BitOffsetInByte = 0;


	__try
	{
		//检查参数
		if (_pBitmap == NULL ||
			_liOffset.QuadPart < 0 ||
			_liOffset.QuadPart + _Length > _pBitmap->BitmapSizeMapped )
		{
			KdPrint( ("_BitmapTest: Invalid Parameters") );
			Result = STATUS_INVALID_PARAMETER;
			__leave;
		}

		if (NULL == *(_pBitmap->bitmap + _liOffset.QuadPart / _pBitmap->PerRegionSizeMapped))
			return	BITS_ALL_CLEAR;

		for (
			i = _liOffset.QuadPart; 
			i < _liOffset.QuadPart + _Length; 
			i += _pBitmap->PerSectorSize
			)
		{
			//针对范围内的bit进行测试
			//如果全部为0 返回BITS_ALL_CLEAR
			//如果全部为1 返回BITS_ALL_SET
			//如果是0和1混合 返回BITS_BLEND
			RegionStart = i / _pBitmap->PerRegionSizeMapped;
			OffsetStartInRegion = i % _pBitmap->PerRegionSizeMapped;
			ByteStartInRegion =
				OffsetStartInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			BitOffsetInByte =
				(OffsetStartInRegion / _pBitmap->PerSectorSize) % _pBitmap->Bits_Of_Byte;

				if (*(*(_pBitmap->bitmap + RegionStart) + OffsetStartInRegion) & BitMask[BitOffsetInByte])
				{
					Result |= BITS_ALL_SET;
				}
				else
				{
					Result |= BITS_ALL_CLEAR;
				}

			//如果是混合型的就早点退出循环
			if (Result == BITS_BLEND)
				break;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Result = STATUS_UNSUCCESSFUL;
	}

	return	Result;
}


NTSTATUS	_BitmapGet(
	IN	PBITMAP	_pBitmap ,
	IN	LARGE_INTEGER	_liOffset ,
	IN	ULONG	_Length ,
	IN OUT	PVOID	_pInOutBuffer ,
	IN	PVOID	_pInBuffer
)
{
	KdPrint( ("[_BitmapGet]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	ULONG	RegionStart = 0;
	ULONG	OffsetStartInRegion = 0;
	ULONG	ByteStartInRegion = 0;
	ULONG	BitOffsetInByte = 0;
	ULONG	i = 0;


	__try
	{
		//检查参数
		if (_pBitmap == NULL || _liOffset.QuadPart < 0 ||
			_pInOutBuffer == NULL || _pInBuffer == NULL)
		{
			KdPrint( ("_BitmapGet: Invalid Parameters\n") );
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		if (_liOffset.QuadPart % _pBitmap->PerSectorSize != 0 ||
			_Length % _pBitmap->PerSectorSize)
		{
			KdPrint( ("_BitmapGet: Invalid Parameters\n") );
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		//遍历每一位，如果位是1，就要用_pInBuffer参数中相应位置的数据
		//拷贝到_pInOutBuffer中
		for (i = 0; i < _Length; i += _pBitmap->PerSectorSize)
		{
			RegionStart =
				(_liOffset.QuadPart + i) / _pBitmap->PerRegionSizeMapped;
			OffsetStartInRegion = 
				(_liOffset.QuadPart + i) % _pBitmap->PerRegionSizeMapped;
			ByteStartInRegion =
				OffsetStartInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			BitOffsetInByte=
				(OffsetStartInRegion / _pBitmap->PerSectorSize) % _pBitmap->Bits_Of_Byte;

			if (*(*(_pBitmap->bitmap + RegionStart) + OffsetStartInRegion) & BitMask[BitOffsetInByte])
			{
				memcpy(
					(UCHAR*)_pInOutBuffer + i ,
					(UCHAR*)_pInBuffer + i ,
					_pBitmap->PerSectorSize );
			}
		}

		status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		status = STATUS_UNSUCCESSFUL;
	}

	return	status;
}


NTSTATUS	_ReadWriteDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	//KdPrint( ("[_ReadWriteDispatch]\n") );

	PDEVICE_EXTENSION	pDevExt = _pDeviceObject->DeviceExtension;
	NTSTATUS	status = STATUS_SUCCESS;


	//判断这个过滤设备是否在保护状态
	if (pDevExt->bProtect)
	{
		//首先把这个irp设置为pending状态
		IoMarkIrpPending( _pIrp );

		//然后将这个irp放进请求队列里
		ExInterlockedInsertTailList(
			&pDevExt->RequestList ,
			&_pIrp->Tail.Overlay.ListEntry ,
			&pDevExt->RequestSpinLock );

		//设置队列的等待事件，通知线程处理irp
		KeSetEvent(
			&pDevExt->RequestEvent ,
			0 , FALSE );

		//放回pending状态，这个irp就算处理完了
		return	STATUS_PENDING;
	}
	else
	{
		//这个过滤设备不在保护状态，直接交给下层处理
		IoSkipCurrentIrpStackLocation( _pIrp );
		return	IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );
	}
}


VOID	_ReadWriteThreadEntry(
	IN	PVOID	_Context
)
{
	KdPrint( ("[_ReadWriteThreadEntry]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	PDEVICE_EXTENSION	pDevExt = (PDEVICE_EXTENSION)_Context;
	//请求队列的入口
	PLIST_ENTRY		pRequestEntry = NULL;
	//irp指针
	PIRP	pIrp = NULL;
	PIO_STACK_LOCATION	pIrpStack = NULL;
	PBYTE	pSysBuffer = NULL;
	LARGE_INTEGER	liOffset = { 0 };
	ULONG	Length = 0;
	PBYTE	pTempBuffer = NULL;
	PBYTE	pDeviceBuffer = NULL;
	IO_STATUS_BLOCK		StatusBlock;


	//降低这个线程的优先级
	KeSetPriorityThread(
		KeGetCurrentThread() ,
		LOW_REALTIME_PRIORITY );

	//线程完成irp的部分
	for (;;)
	{
		//等待请求队列同步事件
		//如果队列没有Irp要处理，就在这里等待
		KeWaitForSingleObject(
			&pDevExt->RequestEvent ,
			Executive ,
			KernelMode ,
			FALSE ,
			NULL );

		//如果有了线程结束标志，就自己结束自己
		if (pDevExt->ThreadExitFlag)
		{
			//这是线程的唯一退出点
			PsTerminateSystemThread( STATUS_SUCCESS );
			return;
		}

		//从请求队列首部拿出一个请求处理，使用了自旋锁，不会有冲突
		while
			(
				pRequestEntry = ExInterlockedRemoveHeadList(
					&pDevExt->RequestList ,
					&pDevExt->RequestSpinLock )
			)
		{
			//从队列入口找到实际的irp地址
			pIrp = CONTAINING_RECORD(
				pRequestEntry ,
				IRP ,
				Tail.Overlay.ListEntry );

			//取得irp stack
			pIrpStack = IoGetCurrentIrpStackLocation( pIrp );

			//获取这个irp中包含的缓存地址
			//这个地址可能来自mdl，也可能是其它，取决于目标卷设备的io方式
			if (pIrp->MdlAddress)
				pSysBuffer = (PBYTE)MmGetSystemAddressForMdlSafe(
				pIrp->MdlAddress ,
				NormalPagePriority );
			else
				pSysBuffer = (PBYTE)pIrp->UserBuffer;

			switch (pIrpStack->MajorFunction)
			{
				case IRP_MJ_READ:
					{
						liOffset =
							pIrpStack->Parameters.Read.ByteOffset;
						Length = 
							pIrpStack->Parameters.Read.Length;

						//首先根据bitmap来判断这个读操作读取的范围
						//是全部为转储空间，还是全部不为转储空间，还是兼而有之
						//通过调用_BitmapTest实现
						LONG	TestResult = _BitmapTest(
							pDevExt->pBitmap ,
							liOffset ,
							Length );
						//判断test的结果
						switch (TestResult)
						{
							case BITS_ALL_CLEAR:
								//说明这次读取的操作全部是读取未转储的空间
								//直接下发给下层设备
								goto	SEND_NEXT;

							case BITS_ALL_SET:
								//说明这次操作读取的全部是已经转储的空间
								//直接从缓冲文件中读取出来，然后完成这个irp
								//分配一个缓冲区来从缓冲文件中读取
								pTempBuffer = (PBYTE)ExAllocatePoolWithTag(
									NonPagedPool ,
									Length ,
									'hqsb' );
								if (pTempBuffer == NULL)
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}

								RtlZeroMemory( pTempBuffer , Length );

								status = ZwReadFile(
									pDevExt->hTempFile ,
									NULL ,
									NULL ,
									NULL ,
									&StatusBlock ,
									pTempBuffer ,
									Length ,
									&liOffset ,
									NULL );
								if (!NT_SUCCESS( status ))
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}
								pIrp->IoStatus.Information = Length;
								RtlCopyMemory(
									pSysBuffer ,
									pTempBuffer ,
									pIrp->IoStatus.Information );
								goto	COMPLETE;

							case BITS_BLEND:
								//说明这次读取的操作是混合的
								//即要从下层设备中读出，也要从缓冲文件读出,然后合并
								//分配一个缓冲区来从缓冲文件中读取
								pTempBuffer = (PBYTE)ExAllocatePoolWithTag(
									NonPagedPool ,
									Length ,
									'hqsb' );
								if (pTempBuffer == NULL)
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}
								RtlZeroMemory( pTempBuffer , Length );

								//分配一个缓冲区从下层设备读取
								pDeviceBuffer = (PBYTE)ExAllocatePoolWithTag(
									NonPagedPool ,
									Length ,
									'hqsb' );
								if (pDeviceBuffer == NULL)
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}
								RtlZeroMemory( pDeviceBuffer , Length );

								//读取同等大小的缓冲区中的数据
								status = ZwReadFile(
									pDevExt->hTempFile ,
									NULL ,
									NULL ,
									NULL ,
									&StatusBlock ,
									pTempBuffer ,
									Length ,
									Length ,
									&liOffset ,
									NULL );
								if (!NT_SUCCESS( status ))
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}

								//将这个Irp发送给下层设备获取同等大小的数据
								status = _ForwardIrpSync( pDevExt->pLowerDeviceObject , pIrp );
								if (!NT_SUCCESS( status ))
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}
								//下层设备获取到的数据保存在SysBuffer中,
								//实际长度是information的值
								memcpy(
									pDeviceBuffer ,
									pSysBuffer ,
									pIrp->IoStatus.Information );

								//将两个缓冲区的数据根据bitmap进行混合
								status = _BitmapGet(
									pDevExt->pBitmap ,
									liOffset ,
									Length ,
									pDeviceBuffer ,
									pTempBuffer );
								if (!NT_SUCCESS( status ))
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}

								//合并完的数据存回SysBuffer,并完成irp
								memcpy(
									pSysBuffer ,
									pDeviceBuffer ,
									pIrp->IoStatus.Information );
								goto	COMPLETE;

							default:
								status = STATUS_INSUFFICIENT_RESOURCES;
								goto	ERROR;
						}
					}

				case IRP_MJ_WRITE:
					{
						liOffset =
							pIrpStack->Parameters.Write.ByteOffset;
						Length =
							pIrpStack->Parameters.Write.Length;

						//写的过程，直接写进缓冲文件,
						//之后设置bitma中的位
						status = ZwWriteFile(
							pDevExt->hTempFile ,
							NULL ,
							NULL ,
							NULL ,
							&StatusBlock ,
							pSysBuffer ,
							Length ,
							&liOffset ,
							NULL );
						if (!NT_SUCCESS( status ))
						{
							status = STATUS_INSUFFICIENT_RESOURCES;
							goto	ERROR;
						}

						//设置bitmap中相应的位
						status = _BitmapSet(
							pDevExt->pBitmap ,
							liOffset ,
							Length );
						if (!NT_SUCCESS( status ))
						{
							status = STATUS_INSUFFICIENT_RESOURCES;
							goto	ERROR;
						}
						else
						{
							goto	COMPLETE;
						}
					}

				default:
					//是其他irp请求，没必要处理，直接下发
					goto	SEND_NEXT;
			}

		ERROR:
			if (pTempBuffer)
			{
				ExFreePoolWithTag( pTempBuffer , 'hqsb' );
				pTempBuffer = NULL;
			}
			if (pDeviceBuffer)
			{
				ExFreePoolWithTag( pDeviceBuffer , 'hqsb' );
				pDeviceBuffer = NULL;
			}

			//完成该Irp
			pIrp->IoStatus.Status = status;
			IoCompleteRequest( pIrp , IO_NO_INCREMENT );

			continue;

		SEND_NEXT:
			if (pTempBuffer)
			{
				ExFreePoolWithTag( pTempBuffer , 'hqsb' );
				pTempBuffer = NULL;
			}
			if (pDeviceBuffer)
			{
				ExFreePoolWithTag( pDeviceBuffer , 'hqsb' );
				pDeviceBuffer = NULL;
			}

			IoSkipCurrentIrpStackLocation( pIrp );
			IoCallDriver( pDevExt->pLowerDeviceObject , pIrp );

			continue;

		COMPLETE:
			if (pTempBuffer)
			{
				ExFreePoolWithTag( pTempBuffer , 'hqsb' );
				pTempBuffer = NULL;
			}
			if (pDeviceBuffer)
			{
				ExFreePoolWithTag( pDeviceBuffer , 'hqsb' );
				pDeviceBuffer = NULL;
			}

			pIrp->IoStatus.Status = STATUS_SUCCESS;
			IoCompleteRequest( pIrp , IO_DISK_INCREMENT );

			continue;
		}
	}
}