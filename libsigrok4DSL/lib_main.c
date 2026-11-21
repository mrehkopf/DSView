/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "libsigrok-internal.h"
#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

#define MAX_DEVCIE_LIST_LENGTH  20

// Hubs below the root port a USB device can sit behind, plus its own port.
#define MAX_USB_PORT_DEPTH 7

// Handle hotplug events once none has arrived for 200ms (ticks of the hotplug thread loop).
#define HOTPLUG_SETTLE_TICKS 2

#undef LOG_PREFIX
#define LOG_PREFIX "lib_main: "

// Have been encoding.
char DS_RES_PATH[500];

char DS_USR_PATH[500];

struct sr_lib_context
{
	dslib_event_callback_t event_callback;
	struct sr_context *sr_ctx;
	GSList *device_list; // All device instance, sr_dev_inst* type
	pthread_mutex_t mutext;
	int lib_exit_flag;
	/* Hotplug events not handled yet, set by the hotplug callback on
	 * whichever thread handles libusb events. Guarded by hotplug_mutex. */
	pthread_mutex_t hotplug_mutex;
	int hotplug_attach_pending;
	int hotplug_detach_pending;
	int hotplug_quiet_ticks;
	struct sr_dev_inst *actived_device_instance;
	GThread *hotplug_thread;
	GThread *collect_thread;
	GSList *collect_device_list;
	struct ds_collect_target *collect_targets;
	int collect_target_count;
	ds_datafeed_callback_t data_forward_callback;
	int callback_thread_count;
	int is_delay_destory_actived_device;
	int is_stop_by_detached;
	int last_error;
	int is_reloading_list;
};

static void hotplug_event_listen_callback(struct libusb_context *ctx, struct libusb_device *dev, int event);
static gpointer usb_hotplug_process_proc(gpointer data);
static void destroy_device_instance(struct sr_dev_inst *dev);
static void close_device_instance(struct sr_dev_inst *dev);
static int open_device_instance(struct sr_dev_inst *dev);
static int ensure_device_instance_open(struct sr_dev_inst *dev);
static gpointer collect_run_proc(gpointer data);
static gpointer collect_multi_run_proc(gpointer data);
static void post_event_async(int event);
static void send_event(int event);
static void make_demo_device_to_list();
static void process_attach_event(int isEvent);
static void remove_detached_devices(void);
static struct libusb_device* get_new_attached_usb_device();
static struct libusb_device* get_new_detached_usb_device();
static void get_device_unique_id(const struct sr_dev_inst *dev, char *buf, size_t len);
static struct sr_dev_inst *find_device_instance_by_handle(ds_device_handle handle);
static void fill_device_full_info(struct sr_dev_inst *dev, struct ds_device_full_info *fill_info);
static int start_collect_target(struct sr_dev_inst *di, const struct ds_collect_target *target,
								const struct ds_trigger *master_trigger);
static int stop_collect_device_list(void);
static void clear_collect_devices(void);

static struct sr_lib_context lib_ctx = {
	.event_callback = NULL,
	.sr_ctx = NULL,
	.device_list = NULL,
	.hotplug_thread = NULL,
	.lib_exit_flag = 0,
	.hotplug_attach_pending = 0,
	.hotplug_detach_pending = 0,
	.hotplug_quiet_ticks = 0,
	.actived_device_instance = NULL,
	.data_forward_callback = NULL,
	.collect_thread = NULL,
	.collect_device_list = NULL,
	.collect_targets = NULL,
	.collect_target_count = 0,
	.callback_thread_count = 0,
	.is_delay_destory_actived_device = 0,
	.is_stop_by_detached = 0,
	.last_error = SR_OK,
	.is_reloading_list = 0,
};

/**
 * Must call first
 */
SR_API int ds_lib_init()
{
	int ret = 0;
	struct sr_dev_driver **drivers = NULL;
	struct sr_dev_driver **dr = NULL;

	if (lib_ctx.sr_ctx != NULL)
	{
		return SR_ERR_HAVE_DONE;
	}

	sr_log_init(); // try init log

	sr_info("Init %s.", SR_LIB_NAME);

	ret = sr_init(&lib_ctx.sr_ctx);
	if (ret != SR_OK)
	{
		return ret;
	}
	lib_ctx.lib_exit_flag = 0;

	// Init trigger.
	ds_trigger_init();

	// Initialise all libsigrok drivers
	drivers = sr_driver_list();
	for (dr = drivers; *dr; dr++)
	{
		if (sr_driver_init(lib_ctx.sr_ctx, *dr) != SR_OK)
		{
			sr_err("Failed to initialize driver '%s'", (*dr)->name);
			return SR_ERR;
		}
	}
	pthread_mutex_init(&lib_ctx.mutext, NULL); // init locker
	pthread_mutex_init(&lib_ctx.hotplug_mutex, NULL); // before the hotplug callback is registered

	make_demo_device_to_list();

	lib_ctx.sr_ctx->hotplug_tv.tv_sec = 0;
	lib_ctx.sr_ctx->hotplug_tv.tv_usec = 0;
	
	/**
	 * User can use custom hotplug function.
	*/
	lib_ctx.sr_ctx->listen_hotplug_ext = NULL;
	lib_ctx.sr_ctx->close_hotplug_ext = NULL;
	lib_ctx.sr_ctx->hotplug_wait_timout_ext = NULL;

#ifdef _WIN32
	#ifdef HAVE_EXTERN_INIT
		lib_extern_init(lib_ctx.sr_ctx);
	#endif
#endif

	// Scan the all hardware device.
	sr_info("Scan all connected hardware device.");
	process_attach_event(0);

	sr_listen_hotplug(lib_ctx.sr_ctx, hotplug_event_listen_callback);

	/** Start usb hotplug thread */
	lib_ctx.hotplug_thread = g_thread_new("hotplug_proc", usb_hotplug_process_proc, (gpointer)0);

	return SR_OK;
}

#ifndef _WIN32
SR_PRIV int lib_extern_init(struct sr_context *ctx){
	(void)ctx;
	return SR_OK;
}
#endif

/**
 * Free all resource before program exits
 */
SR_API int ds_lib_exit()
{
	GSList *l;

	if (lib_ctx.sr_ctx == NULL)
	{
		return SR_ERR_HAVE_DONE;
	}

	sr_info("Uninit %s.", SR_LIB_NAME);

	ds_release_actived_device();
	sr_close_hotplug(lib_ctx.sr_ctx);

	lib_ctx.lib_exit_flag = 1; // all thread to exit

	if (lib_ctx.hotplug_thread != NULL)
	{
		g_thread_join(lib_ctx.hotplug_thread);
		lib_ctx.hotplug_thread = NULL;
	}

	// The device is not in list.
	if (lib_ctx.is_delay_destory_actived_device && lib_ctx.actived_device_instance != NULL)
	{
		sr_info("The current device is delayed for destruction, handle:%p", lib_ctx.actived_device_instance->handle);
		destroy_device_instance(lib_ctx.actived_device_instance);
		lib_ctx.actived_device_instance = NULL;
		lib_ctx.is_delay_destory_actived_device = 0;
	}

	// Release all device
	for (l = lib_ctx.device_list; l; l = l->next)
	{
		destroy_device_instance((struct sr_dev_inst *)l->data);
	}
	g_safe_free_list(lib_ctx.device_list);

	pthread_mutex_destroy(&lib_ctx.mutext); // uninit locker

	// Uninit trigger.
	ds_trigger_destroy();

	if (sr_exit(lib_ctx.sr_ctx) != SR_OK)
	{
		sr_err("call sr_exit error");
	}
	lib_ctx.sr_ctx = NULL;

	sr_hw_cleanup_all();

	sr_log_uninit(); // try uninit log

	return SR_OK;
}

/**
 * Set the firmware binary file directory
 */
SR_API void ds_set_firmware_resource_dir(const char *dir)
{  
	memset(DS_RES_PATH, 0, sizeof(DS_RES_PATH));
	if (dir && strlen(dir) >= sizeof(DS_RES_PATH)) {
		/* Refuse rather than overflow or truncate; firmware loading will report the missing path. */
		sr_err("Firmware resource path is too long (%u bytes, limit %u).",
			(unsigned)strlen(dir), (unsigned)(sizeof(DS_RES_PATH) - 1));
		return;
	}
	if (dir)
		strcpy(DS_RES_PATH, dir);
}

/**
 * Set user data directory.
*/
SR_API void ds_set_user_data_dir(const char *dir)
{ 
	memset(DS_USR_PATH, 0, sizeof(DS_USR_PATH));
	if (dir && strlen(dir) >= sizeof(DS_USR_PATH)) {
		sr_err("User data path is too long (%u bytes, limit %u).",
			(unsigned)strlen(dir), (unsigned)(sizeof(DS_USR_PATH) - 1));
		return;
	}
	if (dir)
		strcpy(DS_USR_PATH, dir);
}

/**
 * Set event callback, event type see enum libsigrok_event_type
 */
SR_API void ds_set_event_callback(dslib_event_callback_t cb)
{
	lib_ctx.event_callback = cb;
}

/**
 * Set the data receive callback.
 */
SR_API void ds_set_datafeed_callback(ds_datafeed_callback_t cb)
{
	lib_ctx.data_forward_callback = cb;
}

SR_API int ds_get_device_list(struct ds_device_base_info **out_list, int *out_count)
{
	int num;
	struct ds_device_base_info *p = NULL;
	GSList *l;
	struct sr_dev_inst *dev;
	void *buf = NULL;

	if (out_list == NULL)
	{
		return SR_ERR_ARG;
	}
	*out_list = NULL;

	pthread_mutex_lock(&lib_ctx.mutext);

	num = g_slist_length(lib_ctx.device_list);
	if (num == 0)
	{
		pthread_mutex_unlock(&lib_ctx.mutext);
		return SR_OK;
	}

	buf = g_try_malloc0(sizeof(struct ds_device_base_info) * (num + 1));
	if (buf == NULL)
	{	
		sr_err("%s,ERROR:failed to alloc memory.", __func__);
		pthread_mutex_unlock(&lib_ctx.mutext);
		return SR_ERR_MALLOC;
	}

	p = (struct ds_device_base_info*)buf;

	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		p->handle = dev->handle;
		strncpy(p->name, (const char*)dev->name, sizeof(p->name) - 1);
		get_device_unique_id(dev, p->unique_id, sizeof(p->unique_id));
		p++;
	}

	p->handle = 0; // is the end
	p->name[0] = '\0';
	p->unique_id[0] = '\0';

	if (out_count)
	{
		*out_count = num;
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	*out_list = (struct ds_device_base_info*)buf;
	return SR_OK;
}

#ifdef __APPLE__
static int darwin_get_number_property(io_service_t service, CFStringRef key, SInt64 *value)
{
	CFTypeRef ref;
	int ok;

	ok = 0;
	ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);

	if (ref != NULL){
		if (CFGetTypeID(ref) == CFNumberGetTypeID())
			ok = CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, value) ? 1 : 0;
		CFRelease(ref);
	}

	return ok;
}

/**
 * The ports macOS lists in the location ID of the USB device at this bus and
 * address. The location ID holds the bus number in its top byte and then one
 * nibble per port from the root, and always names the full path, where the
 * port numbers libusb collects end at the first hub it doesn't know.
 */
static int darwin_get_port_numbers(libusb_device *usb_dev, uint8_t *ports, int size)
{
	static const char *classes[] = {"IOUSBHostDevice", "IOUSBDevice"};
	io_iterator_t iter;
	io_service_t service;
	SInt64 location;
	SInt64 address;
	uint32_t found;
	int have_found;
	int shift;
	int count;
	unsigned int c;

	have_found = 0;
	found = 0;

	for (c = 0; c < sizeof(classes) / sizeof(classes[0]) && !have_found; c++)
	{
		// The matching dictionary is consumed by IOServiceGetMatchingServices().
		if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(classes[c]), &iter) != KERN_SUCCESS)
			continue;

		while (!have_found && (service = IOIteratorNext(iter)) != 0)
		{
			if (darwin_get_number_property(service, CFSTR("locationID"), &location)
				&& darwin_get_number_property(service, CFSTR("USB Address"), &address)
				&& (((uint32_t)location >> 24) & 0xFF) == libusb_get_bus_number(usb_dev)
				&& address == libusb_get_device_address(usb_dev))
			{
				found = (uint32_t)location;
				have_found = 1;
			}
			IOObjectRelease(service);
		}
		IOObjectRelease(iter);
	}

	if (!have_found)
		return 0;

	count = 0;
	for (shift = 20; shift >= 0 && count < size; shift -= 4)
	{
		if (((found >> shift) & 0xF) == 0)
			break;
		ports[count++] = (uint8_t)((found >> shift) & 0xF);
	}

	return count;
}
#endif

/**
 * The USB port path of a device, as "USB <bus>-<port>.<port>...", named
 * like the Linux sysfs device. It stays the same while the device is plugged
 * into the same port, also across reconnects and restarts.
 */
static int get_usb_port_path(libusb_device *usb_dev, char *buf, size_t len)
{
	uint8_t ports[MAX_USB_PORT_DEPTH];
	int count;
	int i;
	size_t pos;

	if (usb_dev == NULL)
		return SR_ERR_ARG;

	count = 0;
#ifdef __APPLE__
	count = darwin_get_port_numbers(usb_dev, ports, MAX_USB_PORT_DEPTH);
#endif
	if (count <= 0)
		count = libusb_get_port_numbers(usb_dev, ports, MAX_USB_PORT_DEPTH);

	if (count <= 0)
		return SR_ERR;

	pos = (size_t)snprintf(buf, len, "USB %u-", libusb_get_bus_number(usb_dev));

	for (i = 0; i < count && pos < len; i++)
		pos += (size_t)snprintf(buf + pos, len - pos, i == 0 ? "%u" : ".%u", ports[i]);

	return pos < len ? SR_OK : SR_ERR;
}

static void get_device_unique_id(const struct sr_dev_inst *dev, char *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return;

	*buf = 0;

	if (dev == NULL || dev->conn == NULL)
		return;

	if (dev->dev_type == DEV_TYPE_USB)
	{
		/* The DSLogic devices have no serial number in their device descriptor,
		   so name the port they are plugged into. Without a port path, fall
		   back to the bus and address, which change on every reconnect. */
		const struct sr_usb_dev_inst *usb = dev->conn;
		libusb_device *usb_dev = usb->usb_dev != NULL ? usb->usb_dev : (libusb_device *)dev->handle;

		if (get_usb_port_path(usb_dev, buf, len) != SR_OK)
			snprintf(buf, len, "USB %u.%u", usb->bus, usb->address);
	}
	else if (dev->dev_type == DEV_TYPE_SERIAL)
	{
		/* probably never used... */
		const struct sr_serial_dev_inst *serial = dev->conn;
		if (serial->port != NULL && serial->port[0] != '\0')
			snprintf(buf, len, "%s", serial->port);
	}
}

/**
 * Active a device.
 */
SR_API int ds_active_device(ds_device_handle handle)
{
	GSList *l;
	struct sr_dev_inst *dev;
	int bFind = 0;
	int ret;
	struct sr_dev_inst *old_dev;

	lib_ctx.last_error = SR_OK;

	if (handle == NULL_HANDLE)
	{
		return SR_ERR_ARG;
	}

	ret = SR_OK;
	old_dev = NULL;

	sr_info("Start activating device.");

	if (ds_is_collecting())
	{
		sr_err("Error!the current device is collecting,can not switch to it.");
		return SR_ERR_CALL_STATUS;
	}

	pthread_mutex_lock(&lib_ctx.mutext);

	if (lib_ctx.actived_device_instance != NULL)
	{
		if (lib_ctx.is_delay_destory_actived_device)
		{
			sr_info("The current device is delayed for destruction, name:\"%s\", handle:%p",
				lib_ctx.actived_device_instance->name,
				lib_ctx.actived_device_instance->handle);
			destroy_device_instance(lib_ctx.actived_device_instance);
			lib_ctx.actived_device_instance = NULL;
			lib_ctx.is_delay_destory_actived_device = 0;
		}
		else
		{
			sr_info("Close the previous device, name:\"%s\"", 
					lib_ctx.actived_device_instance->name);
			close_device_instance(lib_ctx.actived_device_instance);
			old_dev = lib_ctx.actived_device_instance;
		}
	}

	// To open the new.
	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		if (dev->handle == handle)
		{
			bFind = 1;

			if (dev->dev_type == DEV_TYPE_USB && DS_RES_PATH[0] == '\0'){
				sr_err("Please call ds_set_firmware_resource_dir() to set the firmware resource path.");
			}

			if (dev->dev_type == DEV_TYPE_FILELOG)
				sr_info("Activating virtual device name: \"%s\".", dev->name);
			else
				sr_info("Activating device name: \"%s\".", dev->name);

			ret = open_device_instance(dev);

			if (ret == SR_OK)
			{
				lib_ctx.actived_device_instance = dev;
				dev->actived_times++;
			}
			else
			{  
				// Failed to switch new device.
				if(lib_ctx.device_list != NULL && old_dev == NULL){
					old_dev = lib_ctx.device_list->data;					
				}

				if (old_dev != NULL){
					if (old_dev->dev_type == DEV_TYPE_USB){
						sr_err("Open device error! Will switch to \"%s\", handle:%p.",
							old_dev->name,
							old_dev->handle);
					}
					else{
						sr_err("Open device error! Will switch to \"%s\".",
							old_dev->name);
					}
				
					lib_ctx.actived_device_instance = old_dev;
					ret = open_device_instance(old_dev);
				}	
			}
			break;
		}
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	sr_info("Activating device end.");

	if (!bFind)
	{
		sr_err("Active device error, can't find the device.");
		return SR_ERR_CALL_STATUS;
	}

	return ret;
}

/**
 * Active a device
 * if @index is -1, will select the last one.
 */
SR_API int ds_active_device_by_index(int index)
{
	GSList *l;
	struct sr_dev_inst *dev;
	ds_device_handle handle = NULL_HANDLE;
	ds_device_handle lst_handle = NULL_HANDLE;
	int i = 0;

	pthread_mutex_lock(&lib_ctx.mutext);

	// Get index
	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		lst_handle = dev->handle;

		if (index == i)
		{
			handle = dev->handle;
			break;
		}
		++i;
	}
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (index == -1)
	{
		handle = lst_handle; // Get the last one.
	}

	if (handle == NULL_HANDLE)
	{
		sr_err("ds_active_device_by_index(), index is error!");
		return SR_ERR_CALL_STATUS;
	}

	return ds_active_device(handle);
}

/**
 * Get the selected device index.
 */
SR_API int ds_get_actived_device_index()
{
	int dex = -1;
	GSList *l = NULL;
	int i = 0;
	
	(void)l;

	if (lib_ctx.actived_device_instance == NULL)
	{
		return -1;
	}

	pthread_mutex_lock(&lib_ctx.mutext);

	for (l = lib_ctx.device_list; l; l = l->next)
	{
		if ((struct sr_dev_inst *)l->data == lib_ctx.actived_device_instance)
		{
			dex = i;
			break;
		}
		i++;
	}
	pthread_mutex_unlock(&lib_ctx.mutext);

	return dex;
}

/**
 * Detect whether the active device exists
 */
SR_API int ds_have_actived_device()
{
	if (lib_ctx.actived_device_instance != NULL)
	{
		return 1;
	}
	return 0;
}

/**
 * Create a device from session file, and append to the list.
 */
SR_API int ds_device_from_file(const char *file_path)
{ 	
	struct sr_dev_inst *dev;
	int ret;

	if (file_path == NULL || *file_path == '\0'){
		sr_err("Error!File name is empty.");
		return SR_ERR_ARG;
	}

	dev = NULL;
	ret = sr_new_virtual_device(file_path, &dev);

	if (ret != SR_OK){
		return ret;
	}

	pthread_mutex_lock(&lib_ctx.mutext);
	lib_ctx.device_list = g_slist_append(lib_ctx.device_list, dev);
	pthread_mutex_unlock(&lib_ctx.mutext);

	return SR_OK;
}

/**
 * Get the decive supports work mode, mode list: LOGIC、ANALOG、DSO
 * return type see struct sr_dev_mode.
 */
SR_API const GSList *ds_get_actived_device_mode_list()
{
	struct sr_dev_inst *dev;

	dev = lib_ctx.actived_device_instance;

	if (dev == NULL)
	{
		sr_err("Have no actived device.");
	}
	if (dev->driver == NULL || dev->driver->dev_mode_list == NULL)
	{
		sr_err("Module not implemented.");
		return NULL;
	}

	return dev->driver->dev_mode_list(dev);
}

/**
 * Remove one device from the list, and destory it.
 * User need to call ds_get_device_list() to get the new list.
 */
SR_API int ds_remove_device(ds_device_handle handle)
{
	GSList *l;
	struct sr_dev_inst *dev;
	int bFind = 0;

	if (handle == NULL_HANDLE)
		return SR_ERR_ARG;

	if (lib_ctx.actived_device_instance != NULL 
		&& lib_ctx.actived_device_instance->handle == handle 
			&& ds_is_collecting())
	{
		sr_err("Device is collecting, can't remove it.");
		return SR_ERR_CALL_STATUS;
	}

	if (lib_ctx.actived_device_instance != NULL 
		&& lib_ctx.is_delay_destory_actived_device 
			&& lib_ctx.actived_device_instance->handle == handle)
	{
		sr_info("The current device is delayed for destruction, handle:%p", lib_ctx.actived_device_instance->handle);
		destroy_device_instance(lib_ctx.actived_device_instance);
		lib_ctx.actived_device_instance = NULL;
		lib_ctx.is_delay_destory_actived_device = 0;
	}

	pthread_mutex_lock(&lib_ctx.mutext);

	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		if (dev->handle == handle)
		{
			lib_ctx.device_list = g_slist_remove(lib_ctx.device_list, l->data);

			if (dev == lib_ctx.actived_device_instance)
			{
				lib_ctx.actived_device_instance = NULL;
			}

			destroy_device_instance(dev);
			bFind = 1;
			break;
		}
	}
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (bFind == 0)
		return SR_ERR_CALL_STATUS;

	return SR_OK;
}

/**
 * Get the actived device info.
 * If the actived device is not exists, the handle filed will be set null.
 */
SR_API int ds_get_actived_device_info(struct ds_device_full_info *fill_info)
{
	struct sr_dev_inst *dev;
	int ret;

	if (fill_info == NULL)
		return SR_ERR_ARG;

	ret = SR_ERR_CALL_STATUS;
	fill_device_full_info(NULL, fill_info);

	pthread_mutex_lock(&lib_ctx.mutext);

	dev = lib_ctx.actived_device_instance;
	if (dev != NULL)
	{
		fill_device_full_info(dev, fill_info);
		ret = SR_OK;
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	return ret;
}

SR_API int ds_get_device_info_by_handle(ds_device_handle handle,
										struct ds_device_full_info *fill_info)
{
	struct sr_dev_inst *dev;
	int ret;

	if (handle == NULL_HANDLE || fill_info == NULL)
		return SR_ERR_ARG;

	ret = SR_ERR_CALL_STATUS;
	fill_device_full_info(NULL, fill_info);

	pthread_mutex_lock(&lib_ctx.mutext);
	dev = find_device_instance_by_handle(handle);
	if (dev != NULL)
	{
		fill_device_full_info(dev, fill_info);
		ret = SR_OK;
	}
	pthread_mutex_unlock(&lib_ctx.mutext);

	return ret;
}

SR_API ds_device_handle ds_get_device_handle_from_inst(const struct sr_dev_inst *sdi)
{
	if (sdi == NULL)
		return NULL_HANDLE;

	return sdi->handle;
}

/**
 * Get actived device work model. mode list:LOGIC、ANALOG、DSO
 */
SR_API int ds_get_actived_device_mode()
{
	int mode = UNKNOWN_DSL_MODE;
	pthread_mutex_lock(&lib_ctx.mutext);

	if (lib_ctx.actived_device_instance != NULL)
	{
		mode = lib_ctx.actived_device_instance->mode;
	}
	pthread_mutex_unlock(&lib_ctx.mutext);

	return mode;
}

/**
 * Start collect data
 */
SR_API int ds_start_collect()
{
	int ret;
	struct sr_dev_inst *di;
	di = lib_ctx.actived_device_instance;

	lib_ctx.last_error = SR_OK;

	sr_info("Start collect.");

	if (ds_is_collecting())
	{
		sr_err("Error,it's collecting!");
		return SR_ERR_CALL_STATUS;
	}
	if (di == NULL)
	{
		sr_err("Please set a current device first.");
		return SR_ERR_CALL_STATUS;
	}
	if (di->status == SR_ST_INITIALIZING)
	{
		sr_err("Error!The device is initializing.");
		return SR_ERR_CALL_STATUS;
	}
	if (ds_channel_is_enabled() == 0)
	{
		sr_err("There have no useable channel, unable to collect.");
		return SR_ERR_CALL_STATUS;
	}
	if (lib_ctx.data_forward_callback == NULL)
	{
		sr_err("Error! Data forwarding callback is not set, see \"ds_set_datafeed_callback()\".");
		return SR_ERR_CALL_STATUS;
	}

	// Create new session.
	sr_session_new();

	if (di->status != SR_ST_ACTIVE)
	{
		ret = open_device_instance(di); // open device
		if (ret != SR_OK)
		{
			sr_err("Open device error!");
			return ret;
		}
	}


	lib_ctx.collect_thread = g_thread_new("collect_proc", collect_run_proc, (gpointer)0);

	return SR_OK;
}

SR_API int ds_start_collect_multi(const struct ds_collect_target *targets, int count)
{
	int i;
	int ret;
	struct sr_dev_inst *di;

	lib_ctx.last_error = SR_OK;

	sr_info("Start multi-device collect.");

	if (targets == NULL || count <= 0)
		return SR_ERR_ARG;

	if (ds_is_collecting())
	{
		sr_err("Error,it's collecting!");
		return SR_ERR_CALL_STATUS;
	}

	if (lib_ctx.data_forward_callback == NULL)
	{
		sr_err("Error! Data forwarding callback is not set, see \"ds_set_datafeed_callback()\".");
		return SR_ERR_CALL_STATUS;
	}

	clear_collect_devices();
	sr_session_new();

	for (i = 0; i < count; i++)
	{
		if (targets[i].handle == NULL_HANDLE)
		{
			// The hotplug thread holds off removing devices while this list is set.
			clear_collect_devices();
			return SR_ERR_ARG;
		}

		pthread_mutex_lock(&lib_ctx.mutext);
		di = find_device_instance_by_handle(targets[i].handle);
		pthread_mutex_unlock(&lib_ctx.mutext);

		if (di == NULL)
		{
			sr_err("Multi-device collect target not found.");
			clear_collect_devices();
			return SR_ERR_CALL_STATUS;
		}

		if (di->status == SR_ST_INITIALIZING)
		{
			sr_err("Error!The device is initializing.");
			clear_collect_devices();
			return SR_ERR_CALL_STATUS;
		}

		if (di->status != SR_ST_ACTIVE)
		{
			ret = open_device_instance(di);
			if (ret != SR_OK)
			{
				sr_err("Open multi-device target error!");
				clear_collect_devices();
				return ret;
			}
		}

		lib_ctx.collect_device_list = g_slist_append(lib_ctx.collect_device_list, di);
	}

	lib_ctx.collect_targets = g_try_malloc0(sizeof(struct ds_collect_target) * count);
	if (!lib_ctx.collect_targets)
	{
		clear_collect_devices();
		return SR_ERR_MALLOC;
	}

	memcpy(lib_ctx.collect_targets, targets, sizeof(struct ds_collect_target) * count);
	lib_ctx.collect_target_count = count;

	lib_ctx.collect_thread = g_thread_new("collect_multi_proc", collect_multi_run_proc, (gpointer)0);

	return SR_OK;
}

static gpointer collect_run_proc(gpointer data)
{
	(void)data;

	int ret;
	struct sr_dev_inst *di;
	int bError;
	
	bError = 0;
	di = lib_ctx.actived_device_instance;

	send_event(DS_EV_COLLECT_TASK_START);

	sr_info("Collect thread start.");

	if (di == NULL || di->driver == NULL || di->driver->dev_acquisition_start == NULL)
	{
		sr_err("The device cannot be used.");
		bError = 1;
		goto END;
	}

	di->capture_flags = 0;
	ret = di->driver->dev_acquisition_start(di, (void *)di);
	if (ret != SR_OK)
	{
		sr_err("Failed to start acquisition of device in "
			   "running session: %d",
			   ret);
		bError = 1;
		goto END;
	}

	send_event(DS_EV_DEVICE_RUNNING);

	ret = sr_session_run();

	send_event(DS_EV_DEVICE_STOPPED);

	if (ret != SR_OK)
	{
		sr_err("Run session error!");
		bError = 1;
		goto END;
	}

END:
	sr_info("Collect thread end.");
	lib_ctx.collect_thread = NULL;

	if (bError)
		send_event(DS_EV_COLLECT_TASK_END_BY_ERROR);
	else if (lib_ctx.is_stop_by_detached)
        post_event_async(DS_EV_COLLECT_TASK_END_BY_DETACHED);
	else
		send_event(DS_EV_COLLECT_TASK_END); // Normal end.

	lib_ctx.is_stop_by_detached = 0;

	return NULL;
}

static gpointer collect_multi_run_proc(gpointer data)
{
	(void)data;

	int i;
	int ret;
	struct sr_dev_inst *di;
	struct ds_trigger master_trigger;
	int have_master_trigger;
	int bError;

	bError = 0;
	have_master_trigger = 0;

	send_event(DS_EV_COLLECT_TASK_START);
	sr_info("Multi-device collect thread start.");

	ret = ds_trigger_copy(&master_trigger);
	if (ret != SR_OK)
	{
		sr_err("Failed to snapshot master trigger settings.");
		bError = 1;
		goto END;
	}
	have_master_trigger = 1;

	for (i = 0; i < lib_ctx.collect_target_count; i++)
	{
		di = find_device_instance_by_handle(lib_ctx.collect_targets[i].handle);
		if (di == NULL || di->driver == NULL || di->driver->dev_acquisition_start == NULL)
		{
			sr_err("The multi-device collect target cannot be used.");
			bError = 1;
			goto END;
		}

		ret = start_collect_target(di, &lib_ctx.collect_targets[i], &master_trigger);
		if (ret != SR_OK)
		{
			sr_err("Failed to start acquisition of multi-device target: %d", ret);
			bError = 1;
			goto END;
		}
	}

	ds_trigger_apply(&master_trigger);

	send_event(DS_EV_DEVICE_RUNNING);

	ret = sr_session_run();

	send_event(DS_EV_DEVICE_STOPPED);

	if (ret != SR_OK)
	{
		sr_err("Run multi-device session error!");
		bError = 1;
		goto END;
	}

END:
	if (have_master_trigger)
		ds_trigger_apply(&master_trigger);

	if (bError)
		stop_collect_device_list();

	sr_info("Multi-device collect thread end.");
	lib_ctx.collect_thread = NULL;
	clear_collect_devices();

	if (bError)
		send_event(DS_EV_COLLECT_TASK_END_BY_ERROR);
	else if (lib_ctx.is_stop_by_detached)
		post_event_async(DS_EV_COLLECT_TASK_END_BY_DETACHED);
	else
		send_event(DS_EV_COLLECT_TASK_END);

	lib_ctx.is_stop_by_detached = 0;

	return NULL;
}

/**
 * Stop collect data, but not close the device.
 */
static int stop_collect_common(int abort_targets_first)
{
	GThread *collect_thread;

	sr_info("Stop collect.");

	if (!ds_is_collecting())
	{
		sr_err("It's not collecting now.");
		return SR_ERR_CALL_STATUS;
	}

	collect_thread = lib_ctx.collect_thread;

	if (abort_targets_first)
		current_device_acquisition_stop();

	// Stop current session.
	sr_session_stop();

	// Wait the collect thread ends.
	if (collect_thread != NULL)
		g_thread_join(collect_thread);
	lib_ctx.collect_thread = NULL;

	return SR_OK;
}

SR_API int ds_stop_collect()
{
	return stop_collect_common(FALSE);
}

SR_API int ds_stop_collect_multi()
{
	return stop_collect_common(TRUE);
}

/**
 * Check if the device is collecting.
 */
SR_API int ds_is_collecting()
{
	if (lib_ctx.collect_thread != NULL)
	{
		return 1;
	}
	return 0;
}

SR_API int ds_release_actived_device()
{
	if (lib_ctx.actived_device_instance == NULL){
		return SR_ERR_CALL_STATUS;
	}

	if (ds_is_collecting()){
		ds_stop_collect();
	}

	if (lib_ctx.actived_device_instance->dev_type == DEV_TYPE_USB)
	{
		sr_info("Release current actived device. name:\"%s\", handle:%p", 
			lib_ctx.actived_device_instance->name,
			lib_ctx.actived_device_instance->handle);
	}
	else
	{
		sr_info("Release current actived device. name:\"%s\"", 
			lib_ctx.actived_device_instance->name);
	}	

	close_device_instance(lib_ctx.actived_device_instance);

	// Destroy current session.
	sr_session_destroy();

	return SR_OK;
}

int ds_trigger_is_enabled()
{
	GSList *l;
	struct sr_channel *p;
	int ret;

	ret = 0;
	pthread_mutex_lock(&lib_ctx.mutext);

	if (lib_ctx.actived_device_instance != NULL)
	{
		for (l = lib_ctx.actived_device_instance->channels; l; l = l->next)
		{
			p = (struct sr_channel *)l->data;
			if (p->trigger && p->trigger[0] != '\0')
			{
				ret = 1;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lib_ctx.mutext);

	return ret;
}

static struct sr_dev_inst *find_device_instance_by_handle(ds_device_handle handle)
{
	GSList *l;
	struct sr_dev_inst *dev;

	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		if (dev != NULL && dev->handle == handle)
			return dev;
	}

	return NULL;
}

static void fill_device_full_info(struct sr_dev_inst *dev, struct ds_device_full_info *fill_info)
{
	if (!fill_info)
		return;

	fill_info->handle = NULL_HANDLE;
	fill_info->name[0] = '\0';
	fill_info->path[0] = '\0';
	fill_info->driver_name[0] = '\0';
	fill_info->dev_type = DEV_TYPE_UNKOWN;
	fill_info->di = NULL;
	fill_info->actived_times = 0;

	if (!dev)
		return;

	fill_info->handle = dev->handle;
	fill_info->dev_type = dev->dev_type;
	fill_info->di = dev;
	fill_info->actived_times = dev->actived_times;
	strncpy(fill_info->name, (const char *)dev->name, sizeof(fill_info->name) - 1);

	if (dev->driver && dev->driver->name)
		strncpy(fill_info->driver_name, dev->driver->name, sizeof(fill_info->driver_name) - 1);

	if ((dev->dev_type == DEV_TYPE_FILELOG || dev->dev_type == DEV_TYPE_DEMO) && dev->path != NULL)
		strncpy(fill_info->path, dev->path, sizeof(fill_info->path) - 1);
}

SR_API int ds_trigger_reset()
{
	return ds_trigger_init();
}

/**-------------------public end ---------------*/

/**-------------------config -------------------*/
SR_API int ds_get_actived_device_config(const struct sr_channel *ch,
										const struct sr_channel_group *cg,
										int key, GVariant **data)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		sr_err("Have no actived device.");
		return SR_ERR_CALL_STATUS;
	}

	return sr_config_get(lib_ctx.actived_device_instance->driver,
						 lib_ctx.actived_device_instance,
						 ch,
						 cg,
						 key,
						 data);
}

SR_API int ds_get_device_config_by_handle(ds_device_handle handle,
										  const struct sr_channel *ch,
										  const struct sr_channel_group *cg,
										  int key, GVariant **data)
{
	struct sr_dev_inst *di;

	if (handle == NULL_HANDLE)
		return SR_ERR_ARG;

	pthread_mutex_lock(&lib_ctx.mutext);
	di = find_device_instance_by_handle(handle);
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (di == NULL)
	{
		sr_err("Device handle not found.");
		return SR_ERR_CALL_STATUS;
	}

	return sr_config_get(di->driver, di, ch, cg, key, data);
}

SR_API int ds_set_actived_device_config(const struct sr_channel *ch,
										const struct sr_channel_group *cg,
										int key, GVariant *data)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		sr_err("Have no actived device.");
		return SR_ERR_CALL_STATUS;
	}

	return sr_config_set(
		lib_ctx.actived_device_instance,
		(struct sr_channel*)ch,
		(struct sr_channel_group*)cg,
		key,
			data);
}

SR_API int ds_set_device_config_by_handle(ds_device_handle handle,
										  const struct sr_channel *ch,
										  const struct sr_channel_group *cg,
										  int key, GVariant *data)
{
	struct sr_dev_inst *di;

	if (handle == NULL_HANDLE)
		return SR_ERR_ARG;

	pthread_mutex_lock(&lib_ctx.mutext);
	di = find_device_instance_by_handle(handle);
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (di == NULL)
	{
		sr_err("Device handle not found.");
		return SR_ERR_CALL_STATUS;
	}

	if (ensure_device_instance_open(di) != SR_OK)
		return SR_ERR_CALL_STATUS;

	return sr_config_set(di, (struct sr_channel *)ch, (struct sr_channel_group *)cg, key, data);
}

SR_API int ds_get_actived_device_config_list(const struct sr_channel_group *cg,
											 int key, GVariant **data)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		sr_err("Have no actived device.");
		return SR_ERR_CALL_STATUS;
	}

	return sr_config_list(lib_ctx.actived_device_instance->driver,
						  lib_ctx.actived_device_instance,
						  cg,
						  key,
						  data);
}

SR_API const struct sr_config_info *ds_get_actived_device_config_info(int key)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		sr_err("Have no actived device.");
		return NULL;
	}

	return sr_config_info_get(key);
}

SR_API int ds_get_actived_device_status(struct sr_status *status, gboolean prg)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		sr_err("Have no actived device.");
		return SR_ERR_CALL_STATUS;
	}

	return sr_status_get(lib_ctx.actived_device_instance, status, prg);
}

SR_API int ds_get_device_status_by_handle(ds_device_handle handle,
										  struct sr_status *status, gboolean prg)
{
	struct sr_dev_inst *di;

	if (handle == NULL_HANDLE)
		return SR_ERR_ARG;

	pthread_mutex_lock(&lib_ctx.mutext);
	di = find_device_instance_by_handle(handle);
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (di == NULL)
	{
		sr_err("Device handle not found.");
		return SR_ERR_CALL_STATUS;
	}

	return sr_status_get(di, status, prg);
}

SR_API struct sr_config *ds_new_config(int key, GVariant *data)
{
	return sr_config_new(key, data);
}

SR_API void ds_free_config(struct sr_config *src)
{
	sr_config_free(src);
}

SR_API int ds_dsl_option_value_to_code(int work_mode, int config_id, const char *value)
{ 
	if (work_mode == LOGIC)
		return sr_dslogic_option_value_to_code(lib_ctx.actived_device_instance, config_id, value);
	else
		return sr_dscope_option_value_to_code(lib_ctx.actived_device_instance, config_id, value);
}

SR_API int ds_get_actived_device_init_status(int *status)
{
	if (lib_ctx.actived_device_instance != NULL && status != NULL)
	{
		*status = lib_ctx.actived_device_instance->status;
		return SR_OK;
	}

	return SR_ERR;
}

/**-----------channel -------------*/
SR_API int ds_enable_device_channel(const struct sr_channel *ch, gboolean enable)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		return SR_ERR_CALL_STATUS;
	}
	return sr_enable_device_channel(lib_ctx.actived_device_instance, ch, enable);
}

SR_API int ds_enable_device_channel_index(int ch_index, gboolean enable)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		return SR_ERR_CALL_STATUS;
	}
	return sr_dev_probe_enable(lib_ctx.actived_device_instance, ch_index, enable);
}

SR_API int ds_enable_device_channel_index_by_handle(ds_device_handle handle, int ch_index, gboolean enable)
{
	struct sr_dev_inst *di;

	if (handle == NULL_HANDLE)
		return SR_ERR_ARG;

	pthread_mutex_lock(&lib_ctx.mutext);
	di = find_device_instance_by_handle(handle);
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (di == NULL)
		return SR_ERR_CALL_STATUS;

	if (ensure_device_instance_open(di) != SR_OK)
		return SR_ERR_CALL_STATUS;

	return sr_dev_probe_enable(di, ch_index, enable);
}

SR_API int ds_set_device_channel_name(int ch_index, const char *name)
{
	if (lib_ctx.actived_device_instance == NULL)
	{
		return SR_ERR_CALL_STATUS;
	}
	return sr_dev_probe_name_set(lib_ctx.actived_device_instance, ch_index, name);
}

/**
 *  check that at least one probe is enabled
 */
int ds_channel_is_enabled()
{
	GSList *l;
	struct sr_channel *p;
	int ret;

	ret = 0;
	pthread_mutex_lock(&lib_ctx.mutext);

	if (lib_ctx.actived_device_instance != NULL)
	{
		for (l = lib_ctx.actived_device_instance->channels; l; l = l->next)
		{
			p = (struct sr_channel *)l->data;
			if (p->enabled)
			{
				ret = 1;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lib_ctx.mutext);
	return ret;
}

GSList *ds_get_actived_device_channels()
{
	if (lib_ctx.actived_device_instance != NULL)
	{
		return lib_ctx.actived_device_instance->channels;
	}
	return NULL;
}

GSList *ds_get_device_channels_by_handle(ds_device_handle handle)
{
	struct sr_dev_inst *di;

	if (handle == NULL_HANDLE)
		return NULL;

	pthread_mutex_lock(&lib_ctx.mutext);
	di = find_device_instance_by_handle(handle);
	pthread_mutex_unlock(&lib_ctx.mutext);

	if (di != NULL)
		return di->channels;

	return NULL;
}

/**-------------------config end-------------------*/

/**-------------------internal function ---------------*/
/**
 * Check whether the USB device is in the device list.
 */
SR_PRIV int sr_usb_device_is_exists(libusb_device *usb_dev)
{
	GSList *l;
	struct sr_dev_inst *dev;
	int bFind = 0;

	if (lib_ctx.is_reloading_list){
		return 0;
	}

	if (usb_dev == NULL)
	{
		sr_err("sr_usb_device_is_exists(), @usb_dev is null.");
		return 0;
	}

	pthread_mutex_lock(&lib_ctx.mutext);

	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		if (dev->handle == (ds_device_handle)usb_dev)
		{
			bFind = 1;
			break;
		}
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	return bFind;
}

/**
 * Forward the data.
 */
SR_PRIV int ds_data_forward(const struct sr_dev_inst *sdi,
							const struct sr_datafeed_packet *packet)
{
	if (!sdi)
	{
		sr_err("%s: sdi was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!packet)
	{
		sr_err("%s: packet was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (lib_ctx.data_forward_callback != NULL){
		lib_ctx.data_forward_callback(sdi, packet);
		return SR_OK;
	}
	return SR_ERR;
}

SR_PRIV int current_device_acquisition_stop()
{
	struct sr_dev_inst *di;
	if (lib_ctx.collect_device_list != NULL)
	{
		return stop_collect_device_list();
	}

	di = lib_ctx.actived_device_instance;
	if (di != NULL && di->driver && di->driver->dev_acquisition_stop)
	{
		return di->driver->dev_acquisition_stop(di, (void *)di);
	}
	return SR_ERR;
}

static int start_collect_target(struct sr_dev_inst *di, const struct ds_collect_target *target,
								const struct ds_trigger *master_trigger)
{
	int ret;

	if (di == NULL || target == NULL || di->driver == NULL ||
		di->driver->dev_acquisition_start == NULL)
		return SR_ERR_ARG;

	di->capture_flags = 0;

	switch (target->role)
	{
	case DS_COLLECT_TARGET_SECONDARY_SYNC_RISING:
		if (target->sync_channel >= MaxTriggerProbes)
			return SR_ERR_ARG;
		if (target->trigger_pos_percent > 100)
			return SR_ERR_ARG;

		di->capture_flags |= SR_DEV_CAPTURE_KEEP_FULL_LOGIC_UPLOAD;
		ds_trigger_init();
		ds_trigger_set_mode(SIMPLE_TRIGGER);
		ds_trigger_set_pos(target->trigger_pos_percent != 0 || master_trigger == NULL ?
		                   target->trigger_pos_percent : master_trigger->trigger_pos);
		ds_trigger_set_stage(0);
		ds_trigger_set_en(1);
		ds_trigger_probe_set(target->sync_channel, 'R', 'X');
		break;

	case DS_COLLECT_TARGET_INSTANT:
		ds_trigger_init();
		ds_trigger_set_en(0);
		break;

	case DS_COLLECT_TARGET_MASTER_CURRENT_TRIGGER:
	default:
		if (master_trigger)
			ds_trigger_apply(master_trigger);
		break;
	}

	ret = di->driver->dev_acquisition_start(di, (void *)di);

	if (master_trigger)
		ds_trigger_apply(master_trigger);

	return ret;
}

static int stop_collect_device_list(void)
{
	GSList *l;
	struct sr_dev_inst *di;
	int ret;
	int last_ret;

	last_ret = SR_ERR;

	for (l = lib_ctx.collect_device_list; l; l = l->next)
	{
		di = l->data;
		if (di != NULL && di->driver && di->driver->dev_acquisition_stop)
		{
			ret = di->driver->dev_acquisition_stop(di, (void *)di);
			if (ret != SR_OK)
				last_ret = ret;
			else if (last_ret == SR_ERR)
				last_ret = SR_OK;
		}
	}

	return last_ret;
}

static void clear_collect_devices(void)
{
	GSList *l;
	struct sr_dev_inst *di;

	for (l = lib_ctx.collect_device_list; l; l = l->next)
	{
		di = l->data;
		if (di != NULL)
			di->capture_flags = 0;
	}

	if (lib_ctx.collect_device_list != NULL)
	{
		g_slist_free(lib_ctx.collect_device_list);
		lib_ctx.collect_device_list = NULL;
	}

	if (lib_ctx.collect_targets != NULL)
	{
		g_free(lib_ctx.collect_targets);
		lib_ctx.collect_targets = NULL;
	}

	lib_ctx.collect_target_count = 0;
}

/**--------------------internal function end-----------*/

/**-------------------private function ---------------*/

/**
 * hotplug event
 *
 * Called on whichever thread is handling libusb events: the hotplug thread,
 * the collect thread, or any thread inside a synchronous transfer, possibly
 * with lib_ctx.mutext held. So it only records that something changed; the
 * hotplug thread compares the device list with the USB devices present.
 */
static void hotplug_event_listen_callback(struct libusb_context *ctx, struct libusb_device *dev, int event)
{
	(void)ctx;

	if (dev == NULL){
		if (event == USB_EV_HOTPLUG_ATTACH)
			dev = get_new_attached_usb_device();
		else
			dev = get_new_detached_usb_device();
	}	

	if (dev == NULL){
		sr_err("hotplug_event_listen_callback(), the device handle is null.");
		return;
	}

	if (event == USB_EV_HOTPLUG_ATTACH)
	{
		sr_info("----------One device attached,handle:%p", dev);
	}
	else if (event == USB_EV_HOTPLUG_DETTACH)
	{
		sr_info("----------One device detached,handle:%p", dev);

		if (lib_ctx.actived_device_instance != NULL 
			&& lib_ctx.actived_device_instance->handle == (ds_device_handle)dev 
			&& ds_is_collecting())
		{
			sr_info("The collecting device is detached, will stop the collect thread.");
			lib_ctx.is_stop_by_detached = 1;
			ds_release_actived_device();
		}
	}
	else
	{
		sr_err("Unknown usb device event");
		return;
	}

	pthread_mutex_lock(&lib_ctx.hotplug_mutex);
	if (event == USB_EV_HOTPLUG_ATTACH)
		lib_ctx.hotplug_attach_pending = 1;
	else
		lib_ctx.hotplug_detach_pending = 1;
	lib_ctx.hotplug_quiet_ticks = 0;
	pthread_mutex_unlock(&lib_ctx.hotplug_mutex);
}

static void process_attach_event(int isEvent)
{
	struct sr_dev_driver **drivers;
	GSList *dev_list;
	GSList *l;
	GSList *l_check;
	struct sr_dev_driver *dr;
	struct sr_dev_inst *sdi;
	struct sr_dev_inst *new_sdi;
	int num = 0;
	int bFind;

	if (isEvent){
		sr_info("Process device attach event.");
	}

	drivers = sr_driver_list();

	while (*drivers)
	{
		dr = *drivers;

		if (dr->driver_type == DRIVER_TYPE_HARDWARE)
		{
			dev_list = dr->scan(NULL);

			if (dev_list != NULL){
				pthread_mutex_lock(&lib_ctx.mutext);
				  
				for (l = dev_list; l; l = l->next)
				{
					bFind = 0;
					new_sdi = l->data;

					for (l_check = lib_ctx.device_list; l_check; l_check = l_check->next)
					{
						sdi = l_check->data;
						if (sdi->handle == new_sdi->handle){
							bFind = 1;
							break;
						}
					}

					if (!bFind){
						lib_ctx.device_list = g_slist_append(lib_ctx.device_list, l->data);
						num++;
					}
					else{
						sr_dev_inst_free(new_sdi); //Not append to list, so free it.						
					}					
				}
				pthread_mutex_unlock(&lib_ctx.mutext);
				g_slist_free(dev_list);
			}
		}

		drivers++;
	}

	// Tell user one new device attched, and the list is updated.
	if (num > 0 && isEvent){
		post_event_async(DS_EV_NEW_DEVICE_ATTACH);
	}
}

/**
 * Remove every USB device that libusb no longer reports from the list.
 * Presence is checked against the current libusb device list, not against
 * the device a detach event named, so a removal can't be lost to events
 * that arrived together, were coalesced or carried no device.
 */
static void remove_detached_devices(void)
{
	libusb_device **usb_list;
	ssize_t usb_count;
	ssize_t i;
	GSList *l;
	GSList *next;
	struct sr_dev_inst *dev;
	int present;
	int removed;
	int ev;

	usb_list = NULL;
	usb_count = libusb_get_device_list(lib_ctx.sr_ctx->libusb_ctx, &usb_list);

	if (usb_count < 0 || usb_list == NULL){
		// Without a list, every device would look detached.
		sr_err("%s: Failed to get the USB device list: %s",
			   __func__, libusb_error_name((int)usb_count));
		return;
	}

	removed = 0;
	ev = DS_EV_INACTIVE_DEVICE_DETACH;

	pthread_mutex_lock(&lib_ctx.mutext);

	for (l = lib_ctx.device_list; l; l = next)
	{
		next = l->next;
		dev = l->data;

		if (dev->dev_type != DEV_TYPE_USB)
			continue;

		present = 0;
		for (i = 0; i < usb_count; i++){
			if ((ds_device_handle)usb_list[i] == dev->handle){
				present = 1;
				break;
			}
		}

		if (present)
			continue;

		lib_ctx.device_list = g_slist_delete_link(lib_ctx.device_list, l);
		removed++;

		if (dev == lib_ctx.actived_device_instance)
		{
			sr_info("The current device is detached, its destruction is delayed, name:\"%s\", handle:%p",
					dev->name, dev->handle);
			lib_ctx.is_delay_destory_actived_device = 1;
			ev = DS_EV_CURRENT_DEVICE_DETACH;
		}
		else
		{
			sr_info("Remove the detached device, name:\"%s\", handle:%p", dev->name, dev->handle);
			destroy_device_instance(dev);
		}
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	libusb_free_device_list(usb_list, 1);

	// Tell user the list is updated.
	if (removed > 0){
		post_event_async(ev);
	}
}

static gpointer usb_hotplug_process_proc(gpointer data)
{
	int attach;
	int detach;

	(void)data;
	
	sr_info("Hotplug thread start!");

	while (!lib_ctx.lib_exit_flag)
	{
		sr_hotplug_wait_timout(lib_ctx.sr_ctx);

		xsleep(100);

		/* Wait for a burst of events (one unplugged hub, a firmware upload's
		 * re-enumeration) to settle, then take all of them at once. Events
		 * that arrive while this pass runs are kept for the next one. */
		attach = 0;
		detach = 0;

		pthread_mutex_lock(&lib_ctx.hotplug_mutex);
		if (lib_ctx.hotplug_attach_pending || lib_ctx.hotplug_detach_pending)
		{
			lib_ctx.hotplug_quiet_ticks++;

			if (lib_ctx.hotplug_quiet_ticks >= HOTPLUG_SETTLE_TICKS)
			{
				attach = lib_ctx.hotplug_attach_pending;
				detach = lib_ctx.hotplug_detach_pending;
				lib_ctx.hotplug_attach_pending = 0;
				lib_ctx.hotplug_detach_pending = 0;
				lib_ctx.hotplug_quiet_ticks = 0;
			}
		}
		pthread_mutex_unlock(&lib_ctx.hotplug_mutex);

		if (attach || detach)
		{
			/* Don't destroy devices the collect thread may be using. Try again
			 * after the capture; collect_device_list is cleared last. */
			if (ds_is_collecting() || lib_ctx.collect_device_list != NULL)
			{
				if (detach){
					pthread_mutex_lock(&lib_ctx.hotplug_mutex);
					lib_ctx.hotplug_detach_pending = 1;
					pthread_mutex_unlock(&lib_ctx.hotplug_mutex);
				}
			}
			else
			{
				// Before the scan, so a device that came back is listed once.
				remove_detached_devices();
			}
		}

		if (attach){
			process_attach_event(1);
		}
	}

	if (lib_ctx.callback_thread_count > 0)
	{
		sr_info("%d callback thread is actived, waiting all ends...", lib_ctx.callback_thread_count);
	}

	// Wait all callback thread end.
	while (lib_ctx.callback_thread_count > 0)
	{
		xsleep(100);
	}

	sr_info("Hotplug thread end!");

	return NULL;
}

static void make_demo_device_to_list()
{
	struct sr_dev_driver **drivers;
	struct sr_dev_driver *dr;
	GSList *dev_list;
	GSList *l;

	drivers = sr_driver_list();

	while (*drivers)
	{
		dr = *drivers;

		if (dr->driver_type == DRIVER_TYPE_DEMO)
		{
			dev_list = dr->scan(NULL);

			if (dev_list != NULL)
			{

				for (l = dev_list; l; l = l->next)
				{
					lib_ctx.device_list = g_slist_append(lib_ctx.device_list, l->data);
				}
				g_slist_free(dev_list);
			}
		}
		drivers++;
	}
}

static void destroy_device_instance(struct sr_dev_inst *dev)
{
	if (dev == NULL || dev->driver == NULL)
	{
		sr_err("destroy_device_instance() argument error.");
		return;
	}
	struct sr_dev_driver *driver_ins;
	driver_ins = dev->driver;

	if (driver_ins->dev_destroy)
		driver_ins->dev_destroy(dev);
	else if (driver_ins->dev_close)
		driver_ins->dev_close(dev);
}

static void close_device_instance(struct sr_dev_inst *dev)
{
	if (dev == NULL || dev->driver == NULL)
	{
		sr_err("close_device_instance() argument error.");
		return;
	}
	struct sr_dev_driver *driver_ins;
	driver_ins = dev->driver;

	if (driver_ins->dev_close)
		driver_ins->dev_close(dev);
}

static int open_device_instance(struct sr_dev_inst *dev)
{
	if (dev == NULL || dev->driver == NULL)
	{
		sr_err("open_device_instance() argument error.");
		return SR_ERR_ARG;
	}
	struct sr_dev_driver *driver_ins;
	driver_ins = dev->driver;

	if (driver_ins->dev_open)
	{
		sr_info("To open device, name:\"%s\"", dev->name);
		return driver_ins->dev_open(dev);
	}

	return SR_ERR_CALL_STATUS;
}

static int ensure_device_instance_open(struct sr_dev_inst *dev)
{
	if (dev == NULL)
		return SR_ERR_ARG;

	if (dev->status == SR_ST_INITIALIZING)
	{
		sr_err("Error!The device is initializing.");
		return SR_ERR_CALL_STATUS;
	}

	if (dev->status != SR_ST_ACTIVE)
		return open_device_instance(dev);

	return SR_OK;
}

static gpointer post_event_proc(gpointer event)
{
	if (lib_ctx.event_callback != NULL)
	{
		lib_ctx.event_callback((int)((unsigned long)event));
	}

	pthread_mutex_lock(&lib_ctx.mutext);
	lib_ctx.callback_thread_count--;
	pthread_mutex_unlock(&lib_ctx.mutext);

	return NULL;
}

static void post_event_async(int event)
{
	pthread_mutex_lock(&lib_ctx.mutext);
	lib_ctx.callback_thread_count++;
	pthread_mutex_unlock(&lib_ctx.mutext);

	g_thread_new("callback_thread", post_event_proc, (gpointer)((unsigned long)event));
}

static void send_event(int event)
{
	if (lib_ctx.event_callback != NULL)
	{
		lib_ctx.event_callback(event);
	}
}

static struct libusb_device* get_new_attached_usb_device()
{
	struct libusb_device* array[MAX_DEVCIE_LIST_LENGTH];
	int num;
	int i;
	GSList *l;
	int bFind;
	struct libusb_device* dev;
	struct sr_dev_inst *dev_ins;

	dev = NULL;
	num = 0;
	ds_scan_all_device_list(lib_ctx.sr_ctx->libusb_ctx, (struct libusb_device **)array, MAX_DEVCIE_LIST_LENGTH, &num);

	pthread_mutex_lock(&lib_ctx.mutext);

	for (i = 0; i < num; i++){
		bFind = 0;

		for (l = lib_ctx.device_list; l; l = l->next){
			dev_ins = l->data;
			if (dev_ins->handle == (ds_device_handle)array[i]){
				bFind = 1;
				break;
			}
		}

		if (bFind == 0){
			dev = array[i];
			break;
		}
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	if (dev == NULL){
		sr_info("Not found attached device.");
	}

	return dev;
}

static struct libusb_device* get_new_detached_usb_device()
{
    struct libusb_device* array[MAX_DEVCIE_LIST_LENGTH];
	int num;
	int i;
	GSList *l;
	int bFind;
	struct libusb_device* dev;
	struct sr_dev_inst *dev_ins;

	dev = NULL;
	num = 0;
	ds_scan_all_device_list(lib_ctx.sr_ctx->libusb_ctx, (struct libusb_device **)array, MAX_DEVCIE_LIST_LENGTH, &num);

	pthread_mutex_lock(&lib_ctx.mutext);

	for (l = lib_ctx.device_list; l; l = l->next)
	{	
		bFind = 0;
		dev_ins = l->data;

		if (dev_ins->dev_type != DEV_TYPE_USB){
			continue;
		}

		for (i = 0; i < num; i++){
			if (dev_ins->handle == (ds_device_handle)array[i]){
				bFind = 1;
				break;
			}			
		}

		if (bFind == 0){
			dev = (libusb_device*)dev_ins->handle;
			break;
		}
	}		

	pthread_mutex_unlock(&lib_ctx.mutext);

	if (dev == NULL){
		sr_info("Not found detached device.");
	}

	return dev;
}

SR_PRIV int post_message_callback(int msg)
{
	send_event(msg);
	return SR_OK;
}

SR_PRIV void xsleep(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

/**-------------------private function end---------------*/

SR_PRIV void ds_set_last_error(int error)
{
	lib_ctx.last_error = error;
}

SR_API int ds_get_last_error()
{
	return lib_ctx.last_error;
}

SR_API int ds_reload_device_list()
{
	sr_info("Reload device list.");

	lib_ctx.is_reloading_list = 1;
	lib_ctx.is_delay_destory_actived_device = 0;

	/* Windows reports the devices as removed when the machine sleeps. Don't
	 * drop them from the list on waking up while they are still connected. */
	pthread_mutex_lock(&lib_ctx.hotplug_mutex);
	lib_ctx.hotplug_detach_pending = 0;
	pthread_mutex_unlock(&lib_ctx.hotplug_mutex);

	process_attach_event(0);
	lib_ctx.is_reloading_list = 0;

	return SR_OK;
}

SR_API int ds_close_all_device()
{
	sr_info("Close all device.");

	GSList *l;
	struct sr_dev_inst *dev;

	pthread_mutex_lock(&lib_ctx.mutext);

	for (l = lib_ctx.device_list; l; l = l->next)
	{
		dev = l->data;
		if (dev->dev_type == DEV_TYPE_USB){
			close_device_instance(dev);
		}	
	}

	pthread_mutex_unlock(&lib_ctx.mutext);

	return SR_OK;
}
