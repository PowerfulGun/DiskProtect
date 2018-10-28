#include	<ntifs.h>
#include	<windef.h>
#include	<mountmgr.h>
#include	<mountdev.h>
#include	<ntddvol.h>
#include	<ntstrsafe.h>
#include	"DiskProtect.h"


//���ҵ���Ҫ�����ľ��豸ʱ������ָ��ָ��������豸���豸��չ
PDEVICE_EXTENSION	g_pFilterDevExt = NULL;


NTSTATUS	DriverEntry(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PUNICODE_STRING	_pRegistryPath
)
{
	KdPrint( ("[DriverEntry\n]") );

	ULONG	i;	//������ѭ�����Ʊ���

	for ( i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
	{
		//��ʼ������������еķַ�����
		_pDriverObject->MajorFunction[i] = _DefaultDispatch;
	}
	//������ķַ��������¸�ֵΪ�Լ��ĺ���
	_pDriverObject->MajorFunction[IRP_MJ_POWER] = _PowerDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_PNP] = _PnpDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = _DeviceControlDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_READ] = _ReadWriteDispatch;
	_pDriverObject->MajorFunction[IRP_MJ_WRITE] = _ReadWriteDispatch;
	//����������AddDevice����
	_pDriverObject->DriverExtension->AddDevice = _AddDevice;
	//����������Unload����
	_pDriverObject->DriverUnload = _UnloadDispatch;

	//ע��һ��boot���������ص�������ص�������������boot����������Ϻ�ִ��
	IoRegisterBootDriverReinitialization(
		_pDriverObject ,
		_BootReinitializationRoutine ,
		NULL );

	//���سɹ�
	return	STATUS_SUCCESS;
}

//�����µĴ��̾��豸����ʱ��pnp����������AddDevice����
NTSTATUS	_AddDevice(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PDEVICE_OBJECT	_pPhysicalDeviceObject
)
{
	KdPrint( ("[_AddDevice]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	//����ָ������豸���豸��չ��ָ��
	PDEVICE_EXTENSION	pFilterDevExt = NULL;
	//�²��豸��ָ��
	PDEVICE_OBJECT	pLowerDeviceObject = NULL;
	//�����豸��ָ��
	PDEVICE_OBJECT	pFilterDeviceObject = NULL;
	//�����豸�Ĵ����߳̾��
	HANDLE	hThreadHandle = NULL;
	
	__try
	{
	//����һ�������豸������豸ʱFILE DEVICE DISK���͵��豸�����Զ�����豸��չ
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
		//���ù����豸��չָ��
		pFilterDevExt = pFilterDeviceObject->DeviceExtension;
		//��չ����豸���豸��չ
		RtlZeroMemory( pFilterDevExt , sizeof( DEVICE_EXTENSION ) );
		//�������豸���ӵ����豸�������豸��
		pLowerDeviceObject = IoAttachDeviceToDeviceStack( pFilterDeviceObject , _pPhysicalDeviceObject );
		if (pLowerDeviceObject == NULL)
		{
			KdPrint( ("_AddDeivce.ioAttachDeviceToDeviceStack fail ,status = %u" , status) );
			status = STATUS_NO_SUCH_DEVICE;
			__leave;
		}
		//��ʼ��������豸�ķ�ҳ·�������¼�
		KeInitializeEvent(
			&pFilterDevExt->PagingPathCountEvent ,
			SynchronizationEvent ,
			TRUE );
		//�����豸������Ӧ�ú��²��豸��ͬ
		pFilterDeviceObject->Flags = pLowerDeviceObject->Flags;
		//�������豸���ϵ�Դ�ɷ�ҳ������
		pFilterDeviceObject->Flags |= DO_POWER_PAGABLE;
		//�����豸��ʼ�����
		pFilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

		//���ù����豸�豸��չ�е���Ӧ��Ա
		pFilterDevExt->pFilterDeviceObject = pFilterDeviceObject;
		pFilterDevExt->pPhysicalDeviceObject = _pPhysicalDeviceObject;
		pFilterDevExt->pLowerDeviceObject = pLowerDeviceObject;
		//��ʼ�����������
		InitializeListHead( &pFilterDevExt->RequestList );
		//��ʼ����������е�������
		KeInitializeSpinLock( &pFilterDevExt->RequestSpinLock );
		//��ʼ��������е�ͬ���¼�
		KeInitializeEvent(
			&pFilterDevExt->RequestEvent ,
			SynchronizationEvent ,
			FALSE );
		//��ʼ���߳���ֹ��־
		pFilterDevExt->ThreadExitFlag = FALSE;
		//��������������豸������̣߳�����Ĳ������豸��չ
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
		//��ȡ�����̵߳��̶߳���
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
		//����в��ɹ��ĵط�����Ҫ������ܴ��ڵ��豸����
		if (pLowerDeviceObject != NULL)
		{
			IoDetachDevice( pLowerDeviceObject );
			pFilterDevExt->pLowerDeviceObject = NULL;
		}
		//ɾ�����ܽ����Ĺ����豸
		if (pFilterDeviceObject != NULL)
		{
			IoDeleteDevice( pFilterDeviceObject );
			pFilterDevExt->pFilterDeviceObject = NULL;
		}
	}

	//�ر��߳̾�������ж��̵߳����ö�ͨ���̶߳������
	if (hThreadHandle != NULL)
	{
		ZwClose( hThreadHandle );
	}
	return	status;
}

//pnp����Ĵ���
NTSTATUS	_PnpDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
)
{
	KdPrint( ("[_PnpDispatch]\n") );

	NTSTATUS	status = STATUS_SUCCESS;
	//��ȡ�豸��չ
	PDEVICE_EXTENSION	pDevExt = _pDeviceObject->DeviceExtension;
	//��ȡ��ǰIRPջ��Ԫ
	PIO_STACK_LOCATION	pIrpStack = IoGetCurrentIrpStackLocation( _pIrp );

	switch (pIrpStack->MinorFunction)
	{
		case IRP_MN_REMOVE_DEVICE:
			//���pnp�����������������Ƴ��豸��irp�����������ﴦ��,��һЩ������
			{
				//��������̻߳������У���Ҫ֪ͨ��ֹͣ
				if (pDevExt->ThreadExitFlag != TRUE && pDevExt->hThreadHandle != NULL)
				{
					pDevExt->ThreadExitFlag = TRUE;
					KeSetEvent( &pDevExt->RequestEvent , 0 , FALSE );
					//�ȴ��߳̽���
					KeWaitForSingleObject(
						pDevExt->hThreadHandle ,	//�߳̾��Ҳ���Ե���ͬ�����󱻵ȴ�
						Executive ,
						KernelMode ,
						FALSE ,
						NULL );	//���õȴ��������ó�ʱ
					//����̶߳�������
					ObDereferenceObject( pDevExt->hThreadHandle );
				}
				//�������λͼҪ�ͷ�
				if (pDevExt->pBitmap)
				{
					_BitmapFree( pDevExt->pBitmap );
				}
				//��������²��豸��Ҫȥ������
				if (pDevExt->pLowerDeviceObject)
				{
					IoDetachDevice( pDevExt->pLowerDeviceObject );
				}
				//������ڹ����豸����Ҫɾ��
				if (pDevExt->pFilterDeviceObject)
				{
					IoDeleteDevice( pDevExt->pFilterDeviceObject );
				}

				//�·����²��豸������break
				break;
			}

		case IRP_MN_DEVICE_USAGE_NOTIFICATION:
			//�ڶ���Ҫ��������豸ʹ��ͨ������Windows�ڽ�������ɾ�������ļ���ʱ�����洢�豸�������iRP���󣬹����豸��ȻҲ��
			//���ܵ��������ļ����� ҳ���ļ��������ļ���dump�ļ�
			//IrpStack�е�Parameters UsageNotification Type���˵���������ļ�
			//IrpStack�е�Parameters UsageNotification InPath��˵�����������ѯ���豸�Ƿ���Խ�������ļ���������ɾ��������ļ�
			//֮�������豸��֪ͨ.
			//�����ȽϹ��ĵ��Ƕ�ҳ���ļ��Ĵ��������ҳ���ļ��ھ��ϣ���ôӦ����������豸��DO_POWER_PAGABLEλ����֮�������λ
			{
				BOOLEAN	SetPagable;

				//�����ѯ���Ƿ�֧�������ļ���dump�ļ�����ֱ���·����²��豸ȥ����
				if (pIrpStack->Parameters.UsageNotification.Type != DeviceUsageTypePaging)
				{
					//������ǰirpstack
					IoSkipCurrentIrpStackLocation( _pIrp );
					status = IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );
					return	status;
				}

				//Ϊ��ͬ������ҳ���ļ�������������Ҫ�ȴ���ҳ�����¼�
				status = KeWaitForSingleObject(
					&pDevExt->PagingPathCountEvent ,
					Executive ,
					KernelMode ,
					FALSE ,
					NULL );
				//SetPageble��ʼ��Ϊ�٣�û�����ù�DO_POWER_PAGABLEλ
				SetPagable = FALSE;

				//���InPath��Ϊ�٣�����Pnp������֪ͨ����ɾȥ��ҳ�ļ������ҵ�Ŀǰֻʣ�����һ����ҳ�ļ���ʱ��
				if (!(pIrpStack->Parameters.UsageNotification.InPath) && pDevExt->PagingPathCount == 1)
				{
					//Windows Vista֮��İ汾�豸����ı�־λ�� DO_POWER_INRUSH��DO_POWER_PAGABLE����ͬʱ���ڣ�����Ҫ�ж�
					//if(_pDeviceObject->Flags & DO_POWER_INRUSH)
					//˵��û�з�ҳ�ļ�������豸���ˣ���Ҫ����DO_POWER_PAGABLEλ
					_pDeviceObject->Flags |= DO_POWER_PAGABLE;
					SetPagable = TRUE;

				}

				//��������ǹ��ڷ�ҳ�ļ����Ƿ���Խ�����ѯ ����ɾ����֪ͨ�������²��豸����
				//��Ҫͬ����ʽ���͸��²��豸���ȴ��䷵��
				status = _ForwardIrpSync( pDevExt->pLowerDeviceObject , _pIrp );
				if (NT_SUCCESS( status ))
				{
					//��������²������ɹ��ˣ�˵���²��豸֧�����������
					//�ڳɹ�����������Ҫ�ı����ֵ����¼��������豸�ϵ����ж��ٸ���ҳ�ļ�
					IoAdjustPagingPathCount(
						&pDevExt->PagingPathCount ,
						pIrpStack->Parameters.UsageNotification.InPath );

					if (pIrpStack->Parameters.UsageNotification.InPath && pDevExt->PagingPathCount == 1)
					{
						//������������һ��������ҳ�ļ��Ĳ�ѯ���󣬲����²��豸֧�֣��������ǵ�һ��������豸�ϵķ�ҳ�ļ���
						//����Ҫ��������豸��DO_POWER_PAGABLEλ
						_pDeviceObject->Flags &= DO_POWER_PAGABLE;
					}
				}
				else
				{
					//������˵���²㲻֧�����������Ҫ��֮ǰ�����Ĳ�����ԭ�����DO_POWER_PAGABLEλ
					if (SetPagable == TRUE)
					{
						_pDeviceObject->Flags &= DO_POWER_PAGABLE;
						SetPagable = FALSE;
					}
				}

				//���÷�ҳ�����¼�Ϊ���źţ���һ���ȴ��Ĳ����Ż�ִ�У����ڴ��в���
				KeSetEvent(
					&pDevExt->PagingPathCountEvent ,
					IO_NO_INCREMENT ,
					FALSE );

				//���Irp����
				IoCompleteRequest( _pIrp , IO_NO_INCREMENT );
				return	status;
			}

		default:
			//������pnp����ֱ���·����²��豸
			break;
	}

	//�·����²��豸
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
	//���ڵȴ����¼�
	KEVENT	WaitEvent;


	KeInitializeEvent(
		&WaitEvent ,
		NotificationEvent ,
		FALSE );

	//Ϊ�²㴴��ioջ��Ԫ,ѡ�ø��Ʋ���
	IoCopyCurrentIrpStackLocationToNext( _pIrp );
	//�������irp���������²㴦����irpʱ�ص�
	IoSetCompletionRoutine(
		_pIrp ,
		_PnpIrpCompletionRoutine ,
		&WaitEvent ,
		TRUE ,
		TRUE ,
		TRUE );

	//��Irp���͵��²�
	status = IoCallDriver( _pDeviceObject , _pIrp );
	if (status == STATUS_PENDING)
	{
		//�ȴ��¼�
		KeWaitForSingleObject(
			&WaitEvent ,
			Executive ,
			KernelMode ,
			NULL ,
			NULL );

		//����֮���ȡirp����״̬
		status = _pIrp->IoStatus.Status;
	}

	//����״̬
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

	//���õȴ�����Ϊ���źţ�ʹ�ȴ����Ľ��̼������У�_ForwardIrpSync�еĴ���
	//����������������ɸ�Irp�����Է���STATUS_MORE_PROCESSING_REQUIRED
	return	STATUS_MORE_PROCESSING_REQUIRED;
}


//��Դ���������Windows Vista��ǰ�İ汾����Ҫʹ�����⺯���·���Irp�������֮��İ汾������ֱ���·����ײ��豸
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


//��Ҫ��DeviceIoControl�л�ȡIOCTL_VOLUME_ONLINE�������ɲ���ϵͳ�����ģ�Ŀ����豸ֻ�н��ܵ�������Żᴦ������״̬�����ܻ�ȡ�����
//���·���Ŀ����豸���ڶ�ȡ�����
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
	//��������IOCTL_VOLUME_ONLINE����ɺ�����������
	VOLUME_ONLINE_CONTEXT	Context;


	switch (pIrpStack->Parameters.DeviceIoControl.IoControlCode)
	{
		case IOCTL_VOLUME_ONLINE:
			{
				//�ȳ�ʼ��һ���¼���ͬ��
				KeInitializeEvent( &WaitEvent , NotificationEvent , FALSE );

				//��ʼ��������ɺ����Ĳ���
				Context.pDevExt = pDevExt;
				Context.pWaitEvent = &WaitEvent;

				IoCopyCurrentIrpStackLocationToNext( _pIrp );
				//������ɺ���
				IoSetCompletionRoutine(
					_pIrp ,
					_VolumeOnlineCompletionRoutine ,
					&Context ,
					TRUE ,
					TRUE ,
					TRUE );

				//�����²��豸
				status = IoCallDriver( pDevExt->pLowerDeviceObject , _pIrp );

				//�ȴ��²��豸�������irp
				KeWaitForSingleObject(
					&WaitEvent ,
					Executive ,
					KernelMode ,
					FALSE ,
					NULL );

				return	status;
			}

		default:
			//����DeviceIoControl��һ�ɷ��͸��²��豸
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
	//�������̷�
	UNICODE_STRING	DiskName = { 0 };


	ASSERT( _pVolumeContext != NULL );

	__try
	{
		//���ú�����ȡ�����豸�����֣������̷�
		status = IoVolumeDeviceToDosName( _pVolumeContext->pDevExt->pPhysicalDeviceObject , &DiskName );
		if (!NT_SUCCESS( status ))
			__leave;

		KdPrint( ("The current disk name is %C" , DiskName.Buffer[0]) );

		_pVolumeContext->pDevExt->VolumeLetter = DiskName.Buffer[0];

		////�����Ʊ�ɴ�д
		//if (_pVolumeContext->pDevExt->VolumeLetter > L'Z')
		//	_pVolumeContext->pDevExt->VolumeLetter -= (L'a' - L'A');

		//ֻ����D��
		if (_pVolumeContext->pDevExt->VolumeLetter == L'D' || _pVolumeContext->pDevExt->VolumeLetter == L'd')
		{
			//��ȡ�����Ļ�����Ϣ
			status = _QueryVolumeInformation(
				_pVolumeContext->pDevExt->pPhysicalDeviceObject ,
				&(_pVolumeContext->pDevExt->liVolumeTotalSize) ,
				&(_pVolumeContext->pDevExt->dwPerClusterSize) ,
				&(_pVolumeContext->pDevExt->dwPerSectorSize) );
			if (!NT_SUCCESS( status ))
				__leave;

			//����������Ӧ��λͼ�ṹ
			status = _BitmapInit(
				&(_pVolumeContext->pDevExt->pBitmap) ,
				_pVolumeContext->pDevExt->dwPerSectorSize ,
				8 ,
				25600 ,
				(DWORD)(_pVolumeContext->pDevExt->liVolumeTotalSize.QuadPart /
				(LONGLONG)(25600 * 8 * _pVolumeContext->pDevExt->dwPerSectorSize)) + 1 );
			if (!NT_SUCCESS( status ))
				__leave;

			//�ҵ�Ҫ�������豸�ˣ���ֵ��ȫ�ֱ���
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

	//���õȴ�ͬ���¼�����DeviceIoControl����ִ��
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
	//ʲô�����������������е��ػ�
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

#define	FAT16_SIG_OFFSET	54	//����FAT16�ļ�ϵͳǩ����ƫ����
#define	FAT32_SIG_OFFSET	82	//FAT32�ļ�ϵͳǩ����ƫ����
#define	NTFS_SIG_OFFSET		3	//NTFS�ļ�ϵͳǩ����ƫ����

	//FAT16�ļ�ϵͳ��־
	UCHAR	FAT16Flag[4] = { 'F','A','T','1' };
	//FAT32�ļ�ϵͳ��־
	UCHAR	FAT32Flag[4] = { 'F','A','T','3' };
	//NTFS�ļ�ϵͳ��־
	UCHAR	NTFSFlag[4] = { 'N','T','F','S' };

	NTSTATUS	status = STATUS_SUCCESS;
	//������ȡDBR���������ݻ�����
	BYTE	DBR[512] = { 0 };
	ULONG	DBRSize = 512;

	//��������ָ��ͳһָ��DBR������
	//���Ƿֱ����NTFS��FAT32��FAT16��DBR�ṹ
	PNTFS_BOOT_SECTOR	pNtfsBootSector =
		(PNTFS_BOOT_SECTOR)DBR;
	PFAT32_BOOT_SECTOR	pFat32BootSector =
		(PFAT32_BOOT_SECTOR)DBR;
	PFAT16_BOOT_SECTOR	pFat16BootSector =
		(PFAT16_BOOT_SECTOR)DBR;

	//��ȡ��ƫ����������DBR��˵�Ǿ����ʼλ�ã�ƫ����Ϊ0
	LARGE_INTEGER	liReadOffset = { 0 };
	//��¼IO����״̬
	IO_STATUS_BLOCK	IoStatusBlock;

	//Ϊ��ͬ����ȡ���õ�ͬ���¼�
	KEVENT	WaitEvent;
	//Ҫ������IRPָ��
	PIRP	pIrp = NULL;


	__try
	{
		//�����������ȴ�ָ���ľ��豸�϶�ȡƫ����Ϊ0��һ��������Ҳ����������DBR������׼�����Է���
		//��Ϊ����Ҫͬ����ȡ�������ȳ�ʼ��һ��Ϊ��ͬ����ȡ���õ��¼�
		KeInitializeEvent( &WaitEvent , NotificationEvent , FALSE );

		//����һ��irp�����������豸��ȡ��Ϣ
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

		//������ɺ�������ͬ���¼���Ϊ��������
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
			//����²��豸һʱ��ɲ��˾͵ȵ�
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
			//ͨ���Ƚϱ�־�����������һ��ntfs�ļ�ϵͳ�ľ��������ntfs���DBR�������Ը�����Ҫ��ȡ��ֵ���и�ֵ����
			*_pdwPerSectorSize = (DWORD)(pNtfsBootSector->BytesPerSector);
			*_pdwPerClusterSize = (*_pdwPerSectorSize) * (DWORD)(pNtfsBootSector->SectorsPerCluster);
			_liTotalSize->QuadPart = (LONGLONG)(*_pdwPerSectorSize) * (LONGLONG)pNtfsBootSector->TotalSectors;
		}
		else if (*(DWORD*)FAT32Flag == *(DWORD*)&DBR[FAT32_SIG_OFFSET])
		{
			//ͨ���Ƚϱ�־�����������һ��ntfs�ļ�ϵͳ�ľ��������ntfs���DBR�������Ը�����Ҫ��ȡ��ֵ���и�ֵ����
			*_pdwPerSectorSize = (DWORD)(pFat32BootSector->BytesPerSector);
			*_pdwPerClusterSize = (*_pdwPerSectorSize) * (DWORD)(pFat32BootSector->SectorsPerCluster);
			_liTotalSize->QuadPart = (LONGLONG)(*_pdwPerSectorSize) *
				(LONGLONG)(pFat32BootSector->LargeSectors + pFat32BootSector->Sectors);
		}
		else if (*(DWORD*)FAT16Flag == *(DWORD*)&DBR[FAT16_SIG_OFFSET])
		{
			//ͨ���Ƚϱ�־�����������һ��ntfs�ļ�ϵͳ�ľ��������ntfs���DBR�������Ը�����Ҫ��ȡ��ֵ���и�ֵ����
			*_pdwPerSectorSize = (DWORD)(pFat16BootSector->BytesPerSector);
			*_pdwPerClusterSize = (*_pdwPerSectorSize) * (DWORD)(pFat16BootSector->SectorsPerCluster);
			_liTotalSize->QuadPart = (LONGLONG)(*_pdwPerSectorSize) *
				(LONGLONG)(pFat16BootSector->LargeSectors + pFat16BootSector->Sectors);
		}
		else
		{
			//�ߵ���������������κ��ļ�ϵͳ�����ǲ���windows��ʶ���ļ�ϵͳ������ͳһ���ش�
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
	
	//��Irp��Ҫ���ͷ�,Ҫ�ٴλ�ò���
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
	//E�̵�ϡ���ļ���
	WCHAR	SparseFileName[] = L"\\??\\E:\\temp.dat";
	UNICODE_STRING	strSparseFileName;
	IO_STATUS_BLOCK	IoStatusBlock = { 0 };
	//�����ļ�ʱ�Ķ������Ա���
	OBJECT_ATTRIBUTES	ObjectAttr = { 0 };
	//�����ļ���С��ʱ��ʹ�õ��ļ���β������
	FILE_END_OF_FILE_INFORMATION	FileEndInfo = { 0 };


	__try
	{
		//��ʼ��Ҫ�򿪵��ļ���
		RtlInitUnicodeString( &strSparseFileName , SparseFileName );
		//��ʼ���ļ�����Ӧ�Ķ�������������Ҫ�����ʼ��Ϊ�ں˶��󣬲��Ҵ�Сд������
		InitializeObjectAttributes(
			&ObjectAttr ,
			&strSparseFileName ,
			OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE ,
			NULL ,
			NULL );

		//�����ļ���������Ҫע����ǣ�Ҫ����FILE_NO_INTERMEDIATE_BUFFERINGѡ������ļ�ϵͳ�ٻ�������ļ�
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

		//��������ļ�Ϊϡ���ļ�
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

		//��������ļ���СΪD�̵Ĵ�С��������10m�ı����ռ�
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

		//����ɹ���ʼ���ͽ������ı�����־����Ϊ�ڱ���״̬
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


	//�������Ϸ�
	if (_ppBitmap == NULL || _PerSectorSize == 0 ||
		_Bits_Of_Byte == 0 || _Bytes_Of_Region == 0 ||
		_RegionNumber == 0)
	{
		return	STATUS_UNSUCCESSFUL;
	}

	__try
	{
		//����һ��Bitmap�ṹ������һ��Ҫ����ģ��൱��һ��handle
		pBitmap = ExAllocatePoolWithTag(
			NonPagedPool ,
			sizeof( BITMAP ) ,
			'hqsb' );
		if (pBitmap == NULL)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}
		//��սṹ
		RtlZeroMemory( pBitmap , sizeof( BITMAP ) );

		//���ݲ����Խṹ�еĳ�Ա���и�ֵ
		pBitmap->PerSectorSize = _PerSectorSize;
		pBitmap->Bits_Of_Byte = _Bits_Of_Byte;
		pBitmap->Bytes_Of_Region = _Bytes_Of_Region;
		pBitmap->RegionNumber = _RegionNumber;
		pBitmap->PerRegionSizeMapped =
			_Bytes_Of_Region * _Bits_Of_Byte * _PerSectorSize;
		pBitmap->BitmapSizeMapped =
			pBitmap->PerRegionSizeMapped * _RegionNumber;

		//����һ��ָ�����飬��RegionNumber��Ԫ��
		pBitmap->bitmap = ExAllocatePoolWithTag(
			NonPagedPool ,
			sizeof( PUCHAR ) * _RegionNumber ,
			'hqsb' );
		if (pBitmap->bitmap == NULL)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}
		//���ָ������
		RtlZeroMemory( pBitmap->bitmap , sizeof( PUCHAR )*_RegionNumber );

		//������õ�Bitmap�ش���������
		*_ppBitmap = pBitmap;
		status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		status = STATUS_UNSUCCESSFUL;
	}

	//����в���ʧ�ܣ�Ҫ�ͷ���Դ
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


//�����ͷ�Bitmapռ�õ��ڴ�ĺ���
VOID	_BitmapFree(
	IN	PBITMAP	_pBitmap
)
{
	KdPrint( ("[_BitmapFree]\n") );

	ULONG	i;


	if (_pBitmap != NULL)
	{
		//���ͷŽṹ��bitmapռ�õ��ڴ�
		if (_pBitmap->bitmap != NULL)
		{
			//����ָ�������е�ÿ��Ԫ��
			for (i = 0; i < _pBitmap->RegionNumber; i++)
			{
				//������Ԫ����ֵ��������ָ��һ���ڴ棬����Ҫ�ͷ�
				if (*(_pBitmap->bitmap + i) != NULL)
				{
					ExFreePoolWithTag(
						*(_pBitmap->bitmap + i) ,
						'hqsb' );
				}
			}

			//�ͷ���ÿ��Ԫ�ص����ݣ���Ҫ�ͷ����ָ������ռ�õ��ڴ�
			ExFreePoolWithTag(
				_pBitmap->bitmap ,
				'hqsb' );
		}

		//�ͷ���ṹ�е�bitmap�����ͷ������ṹ��ռ�õ��ڴ�
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
		//������
		if (_pBitmap == NULL || _liOffset.QuadPart < 0)
		{
			KdPrint( ("_BitmapSet:\n\t\
				The _pBitmap or Offset is invalid") );
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		//�����Ĵ�С�����������ı���
		if (_liOffset.QuadPart%_pBitmap->PerSectorSize != 0 ||
			_Length%_pBitmap->PerSectorSize != 0)
		{
			KdPrint( ("_BitmapSet:\n\t\
				The Offset or Length is invalid") );
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		//����Ҫ���õ�ƫ�����ͳ���������Ҫʹ����ЩRegion��
		//�����Ҫ�Ļ����ͷ���ָ��Ŀռ�
		RegionStart = _liOffset.QuadPart / _pBitmap->PerRegionSizeMapped;
		RegionEnd = (_liOffset.QuadPart + _Length) / _pBitmap->PerRegionSizeMapped;
		for (i = RegionStart; i <= RegionEnd; ++i)
		{
			//ָ��Ϊ�մ���û�з����ڴ�,Ҫ�ȷ����ڴ�
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

		//��ʼ����bitmap���������е�λ��1
		//�Ƚ�Ҫ���õ�������byte���룬û���ֽڶ�����������ֹ���λ����
		for (i = _liOffset.QuadPart; i < _liOffset.QuadPart + _Length; i += _pBitmap->PerSectorSize)
		{
			//�����ʼ�飬��ָ��������Ԫ�ص�λ��
			RegionStart = i / _pBitmap->PerRegionSizeMapped;
			//���µ��ڿ��е�ӳ���ƫ�ƣ��������ֵ��������ڿ��е���ʼ�ֽ�
			OffsetStartInRegion = i % _pBitmap->PerRegionSizeMapped;
			//����Ҫ���õ��ڿ��е���ʼ�ֽ�λ��
			ByteStartInRegion =
				OffsetStartInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			//����Ҫ���õ���ʼλ��λ�ã����λ������ʼ�ֽڵ���
			BitOffsetInByte =
				(OffsetStartInRegion / _pBitmap->PerSectorSize) % _pBitmap->Bits_Of_Byte;

			if (BitOffsetInByte == 0)
			{
				//�����ʼλ��λ����0��
				//����Ҫ���õ���ʼλ���ǰ����ֽڶ����
				//�����ֹ�����
				//Ҫ���ֽ����õ���ʼλ�þ���iֵ
				SetStart.QuadPart = i;	
				break;
			}
			//�ҵ���ʼλ���ֽ��е�ƫ���ˣ�Ҫ�ֹ��������λ
			*(*(_pBitmap->bitmap + RegionStart) + ByteStartInRegion)
				|= BitMask[BitOffsetInByte];

			//������һ��ѭ�����˳�ѭ����2�����
			//ֱ��BitOffsetInByteΪ0�����ҵ��ֽڶ����λ��
			//����i��ֵ����Offset + Length�����ܹ�Ҫ���õĶ�����һ��
			//�ֽڣ���ȫ���ֹ����ú�
		}
		//���i���ڴ���offset + Length�����Ѿ�ȫ���ֹ��������
		//���Է�����
		if (i >= _liOffset.QuadPart + _Length)
		{
			KdPrint( ("The set bits are less than one byte ,\
				had set by manual"));
			status = STATUS_SUCCESS;
			__leave;
		}

		//��Ҫ��֤β��Ҳ�ǰ����ֽڶ����
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

		//�ж��Ƿ��Ѿ�ȫ���������
		if (i < _liOffset.QuadPart ||
			SetEnd.QuadPart == SetStart.QuadPart)
		{
			status = STATUS_SUCCESS;
			__leave;
		}

		//Ҫ���õ�ĩβ�飬��ָ�����������Ľ���λ��
		RegionEnd = SetEnd.QuadPart / _pBitmap->PerRegionSizeMapped;
		//���ֽ�����λ
		for (i = SetStart.QuadPart; i <= SetEnd.QuadPart;)
		{
			RegionStart = i / _pBitmap->PerRegionSizeMapped;
			OffsetStartInRegion =
				i % _pBitmap->PerRegionSizeMapped;
			ByteStartInRegion =
				OffsetStartInRegion / _pBitmap->PerSectorSize / _pBitmap->Bits_Of_Byte;
			//���Ҫ���õ�λû�п�Խ����Region��ֻҪmemset���ֽ�����
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
			//����������õ�λ��Խ������region����Ҫ����i�Ĵ�С
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
		//������
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
			//��Է�Χ�ڵ�bit���в���
			//���ȫ��Ϊ0 ����BITS_ALL_CLEAR
			//���ȫ��Ϊ1 ����BITS_ALL_SET
			//�����0��1��� ����BITS_BLEND
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

			//����ǻ���͵ľ�����˳�ѭ��
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
		//������
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

		//����ÿһλ�����λ��1����Ҫ��_pInBuffer��������Ӧλ�õ�����
		//������_pInOutBuffer��
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


	//�ж���������豸�Ƿ��ڱ���״̬
	if (pDevExt->bProtect)
	{
		//���Ȱ����irp����Ϊpending״̬
		IoMarkIrpPending( _pIrp );

		//Ȼ�����irp�Ž����������
		ExInterlockedInsertTailList(
			&pDevExt->RequestList ,
			&_pIrp->Tail.Overlay.ListEntry ,
			&pDevExt->RequestSpinLock );

		//���ö��еĵȴ��¼���֪ͨ�̴߳���irp
		KeSetEvent(
			&pDevExt->RequestEvent ,
			0 , FALSE );

		//�Ż�pending״̬�����irp���㴦������
		return	STATUS_PENDING;
	}
	else
	{
		//��������豸���ڱ���״̬��ֱ�ӽ����²㴦��
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
	//������е����
	PLIST_ENTRY		pRequestEntry = NULL;
	//irpָ��
	PIRP	pIrp = NULL;
	PIO_STACK_LOCATION	pIrpStack = NULL;
	PBYTE	pSysBuffer = NULL;
	LARGE_INTEGER	liOffset = { 0 };
	ULONG	Length = 0;
	PBYTE	pTempBuffer = NULL;
	PBYTE	pDeviceBuffer = NULL;
	IO_STATUS_BLOCK		StatusBlock;


	//��������̵߳����ȼ�
	KeSetPriorityThread(
		KeGetCurrentThread() ,
		LOW_REALTIME_PRIORITY );

	//�߳����irp�Ĳ���
	for (;;)
	{
		//�ȴ��������ͬ���¼�
		//�������û��IrpҪ������������ȴ�
		KeWaitForSingleObject(
			&pDevExt->RequestEvent ,
			Executive ,
			KernelMode ,
			FALSE ,
			NULL );

		//��������߳̽�����־�����Լ������Լ�
		if (pDevExt->ThreadExitFlag)
		{
			//�����̵߳�Ψһ�˳���
			PsTerminateSystemThread( STATUS_SUCCESS );
			return;
		}

		//����������ײ��ó�һ��������ʹ�����������������г�ͻ
		while
			(
				pRequestEntry = ExInterlockedRemoveHeadList(
					&pDevExt->RequestList ,
					&pDevExt->RequestSpinLock )
			)
		{
			//�Ӷ�������ҵ�ʵ�ʵ�irp��ַ
			pIrp = CONTAINING_RECORD(
				pRequestEntry ,
				IRP ,
				Tail.Overlay.ListEntry );

			//ȡ��irp stack
			pIrpStack = IoGetCurrentIrpStackLocation( pIrp );

			//��ȡ���irp�а����Ļ����ַ
			//�����ַ��������mdl��Ҳ������������ȡ����Ŀ����豸��io��ʽ
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

						//���ȸ���bitmap���ж������������ȡ�ķ�Χ
						//��ȫ��Ϊת���ռ䣬����ȫ����Ϊת���ռ䣬���Ǽ����֮
						//ͨ������_BitmapTestʵ��
						LONG	TestResult = _BitmapTest(
							pDevExt->pBitmap ,
							liOffset ,
							Length );
						//�ж�test�Ľ��
						switch (TestResult)
						{
							case BITS_ALL_CLEAR:
								//˵����ζ�ȡ�Ĳ���ȫ���Ƕ�ȡδת���Ŀռ�
								//ֱ���·����²��豸
								goto	SEND_NEXT;

							case BITS_ALL_SET:
								//˵����β�����ȡ��ȫ�����Ѿ�ת���Ŀռ�
								//ֱ�Ӵӻ����ļ��ж�ȡ������Ȼ��������irp
								//����һ�����������ӻ����ļ��ж�ȡ
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
								//˵����ζ�ȡ�Ĳ����ǻ�ϵ�
								//��Ҫ���²��豸�ж�����ҲҪ�ӻ����ļ�����,Ȼ��ϲ�
								//����һ�����������ӻ����ļ��ж�ȡ
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

								//����һ�����������²��豸��ȡ
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

								//��ȡͬ�ȴ�С�Ļ������е�����
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

								//�����Irp���͸��²��豸��ȡͬ�ȴ�С������
								status = _ForwardIrpSync( pDevExt->pLowerDeviceObject , pIrp );
								if (!NT_SUCCESS( status ))
								{
									status = STATUS_INSUFFICIENT_RESOURCES;
									pIrp->IoStatus.Information = 0;
									goto	ERROR;
								}
								//�²��豸��ȡ�������ݱ�����SysBuffer��,
								//ʵ�ʳ�����information��ֵ
								memcpy(
									pDeviceBuffer ,
									pSysBuffer ,
									pIrp->IoStatus.Information );

								//�����������������ݸ���bitmap���л��
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

								//�ϲ�������ݴ��SysBuffer,�����irp
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

						//д�Ĺ��̣�ֱ��д�������ļ�,
						//֮������bitma�е�λ
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

						//����bitmap����Ӧ��λ
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
					//������irp����û��Ҫ����ֱ���·�
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

			//��ɸ�Irp
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