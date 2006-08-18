/**
 * hub.c - USB driver stack project for Windows NT 4.0
 *
 * Copyright (c) 2002-2004 Zhiming  mypublic99@yahoo.com
 *
 * This program/include file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program/include file is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program (in the main directory of the distribution, the file
 * COPYING); if not, write to the Free Software Foundation,Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "hub.h"
#include "td.h"
#include "debug.h"
#include "umss.h"
//----------------------------------------------------------
//event pool routines
#define crash_machine() \
{ ( ( PUSB_DEV ) 0 )->flags = 0x12345; }

#define hub_if_from_dev( pdEV, pIF ) \
{\
	int i;\
	for( i = 0; i < pdEV->usb_config->if_count; i ++ )\
	{\
		if( pdEV->usb_config->interf[ i ].pusb_if_desc->bInterfaceClass\
			== USB_CLASS_HUB )\
		{\
			break;\
		}\
	}\
\
	if( i < pdEV->usb_config->if_count )\
		pIF = &pdev->usb_config->interf[ i ];\
	else\
		pIF = NULL;\
\
}

#define hub_ext_from_dev( pdEV ) ( ( PHUB2_EXTENSION )pdEV->dev_ext )

#define realloc_buf( pdEV, puRB ) \
{\
	PBYTE data_buf;\
	int i;\
	data_buf = usb_alloc_mem( NonPagedPool, ( pdEV )->desc_buf_size += 1024 );\
	RtlZeroMemory( data_buf, ( pdEV )->desc_buf_size );\
	for( i = 0; i < ( LONG )( puRB )->context; i++ )\
	{\
		data_buf[ i ] = ( pdEV )->desc_buf[ i ];\
	}\
	usb_free_mem( ( pdEV )->desc_buf );\
	( pdEV )->desc_buf = data_buf;\
	( pdEV )->pusb_dev_desc = ( PUSB_DEVICE_DESC )( pdEV )->desc_buf;\
	( puRB )->data_buffer = &data_buf[ ( LONG ) ( puRB )->context ];\
}


BOOL
dev_mgr_set_if_driver(
PUSB_DEV_MANAGER dev_mgr,
DEV_HANDLE if_handle,
PUSB_DRIVER pdriver,
PUSB_DEV pdev	//if pdev != NULL, we use pdev instead if_handle, and must have dev_lock acquired.
)
{
	ULONG i;
	USE_IRQL;

	if( dev_mgr == NULL || if_handle == 0 || pdriver == NULL )
		return FALSE;

	i = if_idx_from_handle( if_handle );
	if( pdev != NULL )
	{
		if( dev_state( pdev ) < USB_DEV_STATE_BEFORE_ZOMB )
		{
			pdev->usb_config->interf[ i ].pif_drv = pdriver;
			return TRUE;
		}
		return FALSE;
	}

	if( usb_query_and_lock_dev( dev_mgr, if_handle, &pdev ) != STATUS_SUCCESS )
		return FALSE;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) != USB_DEV_STATE_ZOMB )
	{
		pdev->usb_config->interf[ i ].pif_drv = pdriver;
	}
	unlock_dev( pdev, TRUE );
	usb_unlock_dev( pdev );
	return TRUE;
}

BOOL
dev_mgr_set_driver(
PUSB_DEV_MANAGER dev_mgr,
DEV_HANDLE dev_handle,
PUSB_DRIVER pdriver,
PUSB_DEV pdev	//if pdev != NULL, we use pdev instead if_handle
)
{
	USE_IRQL;

	if( dev_mgr == NULL || dev_handle == 0 || pdriver == NULL )
		return FALSE;

	if( pdev != NULL )
	{
		if( dev_state( pdev ) < USB_DEV_STATE_BEFORE_ZOMB )
		{
			pdev->dev_driver = pdriver;
			return TRUE;
		}
		return FALSE;
	}

	if( usb_query_and_lock_dev( dev_mgr, dev_handle, &pdev ) != STATUS_SUCCESS )
		return FALSE;

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) < USB_DEV_STATE_BEFORE_ZOMB )
	{
		pdev->dev_driver = pdriver;
	}
	unlock_dev( pdev, FALSE );
	usb_unlock_dev( pdev );

	return TRUE;

}

USB_DRIVER g_driver_list[ DEVMGR_MAX_DRIVERS ];
USB_DEV_MANAGER	g_dev_mgr;
extern ULONG cpu_clock_freq;

BOOL
hub_check_reset_port_status(
PUSB_DEV pdev,
LONG port_idx
);

BOOL
hub_start_next_reset_port(
PUSB_DEV_MANAGER dev_mgr,
BOOL from_dpc
);

VOID
hub_reexamine_port_status_queue(
PUSB_DEV hub_dev,
ULONG port_idx,
BOOL from_dpc
);

void
hub_int_completion(
PURB purb,
PVOID pcontext
);

VOID
hub_get_port_status_completion(
PURB purb,
PVOID context
);

VOID
hub_clear_port_feature_completion(
PURB purb,
PVOID context
);

VOID
hub_event_examine_status_que(
PUSB_DEV pdev,
ULONG event,
ULONG context, //hub_ext
ULONG param	   //port_idx
);

VOID
hub_timer_wait_dev_stable(
PUSB_DEV pdev,
PVOID context   //port-index
);

VOID
hub_event_dev_stable(
PUSB_DEV pdev,
ULONG event,
ULONG context, //hub_ext
ULONG param	   //port_idx
);

VOID
hub_post_esq_event(
PUSB_DEV pdev,
BYTE port_idx,
PROCESS_EVENT pe
);

void
hub_set_cfg_completion(
PURB purb,
PVOID pcontext
);

void
hub_get_hub_desc_completion(
PURB purb,
PVOID pcontext
);

NTSTATUS
hub_start_int_request(
PUSB_DEV pdev
);

BOOL
hub_remove_reset_event(
PUSB_DEV pdev,
ULONG port_idx,
BOOL from_dpc
);

BOOL
hub_driver_init(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver
);

BOOL
hub_driver_destroy(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver
);

BOOL
hub_connect(
PCONNECT_DATA init_param,
DEV_HANDLE dev_handle
);

BOOL
hub_disconnect(
PUSB_DEV_MANAGER dev_mgr,
DEV_HANDLE dev_handle
);

BOOL
hub_stop(
PUSB_DEV_MANAGER dev_mgr,
DEV_HANDLE dev_handle
);

NTSTATUS
hub_disable_port_request(
PUSB_DEV pdev,
UCHAR port_idx
);

VOID
hub_start_reset_port_completion(
PURB purb,
PVOID context
);

BOOL
dev_mgr_start_config_dev(
PUSB_DEV pdev
);

BOOL
dev_mgr_event_init(
PUSB_DEV dev,		//always null. we do not use this param
ULONG event,
ULONG context,
ULONG param
);

VOID
dev_mgr_get_desc_completion(
PURB purb,
PVOID context
);

VOID
dev_mgr_event_select_driver(
PUSB_DEV pdev,
ULONG event,
ULONG context,
ULONG param
);

LONG
dev_mgr_score_driver_for_dev(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver,
PUSB_DEVICE_DESC pdev_desc
);

NTSTATUS
dev_mgr_destroy_usb_config(
PUSB_CONFIGURATION pcfg
);

BOOL
dev_mgr_start_select_driver(
PUSB_DEV pdev
);

VOID
dev_mgr_cancel_irp(
PDEVICE_OBJECT pdev_obj,
PIRP pirp
);

BOOL
init_event_pool(
PUSB_EVENT_POOL pool
)
{
	int i;

	if( pool == NULL )
		return FALSE;

	if( ( pool->event_array = usb_alloc_mem(
			NonPagedPool,
		   	sizeof( USB_EVENT ) * MAX_EVENTS ) )
		== NULL)
		return FALSE;

	InitializeListHead( &pool->free_que );
	KeInitializeSpinLock( &pool->pool_lock );
	pool->total_count = MAX_EVENTS;
	pool->free_count = 0;

	for( i = 0;  i < MAX_EVENTS; i++ )
	{
		free_event( pool, &pool->event_array[ i ] );
	}

	return TRUE;
}

BOOL
free_event(
PUSB_EVENT_POOL pool,
PUSB_EVENT pevent
)
{
	if( pool == NULL || pevent == NULL )
	{
		return FALSE;
	}

	RtlZeroMemory( pevent, sizeof( USB_EVENT ) );
	InsertTailList( &pool->free_que, (PLIST_ENTRY) pevent );
	pool->free_count++;
	usb_dbg_print( DBGLVL_MAXIMUM + 1, ( "free_event(): alloced=0x%x, addr=0x%x\n", MAX_EVENTS - pool->free_count, pevent ) );

	return TRUE;
}

PUSB_EVENT
alloc_event(
PUSB_EVENT_POOL pool,
LONG count
)  //null if failed
{
	PUSB_EVENT new;
	if( pool == NULL || count != 1)
		return NULL;

	if( pool->free_count == 0 )
		return NULL;

	new = ( PUSB_EVENT )RemoveHeadList( &pool->free_que );
    pool->free_count --;

	usb_dbg_print( DBGLVL_MAXIMUM + 1, ( "alloc_event(): alloced=0x%x, addr=0x%x\n", MAX_EVENTS - pool->free_count, new ) );
	return new;
}

BOOL
destroy_event_pool(
PUSB_EVENT_POOL pool
)
{
	if( pool == NULL )
		return FALSE;

	InitializeListHead( &pool->free_que );
	pool->free_count = pool->total_count = 0;
	usb_free_mem( pool->event_array );
	pool->event_array = NULL;

	return TRUE;
}

VOID
event_list_default_process_event(
PUSB_DEV pdev,
ULONG event,
ULONG context,
ULONG param
)
{}

//----------------------------------------------------------
//timer_svc pool routines

BOOL
init_timer_svc_pool(
PTIMER_SVC_POOL pool
)
{
	int i;
	if( pool == NULL )
		return FALSE;

	pool->timer_svc_array = usb_alloc_mem( NonPagedPool, sizeof( TIMER_SVC ) * MAX_TIMER_SVCS );
	InitializeListHead( &pool->free_que );
	pool->free_count = 0;
	pool->total_count = MAX_TIMER_SVCS;
	KeInitializeSpinLock( &pool->pool_lock );

	for( i = 0; i < MAX_TIMER_SVCS; i++ )
	{
		free_timer_svc( pool, &pool->timer_svc_array[ i ] );
	}

	return TRUE;
}

BOOL
free_timer_svc(
PTIMER_SVC_POOL pool,
PTIMER_SVC ptimer
)
{
	if( pool == NULL || ptimer == NULL )
		return FALSE;

	RtlZeroMemory( ptimer, sizeof( TIMER_SVC ) );
	InsertTailList( &pool->free_que, ( PLIST_ENTRY )&ptimer->timer_svc_link );
	pool->free_count ++;

	return TRUE;
}

PTIMER_SVC
alloc_timer_svc(
PTIMER_SVC_POOL pool,
LONG count
)  //null if failed
{
	PTIMER_SVC new;
	if( pool == NULL || count != 1 )
		return NULL;

	if( pool->free_count <= 0 )
		return NULL;

	new = ( PTIMER_SVC )RemoveHeadList( &pool->free_que );
	pool->free_count --;
	return new;

}

BOOL
destroy_timer_svc_pool(
PTIMER_SVC_POOL pool
)
{
	if( pool == NULL )
		return FALSE;

	usb_free_mem( pool->timer_svc_array );
	pool->timer_svc_array = NULL;
	InitializeListHead( &pool->free_que );
	pool->free_count = 0;
	pool->total_count = 0;

	return TRUE;
}

BOOL
dev_mgr_post_event(
PUSB_DEV_MANAGER dev_mgr,
PUSB_EVENT event
)
{
	KIRQL old_irql;

	if( dev_mgr == NULL || event == NULL )
		return FALSE;

	KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );
	InsertTailList( &dev_mgr->event_list, &event->event_link );
	KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );

	KeSetEvent( &dev_mgr->wake_up_event, 0, FALSE );
	return TRUE;
}


VOID
event_list_default_process_queue(
PLIST_HEAD event_list,
PUSB_EVENT_POOL event_pool,
PUSB_EVENT usb_event,
PUSB_EVENT out_event
)
{
	//remove the first event from the event list, and copy it to
	//out_event

	if( event_list == NULL || event_pool == NULL || usb_event == NULL || out_event == NULL )
		return;

	RemoveEntryList( &usb_event->event_link );
	RtlCopyMemory( out_event, usb_event, sizeof( USB_EVENT ) );
	free_event( event_pool, usb_event );
	return;
}

BOOL
psq_enqueue(
PPORT_STATUS_QUEUE psq,
ULONG status
)
{
	if( psq == NULL )
		return FALSE;

	if( psq_is_full( psq ) )
		return FALSE;

	psq->port_status[ psq->status_count ].wPortChange = HIWORD( status );
	psq->port_status[ psq->status_count ].wPortStatus = LOWORD( status );

	psq->status_count++;

	usb_dbg_print( DBGLVL_MAXIMUM, ("psq_enqueue(): last status=0x%x, status count=0x%x, port_flag=0x%x\n", \
				status, \
				psq->status_count, \
				psq->port_flags ) );
	return TRUE;

}

VOID
psq_init(
PPORT_STATUS_QUEUE psq
)
{
	RtlZeroMemory( psq, sizeof( PORT_STATUS_QUEUE ) );
	psq->port_flags = STATE_IDLE |  USB_PORT_FLAG_DISABLE;
}

ULONG
psq_outqueue(
PPORT_STATUS_QUEUE psq
)			//return 0xffffffff if no element
{
	ULONG status;

	if( psq == NULL )
		return 0;

	if( psq_is_empty( psq ) )
		return 0;

	status = ( ( PULONG )&psq->port_status )[ 0 ];
	psq->port_status[ 0 ] = psq->port_status[ 1 ];
	psq->port_status[ 1 ] = psq->port_status[ 2 ];
	psq->port_status[ 2 ] = psq->port_status[ 3 ];
	psq->status_count--;

	return status;
}

BOOL
psq_push(
PPORT_STATUS_QUEUE psq,
ULONG status
)
{
	if( psq == NULL )
		return FALSE;

	status = ( ( PULONG )&psq->port_status )[ 0 ];
	psq->port_status[ 3 ] = psq->port_status[ 2 ];
	psq->port_status[ 2 ] = psq->port_status[ 1 ];
	psq->port_status[ 1 ] = psq->port_status[ 0 ];

	( ( PULONG )&psq->port_status )[ 0 ]= status;

	psq->status_count++;
	psq->status_count = ( ( 4 > psq->status_count ) ? psq->status_count : 4 );

	return TRUE;
}

VOID
dev_mgr_driver_entry_init(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdrvr
)
{
	// Device Info

	RtlZeroMemory( pdrvr, sizeof( USB_DRIVER ) * DEVMGR_MAX_DRIVERS	);

	pdrvr[ RH_DRIVER_IDX ].driver_init = rh_driver_init;								// in fact, this routine will init the rh device rather that the driver struct.
	pdrvr[ RH_DRIVER_IDX ].driver_destroy = rh_driver_destroy;							// we do not need rh to destroy currently, since that may means fatal hardware failure

	pdrvr[ HUB_DRIVER_IDX ].driver_init = hub_driver_init;  										//no need, since dev_mgr is also a hub driver
	pdrvr[ HUB_DRIVER_IDX ].driver_destroy = hub_driver_destroy;

	pdrvr[ UMSS_DRIVER_IDX ].driver_init = umss_if_driver_init;
	pdrvr[ UMSS_DRIVER_IDX ].driver_destroy = umss_if_driver_destroy;

	pdrvr[ COMP_DRIVER_IDX ].driver_init = compdev_driver_init;
	pdrvr[ COMP_DRIVER_IDX ].driver_destroy = compdev_driver_destroy;

	pdrvr[ GEN_DRIVER_IDX ].driver_init = gendrv_driver_init;
	pdrvr[ GEN_DRIVER_IDX ].driver_destroy = gendrv_driver_destroy;

	pdrvr[ GEN_IF_DRIVER_IDX ].driver_init = gendrv_if_driver_init;
	pdrvr[ GEN_IF_DRIVER_IDX ].driver_destroy = gendrv_if_driver_destroy;
}

BOOL
dev_mgr_strobe(
PUSB_DEV_MANAGER dev_mgr
)
{
	PUSB_EVENT pevent;
	HANDLE thread_handle;

	if( dev_mgr == NULL )
		return FALSE;
	if( dev_mgr->hcd_count == 0 )
		return FALSE;

	dev_mgr->term_flag = FALSE;

	if( dev_mgr->hcd_count == 0 )
		return FALSE;

	KeInitializeSpinLock( &dev_mgr->event_list_lock );
	InitializeListHead( &dev_mgr->event_list );
	init_event_pool( &dev_mgr->event_pool );

	pevent = alloc_event( &dev_mgr->event_pool, 1 );
	if( pevent == NULL )
	{
		destroy_event_pool( &dev_mgr->event_pool );
		return FALSE;
	}

	pevent->flags = USB_EVENT_FLAG_ACTIVE;
	pevent->event = USB_EVENT_INIT_DEV_MGR;

	pevent->process_queue = event_list_default_process_queue;
	pevent->process_event = dev_mgr_event_init;

	pevent->context = ( ULONG )dev_mgr;

	KeInitializeEvent( &dev_mgr->wake_up_event,
					   SynchronizationEvent,
					   FALSE );

	InsertTailList( &dev_mgr->event_list, &pevent->event_link );

	if( PsCreateSystemThread(
							 &thread_handle,
							 0,
							 NULL,
							 NULL,
							 NULL,
							 dev_mgr_thread,
							 dev_mgr )
		!= STATUS_SUCCESS )
	{
		destroy_event_pool( &dev_mgr->event_pool );
		return FALSE;
	}

	ObReferenceObjectByHandle(
			thread_handle,
			THREAD_ALL_ACCESS,
			NULL,
			KernelMode,
			(PVOID*) &dev_mgr->pthread,
			NULL);

	ZwClose( thread_handle );

	return TRUE;
}

BOOL
dev_mgr_event_init(
PUSB_DEV pdev,		//always null. we do not use this param
ULONG event,
ULONG context,
ULONG param
)
{
	LARGE_INTEGER	due_time;
	PUSB_DEV_MANAGER dev_mgr;
	LONG 	i;

	usb_dbg_print( DBGLVL_MAXIMUM, ( "dev_mgr_event_init(): dev_mgr=0x%x, event=0x%x\n", context, event ) );
	dev_mgr = ( PUSB_DEV_MANAGER ) context;
	if( dev_mgr == NULL )
		return FALSE;

	if( event != USB_EVENT_INIT_DEV_MGR )
		return FALSE;

    //dev_mgr->root_hub = NULL;
	KeInitializeTimer( &dev_mgr->dev_mgr_timer );

	KeInitializeDpc( &dev_mgr->dev_mgr_timer_dpc,
					dev_mgr_timer_dpc_callback,
					( PVOID )dev_mgr );

	KeInitializeSpinLock( &dev_mgr->timer_svc_list_lock );
	InitializeListHead( &dev_mgr->timer_svc_list );
	init_timer_svc_pool( &dev_mgr->timer_svc_pool );
	dev_mgr->timer_click = 0;

	init_irp_list( &dev_mgr->irp_list );

	KeInitializeSpinLock( &dev_mgr->dev_list_lock );
	InitializeListHead( &dev_mgr->dev_list );

	dev_mgr->hub_count = 0;
	InitializeListHead( &dev_mgr->hub_list );

	dev_mgr->conn_count = 0;
	dev_mgr->driver_list = g_driver_list;

	dev_mgr_driver_entry_init( dev_mgr, dev_mgr->driver_list );

	for( i = 0; i < DEVMGR_MAX_DRIVERS; i++ )
	{
		if( dev_mgr->driver_list[ i ].driver_init == NULL )
			continue;

		if( dev_mgr->driver_list[ i ].driver_init( dev_mgr, &dev_mgr->driver_list[ i ] ) == FALSE )
			break;
	}
	if( i == DEVMGR_MAX_DRIVERS )
	{
		due_time.QuadPart = -( DEV_MGR_TIMER_INTERVAL_NS - 10 );

		KeSetTimerEx( &dev_mgr->dev_mgr_timer,
					  due_time,
					  DEV_MGR_TIMER_INTERVAL_MS,
					  &dev_mgr->dev_mgr_timer_dpc );

		return TRUE;
	}

	i--;

	for( ; i >= 0; i-- )
	{
		if( dev_mgr->driver_list[ i ].driver_destroy )
			dev_mgr->driver_list[ i ].driver_destroy( dev_mgr, &dev_mgr->driver_list[ i ] );
	}

	KeCancelTimer( &dev_mgr->dev_mgr_timer );
	KeRemoveQueueDpc( &dev_mgr->dev_mgr_timer_dpc );
	return FALSE;

}

VOID dev_mgr_destroy(
PUSB_DEV_MANAGER dev_mgr
)
{
	LONG i;
        // oops...
	KeCancelTimer ( &dev_mgr->dev_mgr_timer );
	KeRemoveQueueDpc( &dev_mgr->dev_mgr_timer_dpc );

	for( i = DEVMGR_MAX_DRIVERS - 1; i >= 0; i-- )
		dev_mgr->driver_list[ i ].driver_destroy( dev_mgr, &dev_mgr->driver_list[ i ]);

	destroy_irp_list( &dev_mgr->irp_list );
	destroy_timer_svc_pool( &dev_mgr->timer_svc_pool );
	destroy_event_pool( &dev_mgr->event_pool );

}

VOID
dev_mgr_thread(
PVOID context
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_EVENT pevent;
	PLIST_ENTRY pthis, pnext;
	USB_EVENT  usb_event;
	LARGE_INTEGER time_out;
	NTSTATUS status;
	BOOL dev_mgr_inited;
	KIRQL old_irql;
	LONG i;

    dev_mgr = ( PUSB_DEV_MANAGER )context;
	dev_mgr_inited = FALSE;
	usb_cal_cpu_freq();
	time_out.u.LowPart = ( 10 * 1000 * 1000 ) * 100 - 1;  		//1 minutes
	time_out.u.HighPart = 0;
	time_out.QuadPart = -time_out.QuadPart;

	//usb_dbg_print( DBGLVL_MAXIMUM + 1, ( "dev_mgr_thread(): current uhci status=0x%x\n", uhci_status( dev_mgr->pdev_ext->uhci ) ) );

	while( dev_mgr->term_flag == FALSE )
	{
		KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );
		if( IsListEmpty( &dev_mgr->event_list ) == TRUE )
		{
			KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
			status = KeWaitForSingleObject(
				&dev_mgr->wake_up_event,
				Executive,
				KernelMode,
				TRUE,
				&time_out
				);
			continue;
		}

		// usb_dbg_print( DBGLVL_MAXIMUM, ( "dev_mgr_thread(): current element in event list is 0x%x\n", \
		// 			dbg_count_list( &dev_mgr->event_list ) ) );

		dev_mgr_inited = TRUE; //since we have post one event, if this statement is executed, dev_mgr_event_init must be called sometime later or earlier

		ListFirst( &dev_mgr->event_list, pthis );
		pevent = struct_ptr( pthis, USB_EVENT, event_link );

		while( pevent && ( ( pevent->flags & USB_EVENT_FLAG_ACTIVE ) == 0 ) )
		{
			//skip inactive ones
			ListNext( &dev_mgr->event_list, &pevent->event_link, pnext );
			pevent = struct_ptr( pnext, USB_EVENT, event_link );
		}

		if( pevent != NULL )
		{
			if( pevent->process_queue == NULL )
				pevent->process_queue = event_list_default_process_queue;

			pevent->process_queue( &dev_mgr->event_list,
								 &dev_mgr->event_pool,
								 pevent,
								 &usb_event );
		}
		else
		{
			//no active event
			KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
			status = KeWaitForSingleObject(
				&dev_mgr->wake_up_event,
				Executive,
				KernelMode,
				TRUE,
				&time_out // 10 minutes
				);

			usb_dbg_print( DBGLVL_MAXIMUM, ("dev_mgr_thread(): wake up, reason=0x%x\n", status ) );
			continue;
		}

		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );

		if( usb_event.process_event )
		{
			usb_event.process_event( usb_event.pdev,
									 usb_event.event,
									 usb_event.context,
									 usb_event.param);
		}
		else
		{
			event_list_default_process_event( usb_event.pdev,
											  usb_event.event,
											  usb_event.context,
											  usb_event.param);
		}
	}

	if( dev_mgr_inited )
	{
		for( i = 0; i < dev_mgr->hcd_count; i++ )
			dev_mgr_disconnect_dev( dev_mgr->hcd_array[ i ]->hcd_get_root_hub( dev_mgr->hcd_array[ i ] ) );
		dev_mgr_destroy( dev_mgr );
	}
	PsTerminateSystemThread( 0 );
}

VOID
dev_mgr_timer_dpc_callback(
PKDPC Dpc,
PVOID context,
PVOID SystemArgument1,
PVOID SystemArgument2
)
{
	PUSB_DEV_MANAGER dev_mgr;
	LIST_HEAD templist;
	PLIST_ENTRY pthis, pnext;
	static ULONG ticks = 0;

	ticks++;
	dev_mgr = ( PUSB_DEV_MANAGER ) context;
	if( dev_mgr == NULL )
		return;

	dev_mgr->timer_click ++;
	InitializeListHead( &templist );

	KeAcquireSpinLockAtDpcLevel( &dev_mgr->timer_svc_list_lock );
	if( IsListEmpty( &dev_mgr->timer_svc_list ) == TRUE )
	{
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->timer_svc_list_lock );
		return;
	}

	ListFirst( &dev_mgr->timer_svc_list, pthis );
	while( pthis )
	{
		( ( PTIMER_SVC )pthis )->counter++;
		ListNext( &dev_mgr->timer_svc_list, pthis, pnext );
		if( ( ( PTIMER_SVC )pthis )->counter >= ( ( PTIMER_SVC )pthis )->threshold )
		{
			RemoveEntryList( pthis );
			InsertTailList( &templist, pthis );
		}
		pthis = pnext;
	}

	KeReleaseSpinLockFromDpcLevel( &dev_mgr->timer_svc_list_lock );


	while( IsListEmpty( &templist ) == FALSE )
	{
		pthis = RemoveHeadList( &templist );
		( ( PTIMER_SVC )pthis )->func( ( ( PTIMER_SVC )pthis )->pdev, ( PVOID )( ( PTIMER_SVC )pthis )->context );
		KeAcquireSpinLockAtDpcLevel( &dev_mgr->timer_svc_list_lock );
		free_timer_svc( &dev_mgr->timer_svc_pool, ( PTIMER_SVC )pthis );
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->timer_svc_list_lock );
	}

}

BOOL
dev_mgr_request_timer_svc(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DEV pdev,
ULONG context,
ULONG due_time,
TIMER_SVC_HANDLER handler
)
{
	PTIMER_SVC timer_svc;
	KIRQL old_irql;

	if( dev_mgr == NULL || pdev == NULL || due_time == 0 || handler == NULL )
		return FALSE;

	KeAcquireSpinLock( &dev_mgr->timer_svc_list_lock, &old_irql );
	timer_svc = alloc_timer_svc( &dev_mgr->timer_svc_pool, 1 );
	if( timer_svc == NULL )
	{
		KeReleaseSpinLock( &dev_mgr->timer_svc_list_lock, old_irql );
		return FALSE;
	}
	timer_svc->pdev = pdev;
	timer_svc->threshold = due_time;
	timer_svc->func = handler;
	timer_svc->counter = 0;

	InsertTailList( &dev_mgr->timer_svc_list, &timer_svc->timer_svc_link );
	KeReleaseSpinLock( &dev_mgr->timer_svc_list_lock, old_irql );
	return TRUE;
}

BYTE
dev_mgr_alloc_addr(
PUSB_DEV_MANAGER dev_mgr,
PHCD hcd
)
{
	// alloc a usb addr for the device within 1-128
	ULONG i;
	if( dev_mgr == NULL || hcd == NULL )
		return 0xff;

	return hcd->hcd_alloc_addr( hcd );
}

BOOL
dev_mgr_free_addr(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DEV pdev,
BYTE addr
)
{
	PHCD hcd;
	if( addr & 0x80 )
		return FALSE;

	if( dev_mgr == NULL || pdev == NULL )
		return FALSE;

	hcd = pdev->hcd;
	if( hcd == NULL )
		return FALSE;
	hcd->hcd_free_addr( hcd, addr );
	return TRUE;
}

PUSB_DEV
dev_mgr_alloc_device(
PUSB_DEV_MANAGER dev_mgr,
PHCD hcd
)
{
	BYTE addr;
	PUSB_DEV pdev;

	if( ( addr = dev_mgr_alloc_addr( dev_mgr, hcd ) ) == 0xff )
		return NULL;

	pdev = usb_alloc_mem( NonPagedPool, sizeof( USB_DEV ) );
	if( pdev == NULL )
		return NULL;

	RtlZeroMemory( pdev, sizeof( USB_DEV ) );

    KeInitializeSpinLock( &pdev->dev_lock );
	dev_mgr->conn_count++;

    pdev->flags = USB_DEV_STATE_RESET;          	//class | cur_state | low speed
	pdev->ref_count = 0;
	pdev->dev_addr = addr;

	pdev->hcd = hcd;

    pdev->dev_id = dev_mgr->conn_count;         	//will be used to compose dev_handle

	InitializeListHead( &pdev->default_endp.urb_list );
	pdev->default_endp.pusb_if = ( PUSB_INTERFACE )pdev;
	pdev->default_endp.flags = USB_ENDP_FLAG_DEFAULT_ENDP; //toggle | busy-count | stall | default-endp

	return pdev;
}

VOID
dev_mgr_free_device(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DEV pdev
)
{
	if( pdev == NULL || dev_mgr == NULL )
		return;

	dev_mgr_free_addr( dev_mgr, pdev, pdev->dev_addr );
	if( pdev->usb_config && pdev != pdev->hcd->hcd_get_root_hub( pdev->hcd ) )
	{
		//root hub has its config and desc buf allocated together,
		//so no usb_config allocated seperately
		dev_mgr_destroy_usb_config( pdev->usb_config );
		pdev->usb_config = NULL;
	}
	if( pdev->desc_buf )
	{
		usb_free_mem( pdev->desc_buf );
		pdev->desc_buf = NULL;
	}
	usb_free_mem( pdev );
	pdev = NULL;
	return;
}

BOOL
rh_driver_destroy(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver
)
{
	LONG i;
	PHCD hcd;

	if( dev_mgr == NULL )
		return FALSE;

	for( i = 0; i < dev_mgr->hcd_count; i++ )
	{
		hcd = dev_mgr->hcd_array[ i ];
		// if( hcd->hcd_get_type( hcd ) != HCD_TYPE_UHCI )
		// continue;
		rh_destroy( hcd->hcd_get_root_hub( hcd ) );
	}
	return TRUE;
}

BOOL
rh_driver_init(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver
)
{

	PUSB_DEV rh;
	PUSB_CONFIGURATION_DESC pconfig_desc;
	PUSB_INTERFACE_DESC pif_desc;
	PUSB_ENDPOINT_DESC pendp_desc;
	PUSB_CONFIGURATION pconfig;
	PUSB_INTERFACE pif;
	PUSB_ENDPOINT pendp;
	PHUB2_EXTENSION phub_ext;
	PTIMER_SVC ptimer;
	PURB purb;
	NTSTATUS status;
	PHCD hcd;
	LONG i;

	if( dev_mgr == NULL || pdriver == NULL )
		return FALSE;

	//init driver structure, no PNP table functions
	pdriver->driver_desc.flags = USB_DRIVER_FLAG_DEV_CAPABLE;
	pdriver->driver_desc.vendor_id = 0xffff; 					// USB Vendor ID
	pdriver->driver_desc.product_id = 0xffff;					// USB Product ID.
	pdriver->driver_desc.release_num = 0xffff;					// Release Number of Device

	pdriver->driver_desc.config_val = 0;						// Configuration Value
	pdriver->driver_desc.if_num = 0;							// Interface Number
	pdriver->driver_desc.if_class = USB_CLASS_HUB;				// Interface Class
	pdriver->driver_desc.if_sub_class = 0; 						// Interface SubClass
	pdriver->driver_desc.if_protocol = 0;						// Interface Protocol

	pdriver->driver_desc.driver_name = "USB root hub";	// Driver name for Name Registry
	pdriver->driver_desc.dev_class = USB_CLASS_HUB;
	pdriver->driver_desc.dev_sub_class = 0;						// Device Subclass
	pdriver->driver_desc.dev_protocol = 0;						// Protocol Info.

	//pdriver->driver_init = rh_driver_init;					// initialized in dev_mgr_init_driver
	//pdriver->driver_destroy = rh_driver_destroy;
	pdriver->disp_tbl.version = 1;								// other fields of the dispatch table is not used since rh needs no pnp

	pdriver->driver_ext = 0;
	pdriver->driver_ext_size = 0;

	for( i = 0; i < dev_mgr->hcd_count; i++ )
	{
		hcd = dev_mgr->hcd_array[ i ];
		//if( hcd->hcd_get_type( hcd ) != HCD_TYPE_UHCI )
		//    continue;

		if( ( rh = dev_mgr_alloc_device( dev_mgr, hcd ) )== NULL )
			return FALSE;

		rh->parent_dev = NULL;
		rh->port_idx = 0;
		rh->hcd = hcd;
		rh->flags = USB_DEV_CLASS_ROOT_HUB | USB_DEV_STATE_CONFIGURED;

		if( usb2( hcd ) )
			rh->flags |= USB_DEV_FLAG_HIGH_SPEED;

		rh->dev_driver = pdriver;

		rh->desc_buf_size = sizeof( USB_DEVICE_DESC )
			+ sizeof( USB_CONFIGURATION_DESC )
			+ sizeof( USB_INTERFACE_DESC )
			+ sizeof( USB_ENDPOINT_DESC )
			+ sizeof( USB_CONFIGURATION )
			+ sizeof( HUB2_EXTENSION );

		rh->desc_buf = usb_alloc_mem( NonPagedPool, rh->desc_buf_size );

		if( rh->desc_buf == NULL )
		{
			return FALSE;
		}
		else
			RtlZeroMemory( rh->desc_buf, rh->desc_buf_size );

		rh->pusb_dev_desc = ( PUSB_DEVICE_DESC )rh->desc_buf;

		rh->pusb_dev_desc->bLength = sizeof( USB_DEVICE_DESC );
		rh->pusb_dev_desc->bDescriptorType = USB_DT_DEVICE;
		rh->pusb_dev_desc->bcdUSB = 0x110;
		if( usb2( hcd ) )
			rh->pusb_dev_desc->bcdUSB = 0x200;
		rh->pusb_dev_desc->bDeviceClass = USB_CLASS_HUB;
		rh->pusb_dev_desc->bDeviceSubClass = 0;
		rh->pusb_dev_desc->bDeviceProtocol = 0;
		rh->pusb_dev_desc->bMaxPacketSize0 = 8;
		if( usb2( hcd ) )
		{
			rh->pusb_dev_desc->bDeviceProtocol = 1;
			rh->pusb_dev_desc->bMaxPacketSize0 = 64;
		}
		rh->pusb_dev_desc->idVendor = 0;
		rh->pusb_dev_desc->idProduct = 0;
		rh->pusb_dev_desc->bcdDevice = 0x100;
		rh->pusb_dev_desc->iManufacturer = 0;
		rh->pusb_dev_desc->iProduct = 0;
		rh->pusb_dev_desc->iSerialNumber = 0;
		rh->pusb_dev_desc->bNumConfigurations = 1;

		pconfig_desc = ( PUSB_CONFIGURATION_DESC )&rh->desc_buf[ sizeof( USB_DEVICE_DESC ) ];
		pif_desc = ( PUSB_INTERFACE_DESC ) &pconfig_desc[ 1 ];
		pendp_desc = ( PUSB_ENDPOINT_DESC ) &pif_desc[ 1 ];

		pconfig_desc->bLength = sizeof( USB_CONFIGURATION_DESC );
		pconfig_desc->bDescriptorType = USB_DT_CONFIG;

		pconfig_desc->wTotalLength = sizeof( USB_CONFIGURATION_DESC )
			+ sizeof( USB_INTERFACE_DESC )
			+ sizeof( USB_ENDPOINT_DESC );

		pconfig_desc->bNumInterfaces = 1;
		pconfig_desc->bConfigurationValue = 1;
		pconfig_desc->iConfiguration = 0;
		pconfig_desc->bmAttributes = 0Xe0;  //self-powered and support remoke wakeup
		pconfig_desc->MaxPower = 0;

		pif_desc->bLength = sizeof( USB_INTERFACE_DESC );
		pif_desc->bDescriptorType = USB_DT_INTERFACE;
		pif_desc->bInterfaceNumber = 0;
		pif_desc->bAlternateSetting = 0;
		pif_desc->bNumEndpoints = 1;
		pif_desc->bInterfaceClass = USB_CLASS_HUB;
		pif_desc->bInterfaceSubClass = 0;
		pif_desc->bInterfaceProtocol = 0;
		pif_desc->iInterface = 0;

		pendp_desc->bLength = sizeof( USB_ENDPOINT_DESC );
		pendp_desc->bDescriptorType = USB_DT_ENDPOINT;
		pendp_desc->bEndpointAddress = 0x81;
		pendp_desc->bmAttributes = 0x03;
		pendp_desc->wMaxPacketSize = 8;
		pendp_desc->bInterval = USB_HUB_INTERVAL;
		if( usb2( hcd ) )
			pendp_desc->bInterval = 0x0c;

		pconfig = rh->usb_config = ( PUSB_CONFIGURATION )&pendp_desc[ 1 ];
		rh->active_config_idx = 0;
		pconfig->pusb_config_desc = pconfig_desc;
		pconfig->if_count = 1;
		pconfig->pusb_dev = rh;
		pif = &pconfig->interf[ 0 ];

		pif->endp_count = 1;
		pendp = &pif->endp[ 0 ];
		pif->pusb_config = pconfig;;
		pif->pusb_if_desc = pif_desc;

		pif->if_ext_size = 0;
		pif->if_ext = NULL;

		phub_ext = ( PHUB2_EXTENSION )&pconfig[ 1 ];
		phub_ext->port_count = 2;

		if( usb2( hcd ) )
		{
			// port count is configurable in usb2
			hcd->hcd_dispatch( hcd, HCD_DISP_READ_PORT_COUNT, &phub_ext->port_count );
		}

		{
			int j;
			for( j = 0; j < phub_ext->port_count; j++ )
			{
				psq_init( &phub_ext->port_status_queue[ j ] );
				phub_ext->child_dev[ j ] = NULL;
				usb_dbg_print( DBGLVL_MAXIMUM, ( "rh_driver_init(): port[ %d ].flag=0x%x\n", \
							j, phub_ext->port_status_queue[ j ].port_flags ) );
			}
		}

		phub_ext->pif = pif;
		phub_ext->hub_desc.bLength = sizeof( USB_HUB_DESCRIPTOR );
		phub_ext->hub_desc.bDescriptorType = USB_DT_HUB;
		phub_ext->hub_desc.bNbrPorts = ( UCHAR )phub_ext->port_count;
		phub_ext->hub_desc.wHubCharacteristics = 0;
		phub_ext->hub_desc.bPwrOn2PwrGood = 0;
		phub_ext->hub_desc.bHubContrCurrent = 50;

		rh->dev_ext = ( PBYTE )phub_ext;
		rh->dev_ext_size = sizeof( HUB2_EXTENSION );

		rh->default_endp.flags = USB_ENDP_FLAG_DEFAULT_ENDP;
		InitializeListHead( &rh->default_endp.urb_list );
		rh->default_endp.pusb_if = ( PUSB_INTERFACE )rh;
		rh->default_endp.pusb_endp_desc = NULL;	//???
		rh->time_out_count = 0;
		rh->error_count = 0;

		InitializeListHead( &pendp->urb_list );
		pendp->flags = 0;
		pendp->pusb_endp_desc = pendp_desc;
		pendp->pusb_if = pif;

		//add to device list
		InsertTailList( &dev_mgr->dev_list, &rh->dev_link );
		hcd->hcd_set_root_hub( hcd, rh );
		status = hub_start_int_request( rh );
		pdriver->driver_ext = 0;
	}
	return TRUE;
}

BOOL
rh_destroy(
PUSB_DEV pdev
)
//to be the reverse of what init does, we assume that the timer is now killed
//int is disconnected and the hub thread will not process event anymore
{
	PUSB_DEV rh;
	PLIST_ENTRY pthis, pnext;
	PUSB_DEV_MANAGER dev_mgr;

	if( pdev == NULL )
		return FALSE;

	dev_mgr = dev_mgr_from_dev( pdev );

	//???
	rh = pdev->hcd->hcd_get_root_hub( pdev->hcd );
	if( rh == pdev )
	{
		//free all the buf
		dev_mgr_free_device(dev_mgr, rh );
		//dev_mgr->root_hub = NULL;
	}

	return TRUE;
}

VOID
rh_timer_svc_int_completion(
PUSB_DEV pdev,
PVOID context
)
{
	PUSB_EVENT pevent;
	PURB purb;
	ULONG status, i;
	PHCD hcd;
	USE_IRQL;

	if( pdev == NULL || context == NULL )
		return;

	purb = ( PURB )context;

	lock_dev( pdev, TRUE );

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		pdev->ref_count -= 2; 	//	one for timer_svc and one for urb, for those rh requests
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		usb_dbg_print( DBGLVL_MAXIMUM, ( "rh_timer_svc_int_completion(): the dev is zomb, 0x%x\n", pdev ) );
		return;
	}

	hcd = pdev->hcd;
	if( purb->data_length < 1 )
	{
		purb->status = STATUS_INVALID_PARAMETER;
		unlock_dev( pdev, TRUE );
		goto LBL_OUT;
	}

	pdev->hcd->hcd_dispatch( pdev->hcd, HCD_DISP_READ_RH_DEV_CHANGE, purb->data_buffer );
	purb->status = STATUS_SUCCESS;
	unlock_dev( pdev, TRUE );

LBL_OUT:
	hcd->hcd_generic_urb_completion( purb, purb->context );

	lock_dev( pdev, TRUE );
	pdev->ref_count -= 2; 	//	one for timer_svc and one for urb, for those rh requests
							//	that completed immediately, the ref_count of the dev for
							//	that urb won't increment and for normal hub request
							//	completion, hcd_generic_urb_completion will be called
							//  by the xhci_dpc_callback, and the ref_count for the urb
							//  is maintained there. So only rh's timer-svc cares refcount
							//  when hcd_generic_urb_completion is called.
	usb_dbg_print( DBGLVL_MAXIMUM, ( "rh_timer_svc_int_completion(): rh's ref_count=0x%x\n", pdev->ref_count ) );
	unlock_dev( pdev, TRUE );
	usb_dbg_print( DBGLVL_MAXIMUM, ( "rh_timer_svc_int_completion(): exitiing...\n" ) );
	return;
}

VOID
rh_timer_svc_reset_port_completion(
PUSB_DEV pdev,
PVOID context
)
{
	PURB					purb;
	ULONG					i;
	USHORT					port_num;
	PHUB2_EXTENSION			hub_ext;
	PLIST_ENTRY				pthis, pnext;
	PUSB_DEV_MANAGER 		dev_mgr;
	PUSB_CTRL_SETUP_PACKET	psetup;

	USE_IRQL;

	if( pdev == NULL || context == NULL )
		return;

	dev_mgr = dev_mgr_from_dev( pdev );  //readonly and hold ref_count

	//block the rh polling
	KeAcquireSpinLockAtDpcLevel( &dev_mgr->timer_svc_list_lock );
	if( IsListEmpty( &dev_mgr->timer_svc_list ) == FALSE )
	{
		ListFirst( &dev_mgr->timer_svc_list, pthis );
		while( pthis )
		{
			if( ( ( PTIMER_SVC )pthis )->pdev == pdev && \
					( ( PTIMER_SVC )pthis )->threshold == RH_INTERVAL )
			{
				( ( PTIMER_SVC )pthis )->threshold = RH_INTERVAL + 0x800000;
				break;
			}

			ListNext( &dev_mgr->timer_svc_list, pthis, pnext );
			pthis = pnext;
		}
	}
	KeReleaseSpinLockFromDpcLevel( &dev_mgr->timer_svc_list_lock );

	purb = ( PURB )context;
	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		//purb->status = STATUS_ERROR;
		//pdev->hcd->hcd_generic_urb_completion( purb, purb->context );

		pdev->ref_count -= 2;
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		return;
	}

	i = pdev->hcd->hcd_rh_reset_port( pdev->hcd, ( UCHAR )psetup->wIndex );

	hub_ext = hub_ext_from_dev( pdev );

	{
		USHORT temp;
		PUCHAR pbuf;
		if( psetup->wIndex < 16 )
		{ 
			temp = 1 << psetup->wIndex;
			pbuf = ( PUCHAR )&temp;
			if( temp > 128 )
				pbuf++;
			hub_ext->int_data_buf[ psetup->wIndex / 8 ] |= *pbuf;
			if( i == TRUE )
				hub_ext->rh_port_status[ psetup->wIndex ].wPortChange |= USB_PORT_STAT_C_RESET;
			else	// notify that is not a high speed device, will lost definitely
				hub_ext->rh_port_status[ psetup->wIndex ].wPortChange |= USB_PORT_STAT_C_CONNECTION;
		}
	}

	//???how to construct port status map
	// decrease the timer_svc ref-count
	pdev->ref_count --;
	unlock_dev( pdev, TRUE );

	purb->status = STATUS_SUCCESS;
	//we delegate the completion to the rh_timer_svc_int_completion.
	//this function is equivalent to hub_start_reset_port_completion

	usb_free_mem( purb );

	//expire the rh polling timer
	KeAcquireSpinLockAtDpcLevel( &dev_mgr->timer_svc_list_lock );
	if( IsListEmpty( &dev_mgr->timer_svc_list ) == FALSE )
	{
		ListFirst( &dev_mgr->timer_svc_list, pthis );
		while( pthis )
		{
			if( ( ( PTIMER_SVC )pthis )->pdev == pdev && \
					( ( PTIMER_SVC )pthis )->threshold == RH_INTERVAL + 0x800000 )
			{
				( ( PTIMER_SVC )pthis )->counter = RH_INTERVAL;
				( ( PTIMER_SVC )pthis )->threshold = RH_INTERVAL;
				break;
			}

			ListNext( &dev_mgr->timer_svc_list, pthis, pnext );
			pthis = pnext;
		}
	}
	KeReleaseSpinLockFromDpcLevel( &dev_mgr->timer_svc_list_lock );

	lock_dev( pdev, TRUE );
	pdev->ref_count--;
	unlock_dev( pdev, TRUE );
	return;
}

VOID
dev_mgr_disconnect_dev(
PUSB_DEV pdev
)
//called when a disconnect is detected on the port
{
	PLIST_ENTRY pthis, pnext;
	PHUB2_EXTENSION phub_ext;
	PUSB_CONFIGURATION pconfig;
	PUSB_INTERFACE pif;
	PUSB_DEV_MANAGER dev_mgr;
	PHCD hcd;
	BOOL is_hub, found;
	ULONG dev_id;

	int i;
	USE_IRQL;

	if( pdev == NULL )
		return;

	found = FALSE;

	usb_dbg_print( DBGLVL_MAXIMUM, ( "dev_mgr_disconnect_dev(): entering, pdev=0x%x\n", pdev ) );
	lock_dev( pdev, FALSE );
	pdev->flags &= ~ USB_DEV_STATE_MASK;
	pdev->flags |= USB_DEV_STATE_BEFORE_ZOMB;
	dev_mgr = dev_mgr_from_dev( pdev );
	unlock_dev( pdev, FALSE );

	// notify dev_driver that the dev stops function before any operations
	if( pdev->dev_driver && pdev->dev_driver->disp_tbl.dev_stop )
		pdev->dev_driver->disp_tbl.dev_stop( dev_mgr, dev_handle_from_dev( pdev ) );

	//safe to use the dev pointer in this function.
	lock_dev( pdev, FALSE );
	pdev->flags &= ~ USB_DEV_STATE_MASK;
	pdev->flags |= USB_DEV_STATE_ZOMB;
	hcd = pdev->hcd;
	dev_id = pdev->dev_id;
	unlock_dev( pdev, FALSE );

	if( dev_mgr == NULL )
		return;

	hcd->hcd_remove_device( hcd, pdev );

	//disconnect its children
	if( ( pdev->flags & USB_DEV_CLASS_MASK ) == USB_DEV_CLASS_HUB || \
		( pdev->flags & USB_DEV_CLASS_MASK ) == USB_DEV_CLASS_ROOT_HUB )
	{
		phub_ext = hub_ext_from_dev( pdev );
		if( phub_ext )
		{
			for( i = 1; i <= phub_ext->port_count; i++ )
			{
				if( phub_ext->child_dev[ i ] )
				{
					dev_mgr_disconnect_dev( phub_ext->child_dev[ i ] );
					phub_ext->child_dev[ i ] = NULL;
				}
			}
		}
	}

	pconfig = pdev->usb_config;

	//remove event belong to the dev
	is_hub = ( ( pdev->flags & USB_DEV_CLASS_MASK ) == USB_DEV_CLASS_HUB );

	if( phub_ext && is_hub )
	{
		for( i = 1; i <= phub_ext->port_count; i++ )
		{
			found = hub_remove_reset_event( pdev, i, FALSE );
			if( found )
				break;
		}
	}

	//free event of the dev from the event list
	KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );
	ListFirst( &dev_mgr->event_list, pthis );
	while( pthis )
	{
		ListNext( &dev_mgr->event_list, pthis, pnext );
		if( ( ( PUSB_EVENT )pthis )->pdev == pdev )
		{
			PLIST_ENTRY p1;
			RemoveEntryList( pthis );
			if( ( ( ( PUSB_EVENT )pthis )->flags & USB_EVENT_FLAG_QUE_TYPE )
				!= USB_EVENT_FLAG_NOQUE )
			{
				//has a queue, re-insert the queue
				if( p1 = ( PLIST_ENTRY )( ( PUSB_EVENT )pthis )->pnext )
				{
					InsertHeadList( &dev_mgr->event_list, p1 );
					free_event( &dev_mgr->event_pool, struct_ptr( pthis, USB_EVENT, event_link ) );
					pthis = p1;
					//note: this queue will be examined again in the next loop
					//to find the matched dev in the queue
					continue;
				}
			}
			free_event( &dev_mgr->event_pool, struct_ptr( pthis, USB_EVENT, event_link ) );
		}
		else if( ( ( ( ( PUSB_EVENT )pthis )->flags & USB_EVENT_FLAG_QUE_TYPE )
				   != USB_EVENT_FLAG_NOQUE ) && ( ( PUSB_EVENT )pthis )->pnext )
		{
			//has a queue, examine the queue
			PUSB_EVENT p1, p2;
			p1 = ( PUSB_EVENT )pthis;
			p2 = p1->pnext;
			while( p2 )
			{
				if( p2->pdev == pdev )
				{
					p1->pnext = p2->pnext;
					p2->pnext = NULL;
					free_event( &dev_mgr->event_pool,  p2 );
					p2 = p1->pnext;
				}
				else
				{
					p1 = p2;
					p2 = p2->pnext;
				}
			}
		}
		pthis = pnext;
	}
	KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );

	// found indicates the reset event on one of the dev's port in process
	if( found )
		hub_start_next_reset_port( dev_mgr_from_dev( pdev ), FALSE );

	// remove timer-svc belonging to the dev
	KeAcquireSpinLock( &dev_mgr->timer_svc_list_lock, &old_irql );
	ListFirst( &dev_mgr->timer_svc_list, pthis );
	i = 0;
	while( pthis )
	{
		ListNext( &dev_mgr->timer_svc_list, pthis, pnext );
		if( ( ( PUSB_EVENT )pthis )->pdev == pdev )
		{
			RemoveEntryList( pthis );
			free_timer_svc( &dev_mgr->timer_svc_pool, struct_ptr( pthis, TIMER_SVC, timer_svc_link ) );
			i++;
		}
		pthis = pnext;
	}
	KeReleaseSpinLock( &dev_mgr->timer_svc_list_lock, old_irql );

	// release the refcount
	if( i )
	{
		lock_dev( pdev, FALSE );
		pdev->ref_count -= i;
		unlock_dev( pdev, FALSE );
	}

	// wait for all the reference count be released
	for( ; ; )
	{
		LARGE_INTEGER interval;

		lock_dev( pdev, FALSE );
		if( pdev->ref_count == 0 )
		{
			unlock_dev( pdev, FALSE );
			break;
		}
		unlock_dev( pdev, FALSE );
		// Wait two ms.
		interval.QuadPart = -20000;
		KeDelayExecutionThread( KernelMode, FALSE, &interval );
	}

	if( pdev->dev_driver && pdev->dev_driver->disp_tbl.dev_disconnect )
		pdev->dev_driver->disp_tbl.dev_disconnect( dev_mgr, dev_handle_from_dev( pdev ) );

	// we put it here to let handle valid before disconnect
	KeAcquireSpinLock( &dev_mgr->dev_list_lock, &old_irql );
	ListFirst( &dev_mgr->dev_list, pthis );
	while( pthis )
	{
		if( ( ( PUSB_DEV )pthis ) == pdev )
		{
			RemoveEntryList( pthis );
			break;
		}
		ListNext( &dev_mgr->dev_list, pthis, pnext );
		pthis = pnext;
	}
	KeReleaseSpinLock( &dev_mgr->dev_list_lock, old_irql );


	if( pdev != pdev->hcd->hcd_get_root_hub( pdev->hcd ) )
	{
		dev_mgr_free_device( dev_mgr, pdev );
	}
	else
	{
		//rh_destroy( pdev );
		//TRAP();
		//destroy it in dev_mgr_destroy
	}

	return;
}

BOOL
hub_driver_init(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver
)
{
	//init driver structure, no PNP table functions
	pdriver->driver_desc.flags = USB_DRIVER_FLAG_DEV_CAPABLE;
	pdriver->driver_desc.vendor_id = 0xffff; 					// USB Vendor ID
	pdriver->driver_desc.product_id = 0xffff;					// USB Product ID.
	pdriver->driver_desc.release_num = 0xffff;					// Release Number of Device

	pdriver->driver_desc.config_val = 0;						// Configuration Value
	pdriver->driver_desc.if_num = 0;							// Interface Number
	pdriver->driver_desc.if_class = USB_CLASS_HUB;				// Interface Class
	pdriver->driver_desc.if_sub_class = 0; 						// Interface SubClass
	pdriver->driver_desc.if_protocol = 0;						// Interface Protocol

	pdriver->driver_desc.driver_name = "USB hub";			// Driver name for Name Registry
	pdriver->driver_desc.dev_class = USB_CLASS_HUB;
	pdriver->driver_desc.dev_sub_class = 0;						// Device Subclass
	pdriver->driver_desc.dev_protocol = 0;						// Protocol Info.

	//pdriver->driver_init = hub_driver_init;					// initialized in dev_mgr_init_driver
	//pdriver->driver_destroy = hub_driver_destroy;

	pdriver->driver_ext = 0;
	pdriver->driver_ext_size = 0;

	pdriver->disp_tbl.version = 1;
	pdriver->disp_tbl.dev_connect = hub_connect;
	pdriver->disp_tbl.dev_disconnect = hub_disconnect;
	pdriver->disp_tbl.dev_stop = hub_stop;
	pdriver->disp_tbl.dev_reserved = NULL;

	return TRUE;
}

BOOL
hub_driver_destroy(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver
)
{
	pdriver->driver_ext = NULL;
	return TRUE;
}
void

hub_reset_pipe_completion(
PURB purb,		//only for reference, can not be released
PVOID context
)
{
	PUSB_DEV pdev;
	PUSB_ENDPOINT pendp;
	NTSTATUS status;

	USE_IRQL;

	if( purb == NULL )
	{
		return;
	}

	pdev = purb->pdev;
	pendp = purb->pendp;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		return;
	}

	if( usb_error( purb->status ) )
	{
		//simply retry it
		unlock_dev( pdev, TRUE );
		//usb_free_mem( purb );
		return;
	}
	unlock_dev( pdev, TRUE );

	pdev = purb->pdev;
	hub_start_int_request( pdev );
	return;
}

NTSTATUS
hub_start_int_request(
PUSB_DEV pdev
)
{
	PURB purb;
	PUSB_INTERFACE pif;
	PHUB2_EXTENSION hub_ext;
	NTSTATUS status;
	PHCD hcd;
	USE_IRQL;

	if( pdev == NULL )
		return STATUS_INVALID_PARAMETER;

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, FALSE );
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}
	purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
	RtlZeroMemory( purb, sizeof( URB ) );

	if( purb == NULL )
	{
		unlock_dev( pdev, FALSE );
		return STATUS_NO_MEMORY;
	}

    purb->flags = 0;
	purb->status = STATUS_SUCCESS;
	hub_ext = hub_ext_from_dev( pdev );
	purb->data_buffer = hub_ext->int_data_buf;
    purb->data_length = ( hub_ext->port_count + 7 ) / 8;

	hub_if_from_dev( pdev, pif );
    usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_start_int_request(): pdev=0x%x, pif=0x%x\n", pdev, pif ) );
	purb->pendp = &pif->endp[ 0 ];
	purb->pdev = pdev;

    purb->completion = hub_int_completion;
    purb->context = hub_ext;

    purb->pirp = NULL;
    purb->reference = 0;
	hcd = pdev->hcd;
	unlock_dev( pdev, FALSE );

	status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb );
	if( status != STATUS_PENDING )
	{
		usb_free_mem( purb );
		purb = NULL;
	}

	return status;
}

void
hub_int_completion(
PURB purb,
PVOID pcontext
)
{

	PUSB_DEV pdev;
	PHUB2_EXTENSION hub_ext;
	ULONG port_idx;
	PUSB_CTRL_SETUP_PACKET psetup;
	NTSTATUS status;
	LONG i;
	PHCD hcd;

	USE_IRQL;

	if( purb == NULL )
		return;

	if( pcontext == NULL )
	{
		usb_free_mem( purb );
		return;
	}

	usb_dbg_print( DBGLVL_MAXIMUM, ("hub_int_completion(): entering...\n" ) );

	pdev = purb->pdev;
	hub_ext = pcontext;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		return;
	}

	hcd = pdev->hcd;

	if( purb->status == STATUS_SUCCESS )
	{

		for( i = 1; i <= hub_ext->port_count; i++ )
		{
			if( hub_ext->int_data_buf[ i >> 3 ] & ( 1 << i ) )
			{
			    break;
			}
		}
		if( i > hub_ext->port_count )
		{
			//no status change, re-initialize the int request
			unlock_dev( pdev, TRUE );
			usb_free_mem( purb );
			hub_start_int_request( pdev );
			return;
		}

		port_idx = ( ULONG )i;

		//re-use the urb to get port status
		purb->pendp = &pdev->default_endp;
		purb->data_buffer = ( PUCHAR )&hub_ext->port_status;

		purb->data_length = sizeof( USB_PORT_STATUS );
		purb->pdev = pdev;

		purb->context = hub_ext;
		purb->pdev = pdev;
		purb->completion = hub_get_port_status_completion;
		purb->reference = port_idx;

		psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

		psetup->bmRequestType = 0xa3; //host-device class other recepient
		psetup->bRequest = USB_REQ_GET_STATUS;
		psetup->wValue = 0;
		psetup->wIndex = ( USHORT )port_idx;
		psetup->wLength = 4;

		purb->pirp = NULL;
		unlock_dev( pdev, TRUE );

		status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb );
		if( usb_error( status ) )
		{
			usb_free_mem( purb );
			purb = NULL;
		}
		else if( status == STATUS_SUCCESS )
		{
			// this is for root hub
			hcd->hcd_generic_urb_completion( purb, purb->context );
		}
		return;
	}
	else
	{
		unlock_dev( pdev, TRUE );
		if( usb_halted( purb->status ) )
		{
			//let's reset pipe
			usb_reset_pipe( pdev, purb->pendp, hub_reset_pipe_completion, NULL );
		}
        //unexpected error
		usb_free_mem( purb );
		purb = NULL;
	}
	return;
}

VOID
hub_get_port_status_completion(
PURB purb,
PVOID context
)
{
	PUSB_DEV pdev;
	PUSB_ENDPOINT pendp;
	BYTE port_idx;
	PHUB2_EXTENSION hub_ext;
	PUSB_CTRL_SETUP_PACKET psetup;
	NTSTATUS status;
	PHCD hcd;

	USE_IRQL;

	if( purb == NULL || context == NULL )
		return;

	usb_dbg_print( DBGLVL_MAXIMUM, ("hub_get_port_feature_completion(): entering...\n" ) );

	pdev = purb->pdev;
	pendp = purb->pendp;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		return;
	}

	hcd = pdev->hcd;
	if( usb_error( purb->status ) )
	{
		unlock_dev( pdev, TRUE );

		purb->status = 0;
        //simply retry the request refer to item 55 in document
		status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );
		if(  status != STATUS_PENDING )
		{
			if( status == STATUS_SUCCESS )
			{
				hcd->hcd_generic_urb_completion( purb, purb->context );

			}
			else
			{	
				//
				// must be fatal error
				// FIXME: better to pass it to the completion for further
				// processing? 
				//
				usb_free_mem( purb );
			}
		}
		return;
	}

	hub_ext = hub_ext_from_dev( pdev );
	port_idx = ( BYTE )purb->reference;

	usb_dbg_print( DBGLVL_MAXIMUM, ("hub_get_port_stataus_completion(): port_idx=0x%x, hcd =0x%x, \
				pdev=0x%x, purb=0x%x, hub_ext=0x%x, portsc=0x%x \n", \
				port_idx, \
				pdev->hcd, \
				pdev, \
				purb, \
				hub_ext,\
				*( ( PULONG ) purb->data_buffer ) ) );

	psq_enqueue( &hub_ext->port_status_queue[ port_idx ],
				 *( ( PULONG )purb->data_buffer ) );

	//reuse the urb to clear the feature
	RtlZeroMemory( purb, sizeof( URB ) );

	purb->data_buffer = NULL;
	purb->data_length = 0;
	purb->pendp = &pdev->default_endp;
	purb->pdev = pdev;

	purb->context = ( PVOID )&hub_ext->port_status ;
	purb->pdev = pdev;
	purb->completion = hub_clear_port_feature_completion;
	purb->reference = port_idx;

	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

	psetup->bmRequestType = 0x23; //host-device class port recepient
	psetup->bRequest = USB_REQ_CLEAR_FEATURE;
	psetup->wIndex = port_idx;
	psetup->wLength = 0;
	purb->pirp = NULL;

	if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_CONNECTION )
	{
		psetup->wValue = USB_PORT_FEAT_C_CONNECTION;
	}
	else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_ENABLE )
	{
		psetup->wValue = USB_PORT_FEAT_C_ENABLE;
	}
	else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_SUSPEND )
	{
		psetup->wValue = USB_PORT_FEAT_C_SUSPEND;
	}
	else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_OVERCURRENT )
	{
		psetup->wValue = USB_PORT_FEAT_C_OVER_CURRENT;
	}
	else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_RESET )
	{
		psetup->wValue = USB_PORT_FEAT_C_RESET;
	}
	unlock_dev( pdev, TRUE );

	status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );

	// if( status != STATUS_SUCCESS )
	if( status != STATUS_PENDING )
	{
		hcd->hcd_generic_urb_completion( purb, purb->context );
	}
	/*else if( usb_error( status ) )
	{
		usb_free_mem( purb );
		return;
	}*/
	return;

}

VOID
hub_clear_port_feature_completion(
PURB purb,
PVOID context
)
{
	BYTE 				port_idx;
	LONG 				i;
	BOOL 				bReset, event_post, brh;
	ULONG 				pc;
	PHCD				hcd;
	NTSTATUS 			status;
	PUSB_DEV 			pdev, pdev2;
	PUSB_EVENT 			pevent;
	PUSB_ENDPOINT 		pendp;
	PUSB_INTERFACE		pif;
	PHUB2_EXTENSION 		hub_ext;
	PUSB_DEV_MANAGER 	dev_mgr;

	PUSB_CTRL_SETUP_PACKET psetup;

	USE_IRQL;

	if( purb == NULL )
		return;

	if( context == NULL )
	{
		usb_free_mem( purb );
		return;
	}

	usb_dbg_print( DBGLVL_MAXIMUM, ("hub_clear_port_feature_completion(): entering...\n" ) );

	pdev = purb->pdev;
	port_idx = ( BYTE )purb->reference;

	lock_dev( pdev, TRUE );
	dev_mgr = dev_mgr_from_dev( pdev );
	hcd = pdev->hcd;
	brh = ( dev_class( pdev ) == USB_DEV_CLASS_ROOT_HUB );

	if( usb_error( purb->status ) )
	{
		unlock_dev( pdev, TRUE );

		purb->status = 0;

		// retry the request
		status = hcd->hcd_submit_urb( hcd, purb->pdev, purb->pendp, purb );
		if( status != STATUS_PENDING )
		{
			if( status == STATUS_SUCCESS )
			{
				hcd->hcd_generic_urb_completion( purb, purb->context );
			}
			else
			{
				//
				// FIXME: should we pass the error to the completion directly
				// instead of forstall it here? 
				//
				// do not think the device is workable, no requests to it any more.
				// including the int polling
				//
				// usb_free_mem( purb );
				//
				goto LBL_SCAN_PORT_STAT;
			}
		}
		return;
	}

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		return;
	}

	pc = ( ( PUSB_PORT_STATUS ) context )->wPortChange;

	if( pc )
	{
		// the bits are tested in ascending order
		if( pc & USB_PORT_STAT_C_CONNECTION )
		{
			pc &= ~USB_PORT_STAT_C_CONNECTION;
		}
		else if( pc & USB_PORT_STAT_C_ENABLE )
		{
			pc &= ~USB_PORT_STAT_C_ENABLE;
		}
		else if( pc & USB_PORT_STAT_C_SUSPEND )
		{
			pc &= ~USB_PORT_STAT_C_SUSPEND;
		}
		else if( pc & USB_PORT_STAT_C_OVERCURRENT )
		{
			pc &= ~USB_PORT_STAT_C_OVERCURRENT;
		}
		else if( pc & USB_PORT_STAT_C_RESET )
		{
			pc &= ~USB_PORT_STAT_C_RESET;
		}
	}
	( ( PUSB_PORT_STATUS ) context )->wPortChange = ( USHORT )pc;

	hub_ext = hub_ext_from_dev( pdev );

	if( pc )
	{
        //some other status change on the port still active
		psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

		if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_CONNECTION )
		{
			psetup->wValue = USB_PORT_FEAT_C_CONNECTION;
		}
		else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_ENABLE )
		{
			psetup->wValue = USB_PORT_FEAT_C_ENABLE;
		}
		else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_SUSPEND )
		{
			psetup->wValue = USB_PORT_FEAT_C_SUSPEND;
		}
		else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_OVERCURRENT )
		{
			psetup->wValue = USB_PORT_FEAT_C_OVER_CURRENT;
		}
		else if( hub_ext->port_status.wPortChange & USB_PORT_STAT_C_RESET )
		{
			psetup->wValue = USB_PORT_FEAT_C_RESET;
		}
		unlock_dev( pdev, TRUE );

		status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb );
		if( status != STATUS_PENDING )
		{
			if( status == STATUS_SUCCESS )
			{
				usb_dbg_print( DBGLVL_MAXIMUM, ("hub_clear_port_stataus_completion(): port_idx=0x%x, hcd=0x%x, \
							pdev=0x%x, purb=0x%x, hub_ext=0x%x, wPortChange=0x%x \n", \
							port_idx, \
							pdev->hcd, \
							pdev, \
							purb, \
							hub_ext,\
							pc ) );

				hcd->hcd_generic_urb_completion( purb, purb->context );
			}
			else
			{
				usb_dbg_print( DBGLVL_MAXIMUM, (" hub_clear_port_feature_completion(): \
							error=0x%x\n", status ) );

				// usb_free_mem( purb );
				goto LBL_SCAN_PORT_STAT;
			}
		}
		return;
	}

	for( i = 1; i <= hub_ext->port_count; i++ )
	{
		if( hub_ext->int_data_buf[ i >> 3 ] & ( 1 << i ) )
		{
		    break;
		}
	}

	//clear the port-change map, we have get port i's status.
	hub_ext->int_data_buf[ i >> 3 ] &= ~( 1 << i );

	//rescan to find some other port that has status change
	for( i = 1; i <= hub_ext->port_count; i++ )
	{
		if( hub_ext->int_data_buf[ i >> 3 ] & ( 1 << i ) )
		{
		    break;
		}
	}

	if( i <= hub_ext->port_count )
	{
		//still has port-change pending, get the port status change
		port_idx = ( UCHAR )i;

		//re-use the urb
		purb->data_buffer = ( PUCHAR )&hub_ext->port_status;
		purb->data_length = sizeof( USB_PORT_STATUS );
		purb->pendp = &pdev->default_endp;
		purb->pdev = pdev;

		purb->context = hub_ext;
		purb->pdev = pdev;
		purb->completion = hub_get_port_status_completion;
		purb->reference = port_idx;

		psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

		psetup->bmRequestType = 0xa3; //host-device class other recepient
		psetup->bRequest = USB_REQ_GET_STATUS;
		psetup->wValue = 0;
		psetup->wIndex = port_idx;
		psetup->wLength = 4;

		purb->pirp = NULL;

		unlock_dev( pdev, TRUE );

		status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb );
		if(  status != STATUS_PENDING )
		{
			if( status == STATUS_SUCCESS )
			{
				hcd->hcd_generic_urb_completion( purb, purb->context );
			}
			else
			{		//must be fatal error
				// usb_free_mem( purb );
				goto LBL_SCAN_PORT_STAT;
			}
		}
		return;
	}

	unlock_dev( pdev, TRUE );

LBL_SCAN_PORT_STAT:

	//all status changes are cleared
	if( purb )
		usb_free_mem( purb );

	purb = NULL;

	KeAcquireSpinLockAtDpcLevel( &dev_mgr->event_list_lock );
	lock_dev( pdev, TRUE );

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		//
		// if reset is in process, the dev_mgr_disconnect_dev will continue
		// the following resets
		//
		unlock_dev( pdev, TRUE );
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );
		return;
	}

	//at last we wake up thread if some port have status change to process
	port_idx = 0;
	for( i = 1, event_post = FALSE; i <= hub_ext->port_count; i++ )
	{
		if(	psq_is_empty( &hub_ext->port_status_queue[ i ]) == FALSE )
		{
			if( port_state( hub_ext->port_status_queue[ i ].port_flags ) == STATE_IDLE || 
				port_state( hub_ext->port_status_queue[ i ].port_flags ) == STATE_WAIT_ADDRESSED )
			{
				// have status in the queue pending
				// STATE_WAIT_ADDRESSED is added to avoid some bad mannered
				// hub to disturb the reset process
				hub_post_esq_event( pdev, ( BYTE )i, hub_event_examine_status_que );
			}
			else if( port_state( hub_ext->port_status_queue[ i ].port_flags ) ==  STATE_WAIT_RESET_COMPLETE )
			{
				//there is only one reset at one time
				port_idx = ( BYTE )i;
			}
		}
	}

	unlock_dev( pdev, TRUE );
	KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );


	if( port_idx )
		hub_check_reset_port_status(
			pdev,
			port_idx );

	//reinitialize the int request, here to reduce some uncertainty of concurrency
	hub_start_int_request( pdev );

	return;
}

VOID
hub_event_examine_status_que(
PUSB_DEV pdev,
ULONG event,
ULONG context, //hub_ext
ULONG param	   //port_idx
)
{
	PHUB2_EXTENSION hub_ext;
	USB_PORT_STATUS ps;
	PUSB_DEV pchild_dev;
	PTIMER_SVC ptimer;
	PUSB_DEV_MANAGER dev_mgr;

	USE_IRQL;

	if( pdev == NULL || context == 0 || param == 0 )
		return;

	while( TRUE )
	{
		lock_dev( pdev, FALSE );
		if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
		{
			unlock_dev( pdev, FALSE );
			break;
		}

		dev_mgr = dev_mgr_from_dev( pdev );
		hub_ext = hub_ext_from_dev( pdev );

		if( psq_is_empty( &hub_ext->port_status_queue[ param ] ) )
		{
			set_port_state( hub_ext->port_status_queue[ param ].port_flags,
							STATE_IDLE );
			unlock_dev( pdev, FALSE );
			break;
		}

		*( ( ULONG* )&ps ) = psq_outqueue( &hub_ext->port_status_queue[ param ] );


		pchild_dev = hub_ext->child_dev[ param ];
		hub_ext->child_dev[ param ] = 0;

		usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_event_examine_status_queue(): dev_addr=0x%x, port=0x%x, wPortChange=0x%x, wPortStatus=0x%x\n", \
					pdev->dev_addr, \
					param,
					ps.wPortChange, \
					ps.wPortStatus ) );

		unlock_dev( pdev, FALSE );

		if( pchild_dev != NULL )
		    dev_mgr_disconnect_dev( pchild_dev );

		if( ( ( ps.wPortChange & USB_PORT_STAT_C_ENABLE ) &&
					( ( pdev->flags & USB_DEV_CLASS_MASK ) != USB_DEV_CLASS_ROOT_HUB ) )
			|| ( ps.wPortChange & USB_PORT_STAT_C_OVERCURRENT )
			|| ( ps.wPortChange & USB_PORT_STAT_C_RESET )
			|| ( ( ps.wPortChange & USB_PORT_STAT_C_CONNECTION ) &&
				 !( ps.wPortStatus & USB_PORT_STAT_CONNECTION ) ) )
		{
			usb_dbg_print( DBGLVL_MAXIMUM, ("hub_event_examine_status_queue(): error occured, portc=0x%x, ports=0x%x\n", \
						ps.wPortChange,\
						ps.wPortStatus ) );

			lock_dev( pdev, FALSE );
			if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
			{
				unlock_dev( pdev, FALSE );
				break;
			}
			if( psq_is_empty( &hub_ext->port_status_queue[ param ] ) )
			{
				set_port_state( hub_ext->port_status_queue[ param ].port_flags,
								STATE_IDLE );
			}
			else
			{
				set_port_state( hub_ext->port_status_queue[ param ].port_flags,
								 STATE_EXAMINE_STATUS_QUE );
			}
			unlock_dev( pdev, FALSE );
			continue;

		}
		else if( ( ps.wPortChange & USB_PORT_STAT_C_CONNECTION )
				 && ( ps.wPortStatus & USB_PORT_STAT_CONNECTION )
				 && psq_is_empty( &hub_ext->port_status_queue[ param ] ) )
		{
			KeAcquireSpinLock( &dev_mgr->timer_svc_list_lock, &old_irql );
			lock_dev( pdev, TRUE );
			if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
			{
				unlock_dev( pdev, TRUE );
				KeReleaseSpinLock( &dev_mgr->timer_svc_list_lock, old_irql );
				usb_dbg_print( DBGLVL_MAXIMUM, ("hub_event_examine_status_queue(): dev lost\n" ) );
				break;
			}
			ptimer = alloc_timer_svc( &dev_mgr->timer_svc_pool, 1 );
			if( ptimer == NULL )
			{
				unlock_dev( pdev, TRUE );
				KeReleaseSpinLock( &dev_mgr->timer_svc_list_lock, old_irql );
				usb_dbg_print( DBGLVL_MAXIMUM, ("hub_event_examine_status_queue(): timer can not allocated\n" ) );
				break;
			}

			//a new connection
			usb_dbg_print( DBGLVL_MAXIMUM, ("hub_event_examine_status_queue(): new connection comes\n" ) );

            ptimer->counter = 0;
			ptimer->threshold = 21; //100 ms

			if( ps.wPortStatus & USB_PORT_STAT_LOW_SPEED )
				ptimer->threshold = 51; //500 ms

			ptimer->context = param;
			ptimer->pdev = pdev;
			ptimer->func = hub_timer_wait_dev_stable;
			InsertTailList( &dev_mgr->timer_svc_list, &ptimer->timer_svc_link );
			pdev->ref_count ++;
			set_port_state( hub_ext->port_status_queue[ param ].port_flags,
								STATE_WAIT_STABLE );
			unlock_dev( pdev, TRUE );
			KeReleaseSpinLock( &dev_mgr->timer_svc_list_lock, old_irql );
			break;

        }
		else
		{
			usb_dbg_print( DBGLVL_MAXIMUM, ("hub_event_examine_status_queue(): unknown error\n" ) );
			continue;
		}
    }
	return;
}

VOID
hub_timer_wait_dev_stable(
PUSB_DEV pdev,
PVOID context   //port-index
)
{

	PHUB2_EXTENSION hub_ext;
	PUSB_INTERFACE pif;
	PUSB_EVENT pevent;
	ULONG param;
	PUSB_DEV_MANAGER dev_mgr;

	USE_IRQL;

	if( pdev == NULL || context == 0 )
		return;

	dev_mgr = dev_mgr_from_dev( pdev );
	param = ( ULONG ) context;
	KeAcquireSpinLockAtDpcLevel( &dev_mgr->event_list_lock );
	lock_dev( pdev, TRUE );

	pdev->ref_count--;

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		goto LBL_OUT;
	}

	hub_ext = hub_ext_from_dev( pdev );

	if( !psq_is_empty( &hub_ext->port_status_queue[ param ] ) )
	{
		//error occured, normally we should not receive event here
		set_port_state( hub_ext->port_status_queue[ param ].port_flags,
							STATE_EXAMINE_STATUS_QUE );

		hub_post_esq_event( pdev, ( BYTE )param, hub_event_examine_status_que );
	}
	else
	{
		set_port_state( hub_ext->port_status_queue[ param ].port_flags,
							STATE_WAIT_RESET );

		hub_post_esq_event( pdev, ( BYTE )param, hub_event_dev_stable );

	}

 LBL_OUT:
	unlock_dev( pdev, TRUE );
	KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );
	return;
}

VOID
hub_event_dev_stable(
PUSB_DEV pdev,
ULONG event,
ULONG context, //hub_ext
ULONG param	   //port_idx
)
{

	PHUB2_EXTENSION hub_ext;
	PUSB_EVENT pevent, pevent1;
	PLIST_ENTRY pthis, pnext;
	BOOL que_exist;
	PHCD hcd;
	PUSB_DEV_MANAGER dev_mgr;
	NTSTATUS status;
	PURB  purb;
	PUSB_CTRL_SETUP_PACKET psetup;

	USE_IRQL;

	if( pdev == NULL || context == 0 || param == 0 )
		return;

	dev_mgr = dev_mgr_from_dev( pdev );
	KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );
	lock_dev( pdev, TRUE );

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
		goto LBL_OUT;

	hub_ext = hub_ext_from_dev( pdev );
	hcd = pdev->hcd;

	pevent = alloc_event( &dev_mgr->event_pool, 1 );
	if( pevent == NULL )
		goto LBL_OUT;

	pevent->event = USB_EVENT_WAIT_RESET_PORT;
	pevent->pdev = pdev;
	pevent->context = ( ULONG )hub_ext;
	pevent->param = param;
	pevent->flags = USB_EVENT_FLAG_QUE_RESET;
	pevent->process_event = NULL; //hub_event_reset_port_complete;
	pevent->process_queue = NULL; //hub_event_reset_process_queue;
	pevent->pnext = NULL;

	ListFirst( &dev_mgr->event_list, pthis );
	que_exist = FALSE;

	while( pthis )
	{
		//insert the event in to the wait-queue
		pevent1 = ( PUSB_EVENT ) pthis;
		if( pevent1->event == USB_EVENT_WAIT_RESET_PORT )
		{
			while( pevent1->pnext )
				pevent1 = pevent1->pnext;

			pevent1->pnext = pevent;
			que_exist = TRUE;
			break;
		}
		ListNext( &dev_mgr->event_list, pthis, pnext );
		pthis = pnext;
	}

	if( !que_exist )
	{
		//Let's start a reset port request
		InsertHeadList( &dev_mgr->event_list, &pevent->event_link );
		purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
		RtlZeroMemory( purb, sizeof( URB ) );

		purb->data_buffer = NULL;
		purb->data_length = 0;
		purb->pendp = &pdev->default_endp;

		purb->context = hub_ext;
		purb->pdev = pdev;
		purb->completion = hub_start_reset_port_completion; //hub_int_completion;
		purb->reference = param;

		psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

		psetup->bmRequestType = 0x23; 	//host-device other recepient
		psetup->bRequest = USB_REQ_SET_FEATURE;
		psetup->wValue = USB_PORT_FEAT_RESET;
		psetup->wIndex = ( USHORT )param;
		psetup->wLength = 0;

		purb->pirp = NULL;
		//enter another state
		set_port_state( hub_ext->port_status_queue[ param ].port_flags, STATE_WAIT_RESET_COMPLETE );

		unlock_dev( pdev, TRUE );
		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );

		status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb ) ;
		if(	status != STATUS_PENDING )
		{
				//must be fatal error
			usb_free_mem( purb );
			hub_reexamine_port_status_queue( pdev, param, FALSE );
			if( hub_remove_reset_event( pdev, param, FALSE ) )
				hub_start_next_reset_port( dev_mgr, FALSE );
		}
		return;
	}

 LBL_OUT:
	unlock_dev( pdev, TRUE );
	KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
	return;
}

VOID
hub_start_reset_port_completion(
PURB purb,
PVOID context
)
{
	PUSB_DEV pdev;
	PUSB_ENDPOINT pendp;
	PUSB_DEV_MANAGER dev_mgr;
	NTSTATUS status;
	ULONG port_idx;
	PHCD hcd;

	USE_IRQL;
	if( purb == NULL )
		return;

	if( context == NULL )
	{
		//fatal error no retry.
		usb_free_mem( purb );
		return;
	}

	pdev = purb->pdev;
	pendp = purb->pendp;
	port_idx = purb->reference;

	lock_dev( pdev, TRUE );

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		goto LBL_FREE_EVENT;
	}

	hcd = pdev->hcd;
	dev_mgr = dev_mgr_from_dev( pdev );
	unlock_dev( pdev, TRUE );

	status = purb->status;
	usb_free_mem( purb );

	if( !usb_error( status ) )
	{
		return;
	}

 LBL_FREE_EVENT:
	//since we have no patient to retry the dev, we should remove the event of
	//wait_reset_port on the port from the event list. and if possible, start
	//another reset process. note other port on the dev still have chance to be
	//reset if necessary.
	hub_reexamine_port_status_queue( pdev, port_idx, TRUE );
	if( hub_remove_reset_event( pdev, port_idx, TRUE ) )
		hub_start_next_reset_port( dev_mgr, TRUE );
	return;
}


VOID
hub_set_address_completion(
PURB purb,
PVOID context
)
{
	PUSB_DEV pdev, hub_dev;
	PUSB_ENDPOINT pendp;
	PUSB_DEV_MANAGER dev_mgr;
	NTSTATUS status;
	ULONG port_idx;
	PHCD hcd;

	USE_IRQL;

	if( purb == NULL )
		return;

	if( context == NULL )
	{
		//fatal error no retry.
		usb_free_mem( purb );
		return;
	}

	pdev = purb->pdev;
	pendp = purb->pendp;
	port_idx = purb->reference;

	lock_dev( pdev, TRUE );

	hcd = pdev->hcd;
	dev_mgr = dev_mgr_from_dev( pdev );
	hub_dev = pdev->parent_dev;
	port_idx = pdev->port_idx;

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		usb_free_mem( purb );
		//some error occured, let's start the next reset event
		goto LBL_RESET_NEXT;
	}

	pdev->flags &= ~USB_DEV_STATE_MASK;
	pdev->flags |= USB_DEV_STATE_ADDRESSED;

	unlock_dev( pdev, TRUE );
	status = purb->status;

	if( usb_error( status ) )
	{
		//retry the urb
		purb->status = 0;
		hcd_dbg_print( DBGLVL_MAXIMUM, ( "hub_set_address_completion: can not set address\n" ) );
		status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );
		//some error occured, disable the port
		if( status != STATUS_PENDING )
		{
			usb_free_mem( purb );
			status = hub_disable_port_request( hub_dev, ( UCHAR )port_idx );
		}
		return;
	}

	usb_free_mem( purb );
	//let address settle
	usb_wait_ms_dpc( 10 );

	//let's config the dev
	dev_mgr_start_config_dev( pdev );

 LBL_RESET_NEXT:

	//second, remove the event in the queue
	hub_reexamine_port_status_queue( hub_dev, port_idx, TRUE );
	if( hub_remove_reset_event( hub_dev, port_idx, TRUE ) )
		hub_start_next_reset_port( dev_mgr, TRUE );
	return;
};

VOID
hub_disable_port_completion(
PURB purb,
PVOID pcontext
)
{
	PHUB2_EXTENSION hub_ext;
	PUSB_DEV pdev;
	PUSB_DEV_MANAGER dev_mgr;
	UCHAR port_idx;
	PUSB_ENDPOINT pendp;
	PUSB_CTRL_SETUP_PACKET psetup;

	if( purb == NULL )
		return;

	pdev = purb->pdev;
	pendp = purb->pendp;
	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;
	port_idx = ( UCHAR )psetup->wIndex;

	dev_mgr = dev_mgr_from_dev( pdev );

	usb_free_mem( purb );

	hub_reexamine_port_status_queue( pdev, port_idx, TRUE );
	if( hub_remove_reset_event( pdev, port_idx, TRUE ) )
		hub_start_next_reset_port( dev_mgr, TRUE );

	return;
}

NTSTATUS
hub_disable_port_request(
PUSB_DEV pdev,
UCHAR port_idx
)
//caller should guarantee the validity of the dev
{
	PURB purb;
	PUSB_ENDPOINT pendp;
	PHUB2_EXTENSION hub_ext;
	PUSB_CTRL_SETUP_PACKET psetup;
	NTSTATUS status;
	PHCD hcd;
	USE_IRQL;

	if( pdev == NULL || port_idx == 0 )
		return STATUS_INVALID_PARAMETER;

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, FALSE );
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}

	purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
	if( purb == NULL )
	{
		unlock_dev( pdev, FALSE );
		return STATUS_NO_MEMORY;
	}

   	RtlZeroMemory( purb, sizeof( URB ) );

	purb->flags = 0;
	purb->status = STATUS_SUCCESS;

	hub_ext = hub_ext_from_dev( pdev );

	purb->data_buffer = NULL;
    purb->data_length = 0;

	pendp = purb->pendp = &pdev->default_endp;
	purb->pdev = pdev;

    purb->completion = hub_disable_port_completion;
    purb->context = hub_ext;

    purb->pirp = NULL;
    purb->reference = 0;

	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

	psetup->bmRequestType = 0x23; 	//host-device other recepient
	psetup->bRequest = USB_REQ_CLEAR_FEATURE;			//clear_feature
	psetup->wValue = USB_PORT_FEAT_ENABLE;
	psetup->wIndex = ( USHORT )port_idx;
	psetup->wLength = 0;

	purb->pirp = NULL;
	//enter another state
	hcd = pdev->hcd;
	unlock_dev( pdev, FALSE );

	status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );
	if( status == STATUS_PENDING )
		return status;

	usb_free_mem( purb );
	return status;
}



BOOL
hub_remove_reset_event(
PUSB_DEV pdev,
ULONG port_idx,
BOOL from_dpc
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PLIST_ENTRY pthis, pnext;
	PUSB_EVENT pevent, pnext_event;
	BOOL found;

	KIRQL old_irql;

	if( pdev == NULL )
		return FALSE;

	if( port_idx == 0 )
		return FALSE;

	dev_mgr = dev_mgr_from_dev( pdev );
	found = FALSE;

	if( from_dpc )
		KeAcquireSpinLockAtDpcLevel( &dev_mgr->event_list_lock );
	else
		KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );

	ListFirst( &dev_mgr->event_list, pthis );
	while( pthis )
	{
		pevent = ( PUSB_EVENT )pthis;
		if( pevent->event == USB_EVENT_WAIT_RESET_PORT &&
			( pevent->flags & USB_EVENT_FLAG_QUE_TYPE ) == USB_EVENT_FLAG_QUE_RESET )
		{
			if( pevent->pdev == pdev && pevent->param == port_idx )
			{
				//remove it
				RemoveEntryList( &pevent->event_link );
				pnext_event = pevent->pnext;
				free_event( &dev_mgr->event_pool, pevent );

				if( pnext_event )
					InsertHeadList( &dev_mgr->event_list, &pnext_event->event_link );

				found = TRUE;
				break;
			}
		}
		ListNext( &dev_mgr->event_list, pthis, pnext );
		pthis = pnext;
	}

	if( from_dpc )
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );
	else
		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
	return found;
}

BOOL
hub_start_next_reset_port(
PUSB_DEV_MANAGER dev_mgr,
BOOL from_dpc
)
{
	PLIST_ENTRY pthis, pnext;
	PUSB_EVENT pevent, pnext_event;
	PUSB_DEV pdev;
	PHUB2_EXTENSION hub_ext;
	BOOL bret;
	PURB purb;
	BOOL processed;
	PUSB_CTRL_SETUP_PACKET psetup;
	PHCD hcd;

	USE_IRQL;

	if( dev_mgr == NULL )
		return FALSE;

	bret = FALSE;
	processed = FALSE;

	if( from_dpc )
		KeAcquireSpinLockAtDpcLevel( &dev_mgr->event_list_lock );
	else
		KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );

	ListFirst( &dev_mgr->event_list, pthis);

	while( pevent = ( PUSB_EVENT )pthis )
	{
		while( pevent->event == USB_EVENT_WAIT_RESET_PORT &&
				( pevent->flags & USB_EVENT_FLAG_QUE_TYPE ) == USB_EVENT_FLAG_QUE_RESET )
		{

			processed = TRUE;

			pdev = pevent->pdev;
			lock_dev( pdev, TRUE );

			if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
			{
				unlock_dev( pdev, TRUE );
				pnext_event = pevent->pnext;
				free_event( &dev_mgr->event_pool, pevent );
				pevent = pnext_event;
				if( pevent == NULL )
				{
					bret = FALSE;
					break;
				}
				continue;
			}

			purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
			RtlZeroMemory( purb, sizeof( URB ) );

			purb->data_buffer = NULL;
			purb->data_length = 0;
			purb->pendp = &pdev->default_endp;

			hub_ext = hub_ext_from_dev( pdev );
			purb->context = hub_ext;
			purb->pdev = pdev;
			purb->completion = hub_start_reset_port_completion;
			purb->reference = pevent->param;

			psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

			psetup->bmRequestType = 0x23; 	//host-device other recepient
			psetup->bRequest = 3;			//set_feature
			psetup->wValue = USB_PORT_FEAT_RESET;
			psetup->wIndex = ( USHORT )pevent->param;
			psetup->wLength = 0;

			purb->pirp = NULL;
			hcd = pdev->hcd;
			set_port_state( hub_ext->port_status_queue[ pevent->param ].port_flags, STATE_WAIT_RESET_COMPLETE );
			unlock_dev( pdev, TRUE );

			bret = TRUE;
			break;
		}

		if( !processed )
		{
			ListNext( &dev_mgr->event_list, pthis, pnext );
			pthis = pnext;
		}
		else
			break;
	}

	if( from_dpc )
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );
	else
		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );

	if( processed && bret )
	{
		if( hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb ) != STATUS_PENDING )
		{
			//fatal error
			usb_free_mem( purb );
			bret = FALSE;
			//do not know what to do
		}
	}

	if( pthis == NULL )
		bret = TRUE;

	return bret;
}

//
//must have event-list-lock and dev-lock acquired
//
VOID
hub_post_esq_event(
PUSB_DEV pdev,
BYTE port_idx,
PROCESS_EVENT pe
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_EVENT pevent;

	if( pdev == NULL || port_idx == 0  || pe == NULL )
		return;

	dev_mgr = dev_mgr_from_dev( pdev );

	pevent = alloc_event( &dev_mgr->event_pool, 1 );
	pevent->event = USB_EVENT_DEFAULT;
	pevent->process_queue = event_list_default_process_queue;
	pevent->process_event = pe;
	pevent->context = ( ULONG )hub_ext_from_dev( pdev );
	pevent->param = port_idx;
	pevent->flags = USB_EVENT_FLAG_ACTIVE;
	pevent->pdev = pdev;
	pevent->pnext = NULL;

	InsertTailList( &dev_mgr->event_list, &pevent->event_link );
	KeSetEvent( &dev_mgr->wake_up_event, 0, FALSE );
	// usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_post_esq_event(): current element in event list is 0x%x\n", \
	// 			dbg_count_list( &dev_mgr->event_list ) ) );
	return;

}

BOOL
hub_check_reset_port_status(
PUSB_DEV pdev,
LONG port_idx
)
// called only in hub_clear_port_feature_completion
{
	PUSB_DEV_MANAGER dev_mgr;
	PHUB2_EXTENSION hub_ext;
	BOOL bReset;
	USB_PORT_STATUS port_status;
	PUSB_DEV  pdev2;
	PURB purb2;
	PHCD hcd;

	PUSB_CTRL_SETUP_PACKET psetup;
	ULONG status;
	LARGE_INTEGER delay;

	USE_IRQL;

	//let's check whether the status change is a reset complete
	usb_dbg_print( DBGLVL_MAXIMUM, ("hub_check_reset_port_status(): entering...\n" ) );
	dev_mgr = dev_mgr_from_dev( pdev );
	KeAcquireSpinLockAtDpcLevel( &dev_mgr->dev_list_lock );
	lock_dev( pdev, TRUE );

	dev_mgr = dev_mgr_from_dev( pdev );
	hcd = pdev->hcd;

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->dev_list_lock );
		return FALSE;
	}

	hub_ext = hub_ext_from_dev( pdev );
	port_status = psq_peek( &hub_ext->port_status_queue[ port_idx ], 0 );

	bReset = FALSE;
	if( port_status.wPortChange & USB_PORT_STAT_C_RESET )
		bReset = TRUE;

	pdev2 = NULL;
	purb2 = NULL;

	if( bReset
		&& port_state( hub_ext->port_status_queue[ port_idx ].port_flags ) ==  STATE_WAIT_RESET_COMPLETE
		&& psq_count( &hub_ext->port_status_queue[ port_idx ] ) == 1 )
	{
		// a port-reset complete, empty the queue, keep the state
		psq_outqueue( &hub_ext->port_status_queue[ port_idx ] );
		set_port_state( hub_ext->port_status_queue[ port_idx ].port_flags,
						STATE_WAIT_ADDRESSED );

		//let's new a dev, and start the set-addr request
		if( hub_ext->child_dev[ port_idx ] == 0 )
		{
			pdev2 = hub_ext->child_dev[ port_idx ]
				= dev_mgr_alloc_device( dev_mgr, hcd );
			if( pdev2 )
			{
				purb2 = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
				if( !purb2 )
				{
					dev_mgr_free_device( dev_mgr, pdev2 );
					pdev2 = hub_ext->child_dev[ port_idx ] = NULL;
				}
				else
				{
					if( port_status.wPortStatus & USB_PORT_STAT_LOW_SPEED )
					{
						pdev2->flags |= USB_DEV_FLAG_LOW_SPEED;
					}
					else if( port_status.wPortStatus & USB_PORT_STAT_HIGH_SPEED )
						pdev2->flags |= USB_DEV_FLAG_HIGH_SPEED;

					pdev2->parent_dev = pdev;
					pdev2->port_idx = ( UCHAR )port_idx;
					pdev2->ref_count++;

					RtlZeroMemory( purb2, sizeof( URB ) );

					purb2->pdev = pdev2;
					purb2->pendp = &pdev2->default_endp;
					purb2->context = hub_ext;
					purb2->completion = hub_set_address_completion;

					InitializeListHead( &purb2->trasac_list );
					purb2->reference = port_idx;
					purb2->pirp = 0;

					psetup = ( PUSB_CTRL_SETUP_PACKET )purb2->setup_packet;
					psetup->bmRequestType = 0;
					psetup->bRequest = USB_REQ_SET_ADDRESS;
					psetup->wValue = pdev2->dev_addr;
				}
			}
		}

		if( pdev2 && purb2 )
		{
			//creation success, emit the urb
			//add to dev list
			InsertTailList( &dev_mgr->dev_list, &pdev2->dev_link );

			unlock_dev( pdev, TRUE );
			KeReleaseSpinLockFromDpcLevel( &dev_mgr->dev_list_lock );

			status = hcd->hcd_submit_urb( hcd, pdev2, purb2->pendp, purb2 );

			lock_dev( pdev2, TRUE );
			pdev2->ref_count--;
			usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_check_reset_port_status(): new dev ref_count=0x%x\n", pdev2->ref_count ) );
			unlock_dev( pdev2, TRUE );

			if( status != STATUS_PENDING )
			{
				usb_free_mem( purb2 );
				//??? do we need to lock it for SMP?
				//dev_mgr_free_device( dev_mgr, pdev2 ), let dev_mgr_thread to clean it;
				// disable the port
				if( hub_disable_port_request( pdev, ( UCHAR )port_idx ) != STATUS_PENDING )
					goto LBL_RESET_FAIL;
			}

			return TRUE;
		}
	}
	else
	{
		usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_check_reset_port_status(): not a correct reset status\n" ) );
	}
	unlock_dev( pdev, TRUE );
	KeReleaseSpinLockFromDpcLevel( &dev_mgr->dev_list_lock );

LBL_RESET_FAIL:
	//Any event other than reset cause the reset process stall and another
	//pending reset-port requeset is serviced
	hub_reexamine_port_status_queue( pdev, port_idx, TRUE );
	if( hub_remove_reset_event( pdev, port_idx, TRUE ) )
		hub_start_next_reset_port( dev_mgr, TRUE);

	return FALSE;
}

VOID
hub_reexamine_port_status_queue(
PUSB_DEV hub_dev,
ULONG port_idx,
BOOL from_dpc
)
{

	PHUB2_EXTENSION hub_ext;
	PUSB_DEV_MANAGER dev_mgr;

	USE_IRQL;

	if( hub_dev == NULL || port_idx == 0 )
		return;

	dev_mgr = dev_mgr_from_dev( hub_dev );
	if( from_dpc )
		KeAcquireSpinLockAtDpcLevel( &dev_mgr->event_list_lock );
	else
		KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );

	lock_dev( hub_dev, TRUE );
	if( dev_state( hub_dev ) != USB_DEV_STATE_ZOMB )
	{

		hub_ext = hub_ext_from_dev( hub_dev );
		if( psq_is_empty( &hub_ext->port_status_queue[ port_idx ] ) )
		{
			set_port_state( hub_ext->port_status_queue[ port_idx ].port_flags,
							STATE_IDLE );

		}
		else
		{
			set_port_state( hub_ext->port_status_queue[ port_idx ].port_flags,
							STATE_EXAMINE_STATUS_QUE );

			hub_post_esq_event( hub_dev, ( UCHAR )port_idx, hub_event_examine_status_que );
		}
	}
	unlock_dev( hub_dev, TRUE );

	if( from_dpc )
		KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );
	else
		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
	return;
}

BOOL
dev_mgr_start_config_dev(
PUSB_DEV pdev
)
//called in hub_set_address_completion
{
	PUSB_DEV_MANAGER dev_mgr;
	PBYTE data_buf;
	PUSB_CTRL_SETUP_PACKET psetup;
	PURB purb;
	PHCD hcd;

	USE_IRQL;

	if( pdev == NULL )
		return FALSE;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		return FALSE;
	}

	hcd = pdev->hcd;

	//first, get device descriptor
	purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
	data_buf = usb_alloc_mem( NonPagedPool, 512 );
	if( purb == NULL )
	{
		unlock_dev( pdev, TRUE );
		return FALSE;
	}

	RtlZeroMemory( purb, sizeof( URB ) );
	RtlZeroMemory( data_buf, 512 );

	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

	purb->data_buffer = data_buf;       	// user data
    purb->data_length = 8;					// get partial desc

	pdev->desc_buf = data_buf;
	pdev->desc_buf_size = 512;

	purb->pdev = pdev;
    purb->pendp = &pdev->default_endp;              	//pipe for current transfer

    purb->completion = dev_mgr_get_desc_completion;
	purb->reference = 0;

	InitializeListHead( &purb->trasac_list );

	psetup->bmRequestType = 0x80;
	psetup->bRequest = USB_REQ_GET_DESCRIPTOR;
	psetup->wValue =  ( USB_DT_DEVICE << 8 ) | 0;
	psetup->wIndex = 0;
	psetup->wLength = 8; //sizeof( USB_DEVICE_DESC );
	unlock_dev( pdev, TRUE );

	if( hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb ) != STATUS_PENDING )
	{
		usb_free_mem( purb );
		usb_free_mem( data_buf );
		return FALSE;
	}
	return TRUE;
}

VOID
dev_mgr_get_desc_completion(
PURB purb,
PVOID context
)
{
	PUSB_DEV pdev;
    PUSB_CONFIGURATION_DESC pconfig_desc;
	PUSB_ENDPOINT pendp;
	PUSB_DEV_MANAGER dev_mgr;
	NTSTATUS status;
	PUSB_CTRL_SETUP_PACKET psetup;
	LONG i;
	PHCD hcd;

	USE_IRQL;

	if( purb == NULL )
		return;

	pdev = purb->pdev;
	pendp = purb->pendp;

	if( pdev == NULL || pendp == NULL )
	{
		usb_free_mem( purb );
		purb = NULL;
		return;
	}

	lock_dev( pdev, TRUE );
	if( dev_state( pdev  ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		goto LBL_OUT;
	}

	pendp = &pdev->default_endp;
	dev_mgr = dev_mgr_from_dev( pdev );
   	hcd = pdev->hcd;
	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;

	if( usb_error( purb->status ) )
	{
		unlock_dev( pdev, TRUE );
		hcd_dbg_print( DBGLVL_MAXIMUM, ( "dev_mgr_get_desc_completion: can not get dev desc ref=0x%x, status=0x%x\n", purb->reference, purb->status ) );
		goto LBL_OUT;
	}

	switch( purb->reference )
	{
	case 0:
		{
			//only partial dev_desc
			//enable the dev specific default endp maxpacketsize
			pdev->pusb_dev_desc = ( PUSB_DEVICE_DESC )purb->data_buffer;

			psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;
			psetup->wLength = sizeof( USB_DEVICE_DESC );

			//get the complete dev_desc
			purb->reference = 1;
			purb->status = 0;
			purb->data_length = sizeof( USB_DEVICE_DESC );

			unlock_dev( pdev, TRUE );

			status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );
			if( status != STATUS_PENDING )
			{
				goto LBL_OUT;
			}
			return;
		}
	case 1:
		{
			//let's begin to get config descriptors.
			if( pdev->pusb_dev_desc->bNumConfigurations == 0 )
			{
				unlock_dev( pdev, TRUE );
				goto LBL_OUT;
			}

			purb->data_buffer += sizeof( USB_DEVICE_DESC );
			purb->data_length = 8;
			purb->reference++;
			purb->context = ( PVOID )sizeof( USB_DEVICE_DESC );
			purb->status = 0;

			psetup->wValue = ( USB_DT_CONFIG << 8 ) | 0;
			psetup->wLength = 8;
			unlock_dev( pdev, TRUE );

			status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );

			if( status != STATUS_PENDING )
			{
				goto LBL_OUT;
			}
			return;
		}
	default:
		{
			LONG config_idx;
			config_idx = ( purb->reference >> 1 ) - 1;
			if( ( purb->reference & 1 ) == 0 )
			{
				//partial config desc is obtained.
				pconfig_desc = ( PUSB_CONFIGURATION_DESC )purb->data_buffer;
				if( pconfig_desc->wTotalLength >= 1024 )
				{
					//treat as an error
					unlock_dev( pdev, TRUE );
					goto LBL_OUT;

				}

				if(	pconfig_desc->wTotalLength > ( USHORT )( pdev->desc_buf_size - ( LONG ) purb->context ) )
				{
					//rewind the 8-byte hdr
					*( ( PULONG )&context ) -= 8;
					realloc_buf( pdev, purb );
				}
				purb->data_length = pconfig_desc->wTotalLength;
				psetup->wLength = pconfig_desc->wTotalLength;
				purb->reference ++;
				unlock_dev( pdev, TRUE );
				status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );
				if( status != STATUS_PENDING )
					goto LBL_OUT;

			}
			else
			{
				//complete desc is returned.
				if( config_idx + 1 < pdev->pusb_dev_desc->bNumConfigurations )
				{
					//still have configurations left
					*( ( PULONG )&context ) += psetup->wLength;
					purb->data_buffer = &pdev->desc_buf[ ( LONG ) context ];
					purb->data_length = 8;
					psetup->wLength = 8;
					psetup->wValue = ( ( ( USB_DT_CONFIG ) << 8 ) | ( config_idx + 1 ) );
					purb->reference ++;
					purb->context = context;

					if( ( ( LONG )context ) + 8 > pdev->desc_buf_size )
						realloc_buf( pdev, purb );

					purb->status = 0;
					unlock_dev( pdev, TRUE );
					status = hcd->hcd_submit_urb( hcd, pdev, pendp, purb );
					if( status != STATUS_PENDING )
						goto LBL_OUT;
				}
				else
				{
					//config descriptors have all been fetched
					unlock_dev( pdev, TRUE );
					usb_free_mem( purb );
					purb = NULL;
				    dev_mgr_start_select_driver( pdev );
				}
			}
			return;
		}
	}

 LBL_OUT:
	usb_free_mem( purb );
	purb = NULL;

	lock_dev( pdev, TRUE );
	if( dev_state( pdev  ) != USB_DEV_STATE_ZOMB )
	{
		if( pdev->desc_buf )
		{
			usb_free_mem( pdev->desc_buf );
			pdev->desc_buf_size = 0;
			pdev->desc_buf = NULL;
			pdev->pusb_dev_desc = NULL;
			pdev->usb_config = NULL;
		}
	}
	unlock_dev( pdev, TRUE );

	return;
}

BOOL
dev_mgr_start_select_driver(
PUSB_DEV pdev
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_EVENT pevent;
	BOOL bret;

	USE_IRQL;

	if( pdev == NULL )
		return FALSE;

	dev_mgr = dev_mgr_from_dev( pdev );
	KeAcquireSpinLockAtDpcLevel( &dev_mgr->event_list_lock );
	lock_dev( pdev, TRUE );

	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		bret = FALSE;
		goto LBL_OUT;
	}

	pevent = alloc_event( &dev_mgr->event_pool, 1 );
	if( pevent == NULL )
	{
		bret = FALSE;
		goto LBL_OUT;
	}
	pevent->flags = USB_EVENT_FLAG_ACTIVE;
	pevent->event = USB_EVENT_DEFAULT;
	pevent->pdev = pdev;
	pevent->context = 0;
	pevent->param = 0;
	pevent->pnext = 0;         //vertical queue for serialized operation
	pevent->process_event = dev_mgr_event_select_driver;
	pevent->process_queue = event_list_default_process_queue;

	InsertTailList( &dev_mgr->event_list, &pevent->event_link );
	KeSetEvent( &dev_mgr->wake_up_event, 0, FALSE );
	bret = TRUE;

 LBL_OUT:
	unlock_dev( pdev, TRUE );
	KeReleaseSpinLockFromDpcLevel( &dev_mgr->event_list_lock );
	return bret;
}

BOOL
dev_mgr_connect_to_dev(
PVOID Parameter
)
{
	PUSB_DEV pdev;
	DEV_HANDLE dev_handle;
	NTSTATUS status;
	PUSB_DRIVER pdriver;
	PCONNECT_DATA pcd = ( PCONNECT_DATA )Parameter;
	PUSB_DEV_MANAGER dev_mgr;
	CONNECT_DATA param;

	USE_IRQL;

	if( pcd == NULL )
		return FALSE; 
	dev_handle = pcd->dev_handle;
	pdriver = pcd->pdriver;
	dev_mgr = pcd->dev_mgr;

	param.dev_mgr = dev_mgr;
	param.pdriver = pdriver;
	param.dev_handle = 0;		//not used

	status = usb_query_and_lock_dev( dev_mgr, dev_handle, &pdev );
	if( status != STATUS_SUCCESS )
		return FALSE;

	usb_dbg_print( DBGLVL_MAXIMUM, ( "dev_mgr_connect_to_dev(): about to call driver's dev_connect\n" ) );
	status = pdriver->disp_tbl.dev_connect( &param, dev_handle );
	usb_unlock_dev( pdev );
	return status;
}

VOID
dev_mgr_event_select_driver(
PUSB_DEV pdev,
ULONG event,
ULONG context,
ULONG param
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PLIST_ENTRY pthis, pnext;
	PUSB_DRIVER pdriver, pcand;
	LONG credit, match, i;
	DEV_HANDLE handle;
	CONNECT_DATA cd;

	USE_IRQL;

	if( pdev == NULL )
		return;

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, FALSE );
		return;
	}
	dev_mgr = dev_mgr_from_dev( pdev );

	pcand = NULL;
	match = 0;
	for( i = HUB_DRIVER_IDX; i < DEVMGR_MAX_DRIVERS; i++ )
	{
		//bypass root-hub driver with idx zero
		pdriver = ( PUSB_DRIVER ) &dev_mgr->driver_list[ i ];

		if( pdriver->driver_desc.flags & USB_DRIVER_FLAG_DEV_CAPABLE )
			credit = dev_mgr_score_driver_for_dev( dev_mgr, pdriver, pdev->pusb_dev_desc );
		else
		{
			continue;
		}
		if( credit > match )
			pcand = pdriver, match = credit;

	}

	if( match )
	{
		// we set class driver here
		// pdev->dev_driver = pcand;
		handle = usb_make_handle( pdev->dev_id, 0, 0 );
	}
	unlock_dev( pdev, FALSE );

	if( match )
	{

		cd.dev_handle = handle;
		cd.pdriver = pcand;
		cd.dev_mgr = dev_mgr;

		if( dev_mgr_connect_to_dev( &cd ) )
			return;

		// ExInitializeWorkItem( pwork_item, dev_mgr_connect_to_dev, ( PVOID )pcd );
		// ExQueueWorkItem( pwork_item, DelayedWorkQueue );
	}
	cd.dev_handle = handle;
	cd.pdriver = &dev_mgr->driver_list[ GEN_DRIVER_IDX ];
	cd.dev_mgr = dev_mgr;
	dev_mgr_connect_to_dev( &cd );
	return;
}

BOOL
dev_mgr_build_usb_endp(
PUSB_INTERFACE pif,
PUSB_ENDPOINT pendp,
PUSB_ENDPOINT_DESC pendp_desc
)
{
	if( pendp == NULL || pif == NULL || pendp_desc == NULL )
		return FALSE;

    pendp->flags = 0;
    InitializeListHead( &pendp->urb_list );       //pending urb queue
	pendp->pusb_if = pif;
    pendp->pusb_endp_desc = pendp_desc;
	return TRUE;
}

BOOL
dev_mgr_build_usb_if(
PUSB_CONFIGURATION pcfg,
PUSB_INTERFACE pif,
PUSB_INTERFACE_DESC pif_desc,
BOOL alt_if
)
{
	LONG i;
	PUSB_ENDPOINT_DESC pendp_desc;

	if( pcfg == NULL || pif == NULL || pif_desc == NULL )
		return FALSE;

	if( alt_if == FALSE )
	{
		pif->endp_count = pif_desc->bNumEndpoints > MAX_ENDPS_PER_IF
			? MAX_ENDPS_PER_IF
			: pif_desc->bNumEndpoints;

		pif->pif_drv = NULL;
		pif->pusb_config = pcfg;
		pif->pusb_if_desc = pif_desc;
		pif->if_ext_size = 0;
		pif->if_ext = NULL;

		InitializeListHead( &pif->altif_list );
		pif->altif_count = 0;

		pendp_desc = ( PUSB_ENDPOINT_DESC )( &( ( PBYTE )pif_desc )[ sizeof( USB_INTERFACE_DESC ) ] );

		for( i = 0; i < pif->endp_count; i++, pendp_desc++ )
		{
			dev_mgr_build_usb_endp( pif, &pif->endp[ i ], pendp_desc );
		}
	}
	else
	{
		PUSB_INTERFACE paltif;
		PLIST_ENTRY pthis, pnext;

		pif->altif_count++;
		paltif = usb_alloc_mem( NonPagedPool, sizeof( USB_INTERFACE ) );
		RtlZeroMemory( paltif, sizeof( USB_INTERFACE ) );
		InsertTailList( &pif->altif_list, &paltif->altif_list );
		paltif->pif_drv = NULL;
		paltif->pusb_config = pcfg;
		paltif->pusb_if_desc = pif_desc;
		paltif->if_ext_size = 0;
		paltif->if_ext = NULL;
		paltif->endp_count = pif_desc->bNumEndpoints > MAX_ENDPS_PER_IF
			? MAX_ENDPS_PER_IF
			: pif_desc->bNumEndpoints;

		ListFirst( &pif->altif_list, pthis );

		while( pthis )
		{
			//synchronize the altif_count;
			PUSB_INTERFACE pthis_if;
			pthis_if =( PUSB_INTERFACE )( ( ( PBYTE )pthis ) - offsetof( USB_INTERFACE, altif_list ) );
			pthis_if->altif_count = pif->altif_count;
			ListNext( &pif->altif_list, pthis, pnext );
		}

	}
	return TRUE;
}

NTSTATUS
dev_mgr_build_usb_config(
PUSB_DEV pdev,
PBYTE pbuf,
ULONG config_val,
LONG config_count
)
{
	PUSB_CONFIGURATION pcfg;
	PUSB_INTERFACE_DESC pif_desc;
	PUSB_INTERFACE pif;
	int i;
	LONG if_count;

	if( pdev == NULL || pbuf == NULL )
		return STATUS_INVALID_PARAMETER;


	pdev->usb_config = usb_alloc_mem( NonPagedPool, sizeof( USB_CONFIGURATION ) );
	pcfg = pdev->usb_config;

	if( pdev->usb_config == NULL )
		return STATUS_NO_MEMORY;

	RtlZeroMemory( pcfg, sizeof( USB_CONFIGURATION ) );
    pcfg->pusb_config_desc = usb_find_config_desc_by_val( pbuf,
													 config_val,
													 config_count );

	if( pcfg->pusb_config_desc == NULL )
	{
		usb_free_mem( pcfg );
		pdev->usb_config = NULL;
		return STATUS_UNSUCCESSFUL;
	}
	pcfg->if_count = pcfg->pusb_config_desc->bNumInterfaces;
    pcfg->pusb_dev = pdev;
	pif_desc = ( PUSB_INTERFACE_DESC )&( ( PBYTE )pcfg->pusb_config_desc )[ sizeof( USB_CONFIGURATION_DESC ) ];
	if_count = pcfg->if_count;

	for( i = 0 ; i < if_count; i++, pif_desc++ )
    {
		if( pif_desc->bAlternateSetting == 0)
		{
			dev_mgr_build_usb_if( pcfg, &pcfg->interf[ i ], pif_desc, FALSE );
		}
		else
		{
			i--;
			pif = &pcfg->interf[ i ];
			dev_mgr_build_usb_if( pcfg, pif, pif_desc, TRUE );
		}
	}
	return STATUS_SUCCESS;
}

NTSTATUS
dev_mgr_destroy_usb_config(
PUSB_CONFIGURATION pcfg
)
{
	long i;
	PLIST_ENTRY pthis, pnext;
	PUSB_INTERFACE pif;

	if( pcfg == NULL )
		return FALSE;

	for( i = 0; i < pcfg->if_count; i++ )
	{
		pif = &pcfg->interf[ i ];

		if( pif->altif_count )
		{
			ListFirst( &pif->altif_list, pthis );
			while( pthis )
			{
				PUSB_INTERFACE pthis_if;
				pthis_if =( PUSB_INTERFACE )( ( ( PBYTE )pthis ) - offsetof( USB_INTERFACE, altif_list ) );
				RemoveEntryList( pthis );
				usb_free_mem( pthis_if );
				if( IsListEmpty( &pif->altif_list ) == TRUE )
					break;

				ListFirst( &pif->altif_list, pthis );
			}
		}
	}
	usb_free_mem( pcfg );
	return TRUE;
}

#define is_dev_product_match( pdriVER, pdev_DESC ) \
( ( pdriVER )->driver_desc.vendor_id == ( pdev_DESC )->idVendor \
  && ( pdriVER )->driver_desc.product_id == ( pdev_DESC )->idProduct )

LONG
dev_mgr_score_driver_for_dev(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver,
PUSB_DEVICE_DESC pdev_desc
)
{
	LONG credit = 0;

	//assume supports all the sub_class are supported if sub_class is zero
	if( pdriver->driver_desc.dev_class == pdev_desc->bDeviceClass )
	{
		if( pdriver->driver_desc.dev_sub_class == 0 && pdriver->driver_desc.dev_protocol == 0 )
			credit = 3;
		else if( pdriver->driver_desc.dev_sub_class == pdev_desc->bDeviceSubClass )
		{
			if( pdriver->driver_desc.dev_protocol == 0 )
				credit = 6;
			else if( pdriver->driver_desc.dev_protocol == pdev_desc->bDeviceProtocol )
				credit = 9;
		}
	}

	if( is_dev_product_match( pdriver, pdev_desc ) )
		credit += 20;

	return credit;
}

LONG
dev_mgr_score_driver_for_if(
PUSB_DEV_MANAGER dev_mgr,
PUSB_DRIVER pdriver,
PUSB_INTERFACE_DESC pif_desc
)
{
	LONG credit;

	if( pdriver == NULL
		|| !( pdriver->driver_desc.flags & USB_DRIVER_FLAG_IF_CAPABLE )
		|| pif_desc == NULL
		|| dev_mgr == NULL )
		return 0;

	if( is_header_match( ( PBYTE )pif_desc, USB_DT_INTERFACE ) == FALSE )
	{
		return 0;
	}

	credit = 0;
	if( ( pdriver->driver_desc.if_class == pif_desc->bInterfaceClass ) )
	{
		if( pdriver->driver_desc.if_sub_class == 0 && pdriver->driver_desc.if_protocol == 0 )
			credit = 2;
		if( pdriver->driver_desc.if_sub_class == pif_desc->bInterfaceSubClass )
		{
			if( pdriver->driver_desc.if_protocol == 0 )
				credit = 4;
			if( pdriver->driver_desc.if_protocol == pif_desc->bInterfaceProtocol )
				credit = 6;
		}
	}
	else
		credit = 1;

	return credit;
}

#define is_equal_driver( pd1, pd2, ret ) \
{\
    int i;\
	ret = TRUE;\
    PUSB_DRIVER pdr1, pdr2;\
    pdr1 = ( PUSB_DRIVER )( pd1 );\
    pdr2 = ( PUSB_DRIVER ) ( pd2 );\
	for( i = 0; i < 16; i++ )\
	{\
		if( pdr1->driver_name[ i ] != pdr2->driver_name[ i ] )\
		{\
			ret = FALSE;\
			break;\
		}\
	}\
}

UCHAR
dev_mgr_register_hcd(
PUSB_DEV_MANAGER dev_mgr,
PHCD hcd
)
//return value is the hcd id
{
	if( dev_mgr == NULL || hcd == NULL )
		return 0xff;

	if( dev_mgr->hcd_count >= MAX_HCDS )
		return 0xff;

	dev_mgr->hcd_array[ dev_mgr->hcd_count++ ] = hcd;
	return dev_mgr->hcd_count - 1;
}

BOOL
dev_mgr_register_irp(
PUSB_DEV_MANAGER dev_mgr,
PIRP pirp,
PURB purb
)
{
	KIRQL old_irql;
	if( dev_mgr == NULL )
		return FALSE;

	if( add_irp_to_list( &dev_mgr->irp_list, pirp, purb ) )
	{
		return TRUE;
	}
	TRAP();
	return FALSE;
}

PURB
dev_mgr_remove_irp(
PUSB_DEV_MANAGER dev_mgr,
PIRP pirp
)
//caller must guarantee that when this func is called,
//the urb associated must exist.
{
	PURB purb;
	KIRQL old_irql;
	if( dev_mgr == NULL )
		return NULL;

	purb = remove_irp_from_list( &dev_mgr->irp_list, pirp, NULL );
	return purb;
}

VOID
dev_mgr_cancel_irp(
PDEVICE_OBJECT dev_obj,
PIRP pirp
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PDEVEXT_HEADER	pdev_ext_hdr;
	ULONG i;

	pdev_ext_hdr = ( PDEVEXT_HEADER )dev_obj->DeviceExtension;
	dev_mgr = pdev_ext_hdr->dev_mgr;

	if( dev_obj->CurrentIrp == pirp )
	{
		IoReleaseCancelSpinLock( pirp->CancelIrql );
		// we did not IoStartNextPacket, leave it for the urb completion
	}
	else
	{
		KeRemoveEntryDeviceQueue( &dev_obj->DeviceQueue, &pirp->Tail.Overlay.DeviceQueueEntry );
		IoReleaseCancelSpinLock( pirp->CancelIrql );

		pirp->IoStatus.Information = 0;
		pirp->IoStatus.Status = STATUS_CANCELLED;
		IoCompleteRequest( pirp, IO_NO_INCREMENT );
		// the device queue is moved on, no need to call IoStartNextPacket
		return;
	}

	//
	// remove the irp and call the dev_mgr_cancel_irp
	// the completion will be done in urb completion
	//
	remove_irp_from_list( &dev_mgr->irp_list, pirp, dev_mgr );
	return;

}

VOID
dev_mgr_release_hcd(
PUSB_DEV_MANAGER dev_mgr
)
// release the hcd
{
	LONG i;
	PHCD hcd;
	for( i = 0; i < dev_mgr->hcd_count; i++ )
	{
		hcd = dev_mgr->hcd_array[ i ];
		hcd->hcd_release( hcd );
		dev_mgr->hcd_array[ i ] = 0;
	}
	dev_mgr->hcd_count = 0;
	return;
}

VOID
dev_mgr_start_hcd(
PUSB_DEV_MANAGER dev_mgr
)
{
	LONG i;
	PHCD hcd;
	for( i = 0; i < dev_mgr->hcd_count; i++ )
	{
		hcd = dev_mgr->hcd_array[ i ];
		hcd->hcd_start( hcd );
	}
	return;
}

BOOL
hub_connect(
PCONNECT_DATA param,
DEV_HANDLE dev_handle
)
{
	URB urb, *purb;
	CHAR buf[ 512 ];
	DEV_HANDLE endp_handle;
	USB_DEVICE_DESC dev_desc;
	PUSB_CONFIGURATION_DESC pcfg_desc;
	PUSB_INTERFACE_DESC pif_desc;
	PUSB_CTRL_SETUP_PACKET psetup;
	NTSTATUS status;
	LONG i, j, found, cfg_val;
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_DEV pdev;
	USE_IRQL;


	if( param == NULL || dev_handle == 0 )
		return FALSE;

	dev_mgr = param->dev_mgr;

	pcfg_desc = ( PUSB_CONFIGURATION_DESC ) buf;
	endp_handle = dev_handle | 0xffff;
	UsbBuildGetDescriptorRequest(&urb,
								 endp_handle,
								 USB_DT_DEVICE,
								 0,
								 0,
								 ( &dev_desc ),
								 ( sizeof( USB_DEVICE_DESC ) ),
								 NULL,
								 0,
								 0 );

	status = usb_submit_urb( dev_mgr, &urb );
	if( status != STATUS_SUCCESS )
		return FALSE;

	found = FALSE;
	for( i = 0; i < dev_desc.bNumConfigurations; i++ )
	{
		UsbBuildGetDescriptorRequest(&urb,
									 endp_handle,
									 USB_DT_CONFIG,
									 ( USHORT )i,
									 0,
									 buf,
									 512,
									 NULL,
									 0,
									 0 );

		status = usb_submit_urb( dev_mgr, &urb );
		if( status != STATUS_SUCCESS )
		{
			return FALSE;
		}

		status = usb_query_and_lock_dev( dev_mgr, dev_handle, &pdev );
		if( status != STATUS_SUCCESS )
			return FALSE;

		pif_desc = ( PUSB_INTERFACE_DESC )&buf[ sizeof( USB_CONFIGURATION_DESC ) ];
		for( j = 0; j < pcfg_desc->bNumInterfaces; j++ )
		{
			if( pif_desc->bInterfaceClass == USB_CLASS_HUB
				&& pif_desc->bInterfaceSubClass == 0
				&& pif_desc->bNumEndpoints == 1 )
			{
				if( ( pif_desc->bInterfaceProtocol > 0 && pif_desc->bInterfaceProtocol < 3 )
					|| ( pif_desc->bInterfaceProtocol == 0 && pdev->flags & USB_DEV_FLAG_HIGH_SPEED )
					|| ( pif_desc->bInterfaceProtocol == 0 && !usb2( pdev->hcd ) ) )
				{
					found = TRUE;
					cfg_val = pcfg_desc->bConfigurationValue;
					break;
				}
			}
			if( usb_skip_if_and_altif( ( PBYTE* )&pif_desc ) == FALSE )
			{
				break;
			}
		}
		usb_unlock_dev( pdev );

		if( found )
			break;

		if( usb_skip_one_config( ( PBYTE* )&pcfg_desc ) == FALSE )
		{
			break;
		}

	}
	if( found )
	{
		purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
		if( purb == NULL )
			return 	FALSE;

		psetup = ( PUSB_CTRL_SETUP_PACKET )( purb )->setup_packet;
		urb_init( ( purb ) );

		purb->endp_handle		= endp_handle;
		purb->data_buffer		= NULL;
		purb->data_length		= 0;
		purb->completion		= hub_set_cfg_completion;
		purb->context			= dev_mgr;
		purb->reference			= ( LONG )param->pdriver;
		psetup->bmRequestType	= 0;
		psetup->bRequest		= USB_REQ_SET_CONFIGURATION;
		psetup->wValue			= ( USHORT )cfg_val;
		psetup->wIndex			= 0;
		psetup->wLength			= 0;

		status = usb_submit_urb( dev_mgr, purb );
		if( status != STATUS_PENDING )
		{
			usb_free_mem( purb );
			return FALSE;
		}
		return TRUE;
	}

	return FALSE;
}

void
hub_set_interface_completion(
PURB purb,
PVOID pcontext
);

void
hub_set_cfg_completion(
PURB purb,
PVOID pcontext
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_DRIVER pdriver;
	ULONG endp_handle, dev_handle;
	PUSB_CTRL_SETUP_PACKET psetup;
	UCHAR if_idx, if_alt_idx;
	PUSB_DEV pdev;
	PUSB_INTERFACE pif;
	BOOL high_speed, multiple_tt;
	NTSTATUS status;
	USE_IRQL;

	if( purb == NULL || pcontext == NULL )
		return;

	//pdev = NULL;
	dev_mgr = ( PUSB_DEV_MANAGER )pcontext;
	endp_handle = purb->endp_handle;
	dev_handle = endp_handle & 0xffff0000;
	pdriver = ( PUSB_DRIVER )purb->reference;
	high_speed = FALSE;
	multiple_tt = FALSE;

	if( purb->status != STATUS_SUCCESS )
	{
		goto LBL_ERROR;
	}

	status = usb_query_and_lock_dev( dev_mgr, purb->endp_handle, &pdev );
	if( status != STATUS_SUCCESS )
	{
		usb_unlock_dev( pdev );
		goto LBL_ERROR;
	}
	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		usb_unlock_dev( pdev );
		goto LBL_ERROR;
	}
	if( pdev->flags & USB_DEV_FLAG_HIGH_SPEED )
	{
		high_speed = TRUE;
		hub_if_from_dev( pdev, pif );
		if( pif->altif_count )
		{
			multiple_tt = TRUE;
			if_idx = pif - &pdev->usb_config->interf[ 0 ];
		}
	}
	unlock_dev( pdev, TRUE );
	usb_unlock_dev( pdev );

	if( !high_speed || !multiple_tt )
	{
		hub_set_interface_completion( purb, pcontext );
		return;
	}

	psetup = ( PUSB_CTRL_SETUP_PACKET )( purb )->setup_packet;
	urb_init( ( purb ) );

	// set the mult-tt if exist
	purb->endp_handle		= endp_handle;
	purb->data_buffer		= NULL;
	purb->data_length		= 0;
	purb->completion		= hub_set_interface_completion;
	purb->context			= dev_mgr;
	purb->reference			= ( LONG )pdriver;
	psetup->bmRequestType	= 0;
	psetup->bRequest		= USB_REQ_SET_INTERFACE;
	psetup->wValue			= ( USHORT )1;		// alternate tt
	psetup->wIndex			= if_idx;			// if index
	psetup->wLength			= 0;

	status = usb_submit_urb( dev_mgr, purb );
	if( status == STATUS_PENDING )
		return;

LBL_ERROR:
	usb_free_mem( purb );
	purb = NULL;
	return;
}

void
hub_set_interface_completion(
PURB purb,
PVOID pcontext
)
{

	PUSB_ENDPOINT pendp;
	NTSTATUS status;
	PUSB_CTRL_SETUP_PACKET psetup;
	PUSB_DEV_MANAGER dev_mgr;
	PBYTE  dev_ext;
	DEV_HANDLE endp_handle;
	PUSB_DRIVER pdriver;

	if( purb == NULL || pcontext == NULL )
		return;

	//pdev = NULL;
	dev_mgr = ( PUSB_DEV_MANAGER )pcontext;
	endp_handle = purb->endp_handle;
	pdriver = ( PUSB_DRIVER )purb->reference;

	if( purb->status != STATUS_SUCCESS )
	{
		usb_free_mem( purb );
		return;
	}

	dev_ext = usb_alloc_mem( NonPagedPool, sizeof( HUB2_EXTENSION ) );
	if( dev_ext == NULL )
	{
		goto LBL_OUT;
	}

	//
	//acquire hub descriptor
	//
	RtlZeroMemory( dev_ext, sizeof( HUB2_EXTENSION ) );
	urb_init( purb );

	purb->data_buffer = ( PUCHAR )&( ( HUB2_EXTENSION* ) dev_ext )->hub_desc;
	purb->endp_handle = endp_handle;
	purb->data_length = sizeof( USB_HUB_DESCRIPTOR );
	purb->completion = hub_get_hub_desc_completion;
	purb->context = ( PVOID )dev_mgr;
	purb->reference = ( ULONG )dev_ext;
	purb->pirp = ( PIRP )pdriver;

	psetup = ( PUSB_CTRL_SETUP_PACKET ) purb->setup_packet;
	psetup->bmRequestType = 0xa0;
	psetup->bRequest = USB_REQ_GET_DESCRIPTOR;
	psetup->wValue = ( 0x29 << 8 );
	psetup->wLength = sizeof( USB_HUB_DESCRIPTOR );
	status = usb_submit_urb( dev_mgr, purb );

	if( status != STATUS_PENDING )
	{
		usb_free_mem( dev_ext );
		goto LBL_OUT;
	}
	return;

 LBL_OUT:

	//clear the dev_driver fields in the dev.
	usb_free_mem( purb );
	return;
}


void
hub_power_on_port_completion(
PURB purb,
PVOID pcontext
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_DEV pdev;
	if( purb == NULL )
		return;
	if( pcontext == NULL )
		goto LBL_OUT;

	dev_mgr = ( PUSB_DEV_MANAGER ) pcontext;

	if( purb->status != STATUS_SUCCESS )
	{
		usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_power_on_port_completion(): port%d power on failed\n", purb->reference ) );
	}
	else
	{
		usb_dbg_print( DBGLVL_MAXIMUM, ( "hub_power_on_port_completion(): port%d power on succeed\n", purb->reference ) );
	}

LBL_OUT:
	usb_free_mem( purb );
	return;
}

NTSTATUS
hub_power_on_port(
PUSB_DEV pdev,
UCHAR port_idx
)
{

	PUSB_ENDPOINT pendp;
	NTSTATUS status;
	PUSB_CTRL_SETUP_PACKET psetup;
	PUSB_DEV_MANAGER dev_mgr;
	PBYTE  dev_ext;
	DEV_HANDLE endp_handle;
	PURB purb;
	PHCD hcd;

	USE_IRQL;
	if( pdev == NULL || port_idx == 0 )
		return STATUS_INVALID_PARAMETER;

	purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
	if( purb == NULL )
		return STATUS_NO_MEMORY;

	urb_init( purb );

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, FALSE );
		status = STATUS_DEVICE_DOES_NOT_EXIST;
		goto LBL_OUT;
	}
	dev_mgr = dev_mgr_from_dev( pdev );
	hcd = pdev->hcd;

	purb->completion = hub_power_on_port_completion;
	purb->context = ( PVOID )dev_mgr;
	purb->reference = ( ULONG )port_idx;
	purb->pdev = pdev;
	purb->pendp = &pdev->default_endp;

	psetup = ( PUSB_CTRL_SETUP_PACKET ) purb->setup_packet;
	psetup->bmRequestType = 0x23;
	psetup->bRequest = USB_REQ_SET_FEATURE;
	psetup->wValue = USB_PORT_FEAT_POWER;
	psetup->wIndex = ( WORD )port_idx;
	psetup->wLength = 0;

	unlock_dev( pdev, FALSE );

	status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb );

	if( status != STATUS_PENDING )
	{
		goto LBL_OUT;
	}
	return STATUS_PENDING;

 LBL_OUT:

	usb_free_mem( purb );
	return status;

}

void
hub_get_hub_desc_completion(
PURB purb,
PVOID pcontext
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PHUB2_EXTENSION hub_ext;
	PUSB_DEV pdev;
	LONG i;
	PUSB_INTERFACE pif;
	ULONG status;
	LONG port_count;
	PUSB_DRIVER pdriver;
	DEV_HANDLE dev_handle;

	USE_IRQL;

	if( purb == NULL )
	{
		return;
	}

	dev_mgr = ( PUSB_DEV_MANAGER ) pcontext;
	hub_ext = ( PHUB2_EXTENSION )purb->reference;
	pdriver = ( PUSB_DRIVER )purb->pirp;
	dev_handle = purb->endp_handle & 0xffff0000;

	if( pcontext == NULL || purb->reference == 0 )
		goto LBL_OUT;

	if( purb->status != STATUS_SUCCESS )
	{
		goto LBL_OUT;
	}

	// obtain the pointer to the dev
	status = usb_query_and_lock_dev( dev_mgr, purb->endp_handle, &pdev );
	if( status != STATUS_SUCCESS )
	{
		usb_unlock_dev( pdev );
		goto LBL_OUT;
	}
	// safe to release the pdev ref since we are in urb completion
	usb_unlock_dev( pdev );

	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB ||
		dev_mgr_set_driver( dev_mgr, dev_handle, pdriver, pdev ) == FALSE )
	{
		unlock_dev( pdev, TRUE );
		goto LBL_OUT;
	}

	//transit the state to configured
	pdev->flags &= ~USB_DEV_STATE_MASK;
	pdev->flags |= USB_DEV_STATE_CONFIGURED;

	pdev->dev_ext = ( PBYTE )hub_ext;
	pdev->dev_ext_size = sizeof( HUB2_EXTENSION );

	port_count = hub_ext->port_count = hub_ext->hub_desc.bNbrPorts;
	hub_ext->pdev = pdev;
	for( i = 0; i < pdev->usb_config->if_count; i++ )
	{
		pif = &pdev->usb_config->interf[ i ];
		if( pif->pusb_if_desc->bInterfaceClass == USB_CLASS_HUB
			&& pif->pusb_if_desc->bInterfaceSubClass == 0
			&& pif->pusb_if_desc->bInterfaceProtocol < 3 
			&& pif->pusb_if_desc->bNumEndpoints == 1 )
		{
			hub_ext->pif = pif;
			break;
		}
	}
	for( i = 0; i < MAX_HUB_PORTS + 1; i++ )
	{
		psq_init( ( PPORT_STATUS_QUEUE )hub_ext->port_status_queue );
	}

	hub_ext->multiple_tt = ( pif->pusb_if_desc->bInterfaceProtocol == 2 );

	unlock_dev( pdev, TRUE );
	usb_free_mem( purb );

	hub_start_int_request( pdev );

	for( i = 0; i < port_count; i ++ )
	{
		hub_power_on_port( pdev, ( UCHAR )( i + 1 ) );
	}
	return;

 LBL_OUT:
	if( purb )
		usb_free_mem( purb );

	if( hub_ext )
		usb_free_mem( hub_ext );
	return;
}

BOOL
hub_stop(
PUSB_DEV_MANAGER dev_mgr,
DEV_HANDLE dev_handle
)
{
	return TRUE;
}

BOOL
hub_disconnect(
PUSB_DEV_MANAGER dev_mgr,
DEV_HANDLE dev_handle
)
{
	PUSB_DEV pdev;
	//special use of usb_query and lock dev
	if( usb_query_and_lock_dev( dev_mgr, dev_handle, &pdev ) != STATUS_SUCCESS )
	{
		//will never be success, since the dev is already in zomb state
		//at this point, the dev is valid, ref_count is of none use,
		//no need to lock it
		if( pdev )
		{
			usb_free_mem( pdev->dev_ext );
		}
	}

	return TRUE;
}

static BOOL
hub_lock_unlock_tt(
PUSB_DEV pdev,
UCHAR port_idx,
UCHAR type,
BOOL lock
)
{
	PUSB_INTERFACE pif;
	PHUB2_EXTENSION dev_ext;
	PULONG pmap;

	USE_IRQL;

	if( pdev == NULL || port_idx > 127 )
		return FALSE;

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, FALSE );
		return FALSE;
	}

	dev_ext = hub_ext_from_dev( pdev );
	if( dev_ext == NULL )
	{
		unlock_dev( pdev, FALSE );
		return FALSE;
	}
	if( type == USB_ENDPOINT_XFER_INT || \
		type == USB_ENDPOINT_XFER_ISOC )
	{
		pmap = dev_ext->tt_status_map;
	}
	else if( type == USB_ENDPOINT_XFER_BULK || \
			type == USB_ENDPOINT_XFER_CONTROL )
	{
		pmap = dev_ext->tt_bulk_map;
	}

	if( lock )
	{ 
		if( pmap[ port_idx >> 5 ] & ( 1 << port_idx ) )
		{
			unlock_dev( pdev, FALSE );
			return FALSE;
		}
		pmap[ port_idx >> 5 ] |= ( 1 << port_idx );
	}
	else
	{
		pmap[ port_idx >> 5 ] &= ~( 1 << port_idx );
	}

	unlock_dev( pdev, FALSE );
	return TRUE;
}

BOOL
hub_lock_tt(
PUSB_DEV pdev,
UCHAR port_idx,
UCHAR type   // transfer type
)
{
	return hub_lock_unlock_tt( pdev, port_idx, type, TRUE );
}

BOOL
hub_unlock_tt(
PUSB_DEV pdev,
UCHAR port_idx,
UCHAR type
)
{
	return hub_lock_unlock_tt( pdev, port_idx, type, FALSE );
}

VOID
hub_clear_tt_buffer_completion(
PURB purb,
PVOID context
)
{
	PUSB_CTRL_SETUP_PACKET psetup;
	PURB_HS_PIPE_CONTENT pipe_content;
	PHUB2_EXTENSION hub_ext;
	PHCD hcd;

	if( purb == NULL || context == NULL )
		return;

	hub_ext = ( PHUB2_EXTENSION )context;
	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;
	pipe_content = ( PURB_HS_PIPE_CONTENT )&purb->reference;
	hub_unlock_tt( purb->pdev, ( UCHAR )psetup->wIndex, ( UCHAR )pipe_content->trans_type );
	usb_free_mem( purb );
	purb = NULL;
	hcd = hub_ext->pdev->hcd;

	// let those blocked urbs ( sharing the same tt )have chance to be scheduled
	if( hcd && usb2( hcd ) )
		hcd->hcd_submit_urb( hcd, NULL, NULL, NULL );

	return;
}

BOOL
hub_clear_tt_buffer(
PUSB_DEV pdev,
URB_HS_PIPE_CONTENT pipe_content,
UCHAR port_idx
)
// send CLEAR_TT_BUFFER to the hub
{
	PURB purb;
	PUSB_CTRL_SETUP_PACKET psetup;
	PHUB2_EXTENSION hub_ext;
	PHCD hcd;
	NTSTATUS status;
	USE_IRQL;

	if( pdev == NULL )
		return FALSE;

	if( pipe_content.speed_high )
		return FALSE;

	lock_dev( pdev, FALSE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, FALSE );
		return FALSE;
	}

	hub_ext = hub_ext_from_dev( pdev );
	if( hub_ext == NULL )
	{
		unlock_dev( pdev, FALSE );
		return FALSE;
	}
	purb = usb_alloc_mem( NonPagedPool, sizeof( URB ) );
	RtlZeroMemory( purb, sizeof( URB ) );

	if( purb == NULL )
	{
		unlock_dev( pdev, FALSE );
		return STATUS_NO_MEMORY;
	}

    purb->flags = 0;
	purb->status = STATUS_SUCCESS;
	purb->data_buffer = NULL;
    purb->data_length = 0; // ( hub_ext->port_count + 7 ) / 8;

	// hub_if_from_dev( pdev, pif );
	purb->pendp = &pdev->default_endp;
	purb->pdev = pdev;

    purb->completion = hub_clear_tt_buffer_completion;
    purb->context = hub_ext;
	purb->reference = *( ( PLONG )&pipe_content );

    purb->pirp = NULL;
	hcd = pdev->hcd;

	psetup = ( PUSB_CTRL_SETUP_PACKET )purb->setup_packet;
	psetup->bmRequestType = 0x23; //host-device class other recepient
	psetup->bRequest = HUB_REQ_CLEAR_TT_BUFFER;
	psetup->wValue = ( USHORT )( ( pipe_content.endp_addr ) | ( pipe_content.dev_addr << 4 )| \
			( pipe_content.trans_type << 10 ) | ( pipe_content.trans_dir << 15 ) );

	if( hub_ext->multiple_tt )
	{
		psetup->wIndex = ( USHORT )port_idx;
	}
	else
		psetup->wIndex = 1;

	psetup->wLength = 0;
	unlock_dev( pdev, FALSE );

	status = hcd->hcd_submit_urb( hcd, pdev, purb->pendp, purb );
	if( status != STATUS_PENDING )
	{
		usb_free_mem( purb );
		purb = NULL;
		return FALSE;
	}
	return TRUE;
}

BOOL
hub_event_clear_tt_buffer(
PUSB_DEV pdev,		//always null. we do not use this param
ULONG event,
ULONG context,
ULONG param
)
{
	hub_clear_tt_buffer( pdev, *( ( PURB_HS_PIPE_CONTENT )&context ), ( UCHAR )param );
	return TRUE;
}

VOID
hub_post_clear_tt_event(
PUSB_DEV pdev,
BYTE port_idx,
ULONG pipe
)
{
	PUSB_DEV_MANAGER dev_mgr;
	PUSB_EVENT pevent;
	USE_IRQL;

	dev_mgr = dev_mgr_from_dev( pdev );

	KeAcquireSpinLock( &dev_mgr->event_list_lock, &old_irql );
	lock_dev( pdev, TRUE );
	if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
	{
		unlock_dev( pdev, TRUE );
		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
		return;
	}
	pevent = alloc_event( &dev_mgr->event_pool, 1 );
	if( pevent == NULL )
	{
		unlock_dev( pdev, TRUE );
		KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );
		TRAP();
		return;
	}

	pevent->event = USB_EVENT_WAIT_RESET_PORT;
	pevent->pdev = pdev;
	pevent->context = pipe;
	pevent->param = port_idx;
	pevent->flags = USB_EVENT_FLAG_ACTIVE;
	pevent->process_queue = event_list_default_process_queue;
	pevent->process_event = hub_event_clear_tt_buffer;
	pevent->pnext = NULL;
	InsertTailList( &dev_mgr->event_list, &pevent->event_link );
	
	unlock_dev( pdev, TRUE );
	KeReleaseSpinLock( &dev_mgr->event_list_lock, old_irql );

	KeSetEvent( &dev_mgr->wake_up_event, 0, FALSE );
	return;
}

BOOL
init_irp_list(
PIRP_LIST irp_list
)
{
	LONG i;
	KeInitializeSpinLock( &irp_list->irp_list_lock );
	InitializeListHead( &irp_list->irp_busy_list );
	InitializeListHead( &irp_list->irp_free_list );
	irp_list->irp_list_element_array = usb_alloc_mem( NonPagedPool, sizeof( IRP_LIST_ELEMENT ) * MAX_IRP_LIST_SIZE );

	if( irp_list->irp_list_element_array == NULL )
		return FALSE;

	RtlZeroMemory( irp_list->irp_list_element_array, sizeof( IRP_LIST_ELEMENT ) * MAX_IRP_LIST_SIZE );
	for( i = 0; i < MAX_IRP_LIST_SIZE; i++ )
	{
		InsertTailList( &irp_list->irp_free_list, &irp_list->irp_list_element_array[ i ].irp_link );
	}
	irp_list->irp_free_list_count = MAX_IRP_LIST_SIZE;
	return TRUE;
}

VOID
destroy_irp_list(
PIRP_LIST irp_list
)
{
	InitializeListHead( &irp_list->irp_busy_list );
	InitializeListHead( &irp_list->irp_free_list );
	usb_free_mem( irp_list->irp_list_element_array );
	irp_list->irp_list_element_array = NULL;
	irp_list->irp_free_list_count = 0;
	return;
}

BOOL
add_irp_to_list(
PIRP_LIST irp_list,
PIRP pirp,
PURB purb
)
{
	KIRQL	old_irql;
	PIRP_LIST_ELEMENT pile;

	if( irp_list == NULL || pirp == NULL || purb == NULL )
		return FALSE;

	IoAcquireCancelSpinLock( &old_irql );
	KeAcquireSpinLockAtDpcLevel( &irp_list->irp_list_lock );

	if( irp_list->irp_free_list_count == 0 )
	{
		KeReleaseSpinLockFromDpcLevel( &irp_list->irp_list_lock );
		IoReleaseCancelSpinLock( old_irql );
		return FALSE;
	}
	pile = ( PIRP_LIST_ELEMENT )RemoveHeadList( &irp_list->irp_free_list );

	pile->pirp = pirp;
	pile->purb = purb;

	irp_list->irp_free_list_count--;
	InsertTailList( &irp_list->irp_busy_list, &pile->irp_link );
	IoSetCancelRoutine( pirp, dev_mgr_cancel_irp );

	KeReleaseSpinLockFromDpcLevel( &irp_list->irp_list_lock );
	IoReleaseCancelSpinLock( old_irql );
	return TRUE;
}

PURB
remove_irp_from_list(
PIRP_LIST irp_list,
PIRP pirp,
PUSB_DEV_MANAGER dev_mgr //if dev_mgr is not NULL, the urb needs to be canceled
)
{
	PIRP_LIST_ELEMENT pile;
	PLIST_ENTRY pthis, pnext;
	PURB purb;
	DEV_HANDLE endp_handle;
	PUSB_DEV pdev;
	PUSB_ENDPOINT pendp;
	PHCD hcd;

	USE_IRQL;

	if( irp_list == NULL || pirp == NULL )
		return NULL;

	KeAcquireSpinLock( &irp_list->irp_list_lock, &old_irql );

	if( irp_list->irp_free_list_count == MAX_IRP_LIST_SIZE )
	{
		KeReleaseSpinLock( &irp_list->irp_list_lock, old_irql );
		return NULL;
	}

	purb = NULL;
	ListFirst( &irp_list->irp_busy_list, pthis );
	while( pthis )
	{
		pile = struct_ptr( pthis, IRP_LIST_ELEMENT, irp_link );
		if( pile->pirp == pirp )
		{
			purb = pile->purb;
			pile->pirp = NULL;
			pile->purb = NULL;
			RemoveEntryList( pthis );
			InsertTailList( &irp_list->irp_free_list, pthis );
			irp_list->irp_free_list_count++;
			break;
		}
		ListNext( &irp_list->irp_busy_list, pthis, pnext );
		pthis = pnext;
	}

	if( purb == NULL )
	{
		// not found
		KeReleaseSpinLock( &irp_list->irp_list_lock, old_irql );
		return NULL;
	}

	endp_handle = purb->endp_handle;
	KeReleaseSpinLock( &irp_list->irp_list_lock, old_irql );

	if( dev_mgr )
	{
		// indicate we needs to cancel the urb, this condition happens only in cancel routine
		// we should notice that even the hcd_cancel_urb is called, the irp may not be canceled
		// if the urb does not exist in any queue of the host controller driver, indicating
		// it is being processed by dpc. Thus, the dpc will certainly prevent the irp in
		// completion from being canceled at the same time. On the other hand, if the
		// hcd_cancel_urb succeeds, it either directly complete the irp or queue the dpc for
		// irp completion. So, there won't be two simutaneous threads processing the same
		// irp.

		if( usb_query_and_lock_dev( dev_mgr, endp_handle, &pdev ) != STATUS_SUCCESS )
			return NULL;

		lock_dev( pdev, TRUE );
		if( dev_state( pdev ) == USB_DEV_STATE_ZOMB )
		{
			unlock_dev( pdev, FALSE );
			usb_unlock_dev( pdev );
			return NULL;
		}

		hcd = pdev->hcd;
		endp_from_handle( pdev, endp_handle, pendp );
		unlock_dev( pdev, TRUE );
		hcd->hcd_cancel_urb( hcd, pdev, pendp, purb );
		usb_unlock_dev( pdev );
		return NULL;
	}
	return purb;
}

BOOL
irp_list_empty(
PIRP_LIST irp_list
)
{
	KIRQL old_irql;
	BOOL ret;
	KeAcquireSpinLock( &irp_list->irp_list_lock, &old_irql );
	ret = ( irp_list->irp_free_list_count == MAX_IRP_LIST_SIZE );
	KeReleaseSpinLock( &irp_list->irp_list_lock, old_irql );
	return ret;
}

BOOL
irp_list_full(
PIRP_LIST irp_list
)
{
	KIRQL old_irql;
	BOOL ret;
	KeAcquireSpinLock( &irp_list->irp_list_lock, &old_irql );
	ret = ( irp_list->irp_free_list_count == 0 );
	KeReleaseSpinLock( &irp_list->irp_list_lock, old_irql );
	return ret;
}


VOID
zzz()
{};
