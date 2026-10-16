/*
 * This file is part of the DSView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * USB hotplug for Windows, installed through lib_extern_init().
 *
 * libusb has no hotplug on Windows unless it was built with
 * --enable-windows-hotplug, which no distribution does. The configuration
 * manager reports USB device interface arrival and removal on a thread pool
 * thread instead; the callback only counts the events of DreamSourceLab
 * devices. The hotplug thread consumes them in hotplug_wait_timout_ext() and
 * forwards each with a NULL device, so lib_main finds the device by
 * comparing the libusb device lists.
 */

/* CM_Register_Notification() needs Windows 8. */
#undef WINVER
#undef _WIN32_WINNT
#define WINVER 0x0602
#define _WIN32_WINNT 0x0602

#include "../libsigrok-internal.h"
#include "../log.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>
#include <wchar.h>

#undef LOG_PREFIX
#define LOG_PREFIX "hotplug_win: "

/*
 * An arrived device is reported once no other arrival came for this long:
 * right after the interface arrives, WinUSB may not be ready, and reading
 * the descriptors times out.
 */
#define ATTACH_SETTLE_MS	300

static HCMNOTIFICATION notify_handle = NULL;
static volatile LONG pending_attach = 0;
static volatile LONG pending_detach = 0;
static volatile LONG64 last_arrival_ms = 0;

static int is_ds_device(const WCHAR *symbolic_link)
{
	WCHAR vid[16];
	WCHAR link[512];

	swprintf(vid, sizeof(vid) / sizeof(vid[0]), L"VID_%04X", DS_VENDOR_ID);
	wcsncpy(link, symbolic_link, sizeof(link) / sizeof(link[0]) - 1);
	link[sizeof(link) / sizeof(link[0]) - 1] = L'\0';
	return wcsstr(_wcsupr(link), vid) != NULL;
}

static DWORD CALLBACK notify_callback(HCMNOTIFICATION notify, PVOID context,
		CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD size)
{
	(void)notify;
	(void)context;
	(void)size;

	if (data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE
		|| !is_ds_device(data->u.DeviceInterface.SymbolicLink))
		return ERROR_SUCCESS;

	if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
		InterlockedExchange64(&last_arrival_ms, (LONG64)GetTickCount64());
		InterlockedIncrement(&pending_attach);
	}
	else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
		InterlockedIncrement(&pending_detach);
	}

	return ERROR_SUCCESS;
}

static int listen_hotplug_win(struct sr_context *ctx)
{
	CM_NOTIFY_FILTER filter;
	CONFIGRET cr;

	(void)ctx;

	memset(&filter, 0, sizeof(filter));
	filter.cbSize = sizeof(filter);
	filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
	filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;

	cr = CM_Register_Notification(&filter, NULL, notify_callback, &notify_handle);
	if (cr != CR_SUCCESS) {
		sr_err("CM_Register_Notification() failed, error:%lu.", (unsigned long)cr);
		notify_handle = NULL;
		return SR_ERR;
	}

	sr_info("Listening to USB device interface notifications.");
	return SR_OK;
}

static int close_hotplug_win(struct sr_context *ctx)
{
	(void)ctx;

	if (notify_handle != NULL) {
		CM_Unregister_Notification(notify_handle);
		notify_handle = NULL;
	}
	return SR_OK;
}

static int hotplug_wait_timout_win(struct sr_context *ctx)
{
	LONG64 settled;

	/* Kept from the default implementation, handles the pending libusb events. */
	libusb_handle_events_timeout(ctx->libusb_ctx, &ctx->hotplug_tv);

	if (notify_handle == NULL)
		return SR_OK;

	/*
	 * One event per call, removal first: lib_main treats a removal
	 * followed by an arrival within 500ms as a reconnection.
	 */
	if (pending_detach > 0) {
		InterlockedDecrement(&pending_detach);
		sr_info("A device was removed.");
		ctx->hotplug_callback(ctx->libusb_ctx, NULL, USB_EV_HOTPLUG_DETTACH);
	}
	else if (pending_attach > 0) {
		settled = (LONG64)GetTickCount64() - InterlockedCompareExchange64(&last_arrival_ms, 0, 0);
		if (settled >= ATTACH_SETTLE_MS) {
			InterlockedDecrement(&pending_attach);
			sr_info("A device arrived.");
			ctx->hotplug_callback(ctx->libusb_ctx, NULL, USB_EV_HOTPLUG_ATTACH);
		}
	}
	return SR_OK;
}

SR_PRIV int lib_extern_init(struct sr_context *ctx)
{
	ctx->listen_hotplug_ext = listen_hotplug_win;
	ctx->close_hotplug_ext = close_hotplug_win;
	ctx->hotplug_wait_timout_ext = hotplug_wait_timout_win;
	return SR_OK;
}
