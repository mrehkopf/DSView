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

#include "deviceagent.h"
#include <assert.h>
#include <string.h>
#include "log.h"

namespace {

struct StackingSecondaryTiming
{
    uint64_t samplelimit;
    uint16_t trigger_percent;
};

uint64_t samples_at_percent(uint64_t sample_count, uint16_t percent)
{
    if (percent == 0 || sample_count == 0)
        return 0;

    return (uint64_t)((long double)sample_count * (long double)percent / 100.0L);
}

uint16_t ceil_percent_for_samples(uint64_t desired_samples, uint64_t sample_count)
{
    if (desired_samples == 0 || sample_count == 0)
        return 0;

    long double scaled = (long double)desired_samples * 100.0L / (long double)sample_count;
    uint64_t percent = (uint64_t)scaled;
    if ((long double)percent < scaled)
        percent++;

    if (percent > 100)
        percent = 100;

    return (uint16_t)percent;
}

StackingSecondaryTiming stacking_secondary_timing(uint64_t master_samplelimit,
                                                  uint16_t master_trigger_percent,
                                                  uint64_t hw_depth)
{
    StackingSecondaryTiming timing;
    timing.samplelimit = master_samplelimit;
    timing.trigger_percent = master_trigger_percent;

    if (master_samplelimit == 0 || master_trigger_percent == 0 ||
        hw_depth <= master_samplelimit)
        return timing;

    const uint64_t desired_trigger_samples =
        samples_at_percent(master_samplelimit, master_trigger_percent);
    if (desired_trigger_samples == 0)
        return timing;

    const uint64_t max_extra = hw_depth - master_samplelimit;
    uint64_t extra_samples = desired_trigger_samples;
    if (extra_samples > max_extra)
        extra_samples = max_extra;

    for (int i = 0; i < 8; i++){
        timing.samplelimit = master_samplelimit + extra_samples;
        timing.trigger_percent =
            ceil_percent_for_samples(desired_trigger_samples, timing.samplelimit);

        const uint64_t secondary_trigger_samples =
            samples_at_percent(timing.samplelimit, timing.trigger_percent);
        if (secondary_trigger_samples <= extra_samples || extra_samples == max_extra)
            return timing;

        extra_samples = secondary_trigger_samples;
        if (extra_samples > max_extra)
            extra_samples = max_extra;
    }

    timing.samplelimit = master_samplelimit + extra_samples;
    timing.trigger_percent =
        ceil_percent_for_samples(desired_trigger_samples, timing.samplelimit);
    return timing;
}

}


DeviceAgent::DeviceAgent()
{
    _dev_handle = NULL_HANDLE;
    _di = NULL;
    _dev_type = 0;
    _callback = NULL;
    _is_new_device = false;
    _logic_stacking_channels = NULL;
}

DeviceAgent::~DeviceAgent()
{
    clear_stacking_channels();
}

void DeviceAgent::update()
{
    _dev_handle = NULL_HANDLE;
    _dev_name = "";
    _path = "";
    _di = NULL;
    _dev_type = 0;
    _is_new_device = false;

    struct ds_device_full_info info;

    if (ds_get_actived_device_info(&info) == SR_OK)
    {
        _dev_handle = info.handle;
        _dev_type = info.dev_type;
        _di = info.di;
        _is_new_device = info.actived_times == 1;

        _dev_name = QString::fromLocal8Bit(info.name);
        _driver_name = QString::fromLocal8Bit(info.driver_name);

        if (info.path[0] != '\0'){
            _path = QString::fromLocal8Bit(info.path);
        } 
    }
}

 sr_dev_inst* DeviceAgent::inst()
 {
    assert(_dev_handle);
    return _di;
 }

bool DeviceAgent::enable_probe(const sr_channel *probe, bool enable)
{
    assert(_dev_handle);

    if (ds_enable_device_channel(probe, enable) == SR_OK){
        config_changed();
        return true;
    }
    return false;
}

bool DeviceAgent::enable_probe(int probe_index, bool enable)
{
     assert(_dev_handle);

     if (ds_enable_device_channel_index(probe_index, enable) == SR_OK){
        config_changed();
        return true;
    }
    return false;
}

bool DeviceAgent::set_channel_name(int ch_index, const char *name)
{
    assert(_dev_handle);
    
    if (ds_set_device_channel_name(ch_index, name) == SR_OK){
        config_changed();
        return true;
    }
    return false;
}

uint64_t DeviceAgent::get_sample_limit()
{
    assert(_dev_handle);

    uint64_t v;
    GVariant* gvar = NULL;

    ds_get_actived_device_config(NULL, NULL, SR_CONF_LIMIT_SAMPLES, &gvar);

	if (gvar != NULL) {
        v = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
	}
    else {
		v = 0U;
	}

	return v;
}

uint64_t DeviceAgent::get_sample_rate()
{
    assert(_dev_handle);

    uint64_t v;
    GVariant* gvar = NULL;

    int key;

    if(is_external_clock()) {
        key = SR_CONF_EXT_SAMPLERATE;
    } else {
        key = SR_CONF_SAMPLERATE;
    }

    ds_get_actived_device_config(NULL, NULL, key, &gvar);
	if (gvar != NULL) {
        v = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
	}
    else {
		v = 0U;
	}

	return v;
}

bool DeviceAgent::is_external_clock()
{
    assert(_dev_handle);

    bool ct = false;
    GVariant* gvar = NULL;

    ds_get_actived_device_config(NULL, NULL, SR_CONF_CLOCK_TYPE, &gvar);
    if (gvar != NULL) {
        ct = g_variant_get_boolean(gvar);
        g_variant_unref(gvar);
    }

    return ct;
}

uint64_t DeviceAgent::get_time_base()
{
    assert(_dev_handle);

    uint64_t v;
    GVariant* gvar = NULL;

    ds_get_actived_device_config(NULL, NULL, SR_CONF_TIMEBASE, &gvar);

	if (gvar != NULL) {
        v = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
	}
    else {
		v = 0U;
	}

	return v;
}

double DeviceAgent::get_sample_time()
{
    assert(_dev_handle);

    uint64_t sample_rate = get_sample_rate();
    uint64_t sample_limit = get_sample_limit();
    double sample_time;

    if (sample_rate == 0)
        sample_time = 0;
    else
        sample_time = sample_limit * 1.0 / sample_rate;

    return sample_time;
}

const GSList* DeviceAgent::get_device_mode_list()
{
    assert(_dev_handle);
    return ds_get_actived_device_mode_list();
}

bool DeviceAgent::is_trigger_enabled()
{
    assert(_dev_handle);
    if (ds_trigger_is_enabled() > 0){
        return true;
    }
    return false;
}

bool DeviceAgent::start(bool instant)
{
    assert(_dev_handle);

    if (is_logic_stacking()){
        if (!is_logic_stacking_ready())
            return false;

        uint16_t secondary_trigger_percent = 0;
        if (!configure_stacking_capture(instant, &secondary_trigger_percent))
            return false;

        ds_collect_target targets[2];
        memset(targets, 0, sizeof(targets));

        targets[0].handle = _logic_stacking_config.secondary_handle;
        targets[0].role = instant ? DS_COLLECT_TARGET_INSTANT : DS_COLLECT_TARGET_SECONDARY_SYNC_RISING;
        targets[0].sync_channel = _logic_stacking_config.secondary_sync_channel;
        targets[0].trigger_pos_percent = secondary_trigger_percent;

        targets[1].handle = _logic_stacking_config.master_handle;
        targets[1].role = instant ? DS_COLLECT_TARGET_INSTANT : DS_COLLECT_TARGET_MASTER_CURRENT_TRIGGER;
        targets[1].sync_channel = 0;

        if (ds_start_collect_multi(targets, 2) == SR_OK)
            return true;

        return false;
    }

    if (ds_start_collect() == SR_OK){
        return true;
    }
    return false;
}

bool DeviceAgent::stop()
{
    assert(_dev_handle);

    if (is_logic_stacking_ready() && ds_stop_collect_multi() == SR_OK){
        return true;
    }

    if (ds_stop_collect() == SR_OK){
        return true;
    }
    return false;
}

void DeviceAgent::release()
{
    ds_release_actived_device();
}

bool DeviceAgent::have_enabled_channel()
{
    assert(_dev_handle);
    if (is_logic_stacking())
        return _logic_stacking_channel_map.empty() == false;

    return ds_channel_is_enabled() > 0;
}

void DeviceAgent::config_changed()
{
    if (_callback != NULL){
        _callback->DeviceConfigChanged();
    }
}

bool DeviceAgent::channel_is_enable(int index)
{  
    for (const GSList *l = get_channels(); l; l = l->next)
    {
        const sr_channel *const probe = (const sr_channel *)l->data;
        if (probe->index == index)
            return probe->enabled;          
    }

    return false;
}
 
//---------------device config-----------/

int DeviceAgent::get_work_mode()
{
    return ds_get_actived_device_mode();
}

const struct sr_config_info *DeviceAgent::get_config_info(int key)
{
    return ds_get_actived_device_config_info(key);
}

bool DeviceAgent::get_device_status(struct sr_status &status, gboolean prg)
{   
    assert(_dev_handle);

    if (ds_get_actived_device_status(&status, prg) == SR_OK)
    {
        return true;
    }
    return false;
}

struct sr_config *DeviceAgent::new_config(int key, GVariant *data)
{
    return ds_new_config(key, data);
}

void DeviceAgent::free_config(struct sr_config *src)
{
    ds_free_config(src);
}

bool DeviceAgent::is_collecting()
{
    return ds_is_collecting() > 0;
}

GSList *DeviceAgent::get_channels()
{
    assert(_dev_handle);
    if (is_logic_stacking()){
        if (_logic_stacking_channels == NULL)
            rebuild_stacking_channels();
        return _logic_stacking_channels;
    }

    return ds_get_actived_device_channels();
}

bool DeviceAgent::set_logic_stacking_config(const pv::LogicStackingConfig &config)
{
    if (config.enabled && !config.has_channel_model())
        return false;

    const bool channel_model_changed =
        !_logic_stacking_config.has_same_channel_model(config);

    _logic_stacking_config = config;

    if (channel_model_changed){
        clear_stacking_channels();

        if (_logic_stacking_config.has_channel_model())
            rebuild_stacking_channels();
    }

    config_changed();
    return true;
}

const pv::LogicStackingConfig& DeviceAgent::logic_stacking_config() const
{
    return _logic_stacking_config;
}

bool DeviceAgent::is_logic_stacking() const
{
    // Stacking may be configured even while saved analyzer handles still need remapping.
    return _logic_stacking_config.has_channel_model() &&
           _driver_name == "DSLogic";
}

bool DeviceAgent::is_logic_stacking_ready() const
{
    // Capture/device writes require both handles to be valid and the master to be active.
    return is_logic_stacking() &&
           _logic_stacking_config.is_valid() &&
           _dev_handle == _logic_stacking_config.master_handle;
}

const std::vector<pv::LogicStackingChannel>& DeviceAgent::logic_stacking_channels()
{
    if (is_logic_stacking() && _logic_stacking_channels == NULL)
        rebuild_stacking_channels();

    return _logic_stacking_channel_map;
}

void DeviceAgent::clear_stacking_channels()
{
    for (GSList *l = _logic_stacking_channels; l; l = l->next){
        sr_channel *ch = (sr_channel*)l->data;
        if (ch != NULL){
            if (ch->name)
                g_free(ch->name);
            if (ch->trigger)
                g_free(ch->trigger);
            g_free(ch);
        }
    }

    if (_logic_stacking_channels != NULL)
        g_slist_free(_logic_stacking_channels);

    _logic_stacking_channels = NULL;
    _logic_stacking_channel_map.clear();
}

sr_channel* DeviceAgent::make_stacking_channel(int analyzer, int physical_index)
{
    sr_channel *ch = (sr_channel*)g_try_malloc0(sizeof(sr_channel));
    if (ch == NULL)
        return NULL;

    const int global_index = analyzer == 0 ? physical_index : 32 + physical_index;
    QString name = QString("A%1-D%2").arg(analyzer + 1).arg(physical_index);

    ch->index = global_index;
    ch->type = SR_CHANNEL_LOGIC;
    ch->enabled = TRUE;
    ch->name = g_strdup(name.toLocal8Bit().constData());
    ch->trigger = g_strdup("");
    ch->bits = 1;

    return ch;
}

void DeviceAgent::rebuild_stacking_channels()
{
    clear_stacking_channels();

    // Build synthetic channels from the saved model, even if hardware remapping is pending.
    if (!_logic_stacking_config.has_channel_model())
        return;

    for (int analyzer = 0; analyzer < 2; analyzer++){
        for (int physical = 0; physical < 32; physical++){
            const bool is_sync = analyzer == 1 &&
                                 physical == _logic_stacking_config.secondary_sync_channel;
            const bool visible = _logic_stacking_config.show_sync_channel || !is_sync;

            pv::LogicStackingChannel mapped;
            mapped.analyzer = analyzer;
            mapped.physical_index = physical;
            mapped.global_index = analyzer == 0 ? physical : 32 + physical;
            mapped.visible = visible;
            mapped.sync = is_sync;

            if (!visible)
                continue;

            sr_channel *ch = make_stacking_channel(analyzer, physical);
            if (ch == NULL){
                dsv_err("DeviceAgent::rebuild_stacking_channels, malloc failed.");
                clear_stacking_channels();
                return;
            }

            _logic_stacking_channels = g_slist_append(_logic_stacking_channels, ch);
            _logic_stacking_channel_map.push_back(mapped);
        }
    }
}

bool DeviceAgent::set_handle_config_bool(ds_device_handle handle, int key, bool value)
{
    GVariant *gvar = g_variant_new_boolean(value);
    return ds_set_device_config_by_handle(handle, NULL, NULL, key, gvar) == SR_OK;
}

bool DeviceAgent::set_handle_config_uint64(ds_device_handle handle, int key, uint64_t value)
{
    GVariant *gvar = g_variant_new_uint64(value);
    return ds_set_device_config_by_handle(handle, NULL, NULL, key, gvar) == SR_OK;
}

bool DeviceAgent::set_handle_config_int16(ds_device_handle handle, int key, int value)
{
    GVariant *gvar = g_variant_new_int16(value);
    return ds_set_device_config_by_handle(handle, NULL, NULL, key, gvar) == SR_OK;
}

bool DeviceAgent::set_handle_config_double(ds_device_handle handle, int key, double value)
{
    GVariant *gvar = g_variant_new_double(value);
    return ds_set_device_config_by_handle(handle, NULL, NULL, key, gvar) == SR_OK;
}

bool DeviceAgent::enable_handle_probe(ds_device_handle handle, int probe_index, bool enable)
{
    return ds_enable_device_channel_index_by_handle(handle, probe_index, enable ? TRUE : FALSE) == SR_OK;
}

bool DeviceAgent::configure_stacking_capture(bool instant, uint16_t *secondary_trigger_percent)
{
    if (!is_logic_stacking_ready())
        return false;

    uint64_t samplerate = get_sample_rate();
    uint64_t samplelimit = get_sample_limit();
    uint64_t hw_depth = 0;
    const uint16_t master_trigger_percent = ds_trigger_get_pos();
    StackingSecondaryTiming secondary_timing;
    secondary_timing.samplelimit = samplelimit;
    secondary_timing.trigger_percent = master_trigger_percent;

    if (samplerate == 0 || samplelimit == 0)
        return false;

    bool ok = true;
    ok = set_config_bool(SR_CONF_INSTANT, instant) && ok;
    ok = set_config_bool(SR_CONF_RLE, false) && ok;
    ok = set_config_bool(SR_CONF_LOOP_MODE, false) && ok;
    ok = set_config_int16(SR_CONF_OPERATION_MODE, LO_OP_BUFFER) && ok;
    ok = set_config_int16(SR_CONF_BUFFER_OPTIONS, SR_BUF_UPLOAD) && ok;
    ok = set_config_int16(SR_CONF_CHANNEL_MODE, DSL_BUFFER250x32) && ok;

    ok = set_handle_config_bool(_logic_stacking_config.secondary_handle, SR_CONF_INSTANT, instant) && ok;
    ok = set_handle_config_bool(_logic_stacking_config.secondary_handle, SR_CONF_RLE, false) && ok;
    ok = set_handle_config_bool(_logic_stacking_config.secondary_handle, SR_CONF_LOOP_MODE, false) && ok;
    ok = set_handle_config_int16(_logic_stacking_config.secondary_handle, SR_CONF_OPERATION_MODE, LO_OP_BUFFER) && ok;
    ok = set_handle_config_int16(_logic_stacking_config.secondary_handle, SR_CONF_BUFFER_OPTIONS, SR_BUF_UPLOAD) && ok;
    ok = set_handle_config_int16(_logic_stacking_config.secondary_handle, SR_CONF_CHANNEL_MODE, DSL_BUFFER250x32) && ok;

    // Hidden sync inputs still need to be sampled so the merger can align the streams.
    for (int i = 0; i < 32; i++){
        ok = enable_probe(i, true) && ok;
        ok = enable_handle_probe(_logic_stacking_config.secondary_handle, i, true) && ok;
    }

    if (!ok)
        return false;

    // Give the secondary enough post-trigger data when early master triggers shift A1 left.
    if (!instant && get_config_uint64(SR_CONF_HW_DEPTH, hw_depth))
        secondary_timing = stacking_secondary_timing(samplelimit,
                                                    master_trigger_percent,
                                                    hw_depth);

    ok = set_handle_config_uint64(_logic_stacking_config.secondary_handle, SR_CONF_SAMPLERATE, samplerate) && ok;
    ok = set_handle_config_uint64(_logic_stacking_config.secondary_handle,
                                  SR_CONF_LIMIT_SAMPLES,
                                  secondary_timing.samplelimit) && ok;
    if (secondary_trigger_percent != NULL)
        *secondary_trigger_percent = secondary_timing.trigger_percent;

    bool clock_bool = false;
    if (get_config_bool(SR_CONF_CLOCK_TYPE, clock_bool))
        ok = set_handle_config_bool(_logic_stacking_config.secondary_handle, SR_CONF_CLOCK_TYPE, clock_bool) && ok;

    if (get_config_bool(SR_CONF_CLOCK_EDGE, clock_bool))
        ok = set_handle_config_bool(_logic_stacking_config.secondary_handle, SR_CONF_CLOCK_EDGE, clock_bool) && ok;

    double v_th = 0;
    if (get_config_double(SR_CONF_VTH, v_th))
        ok = set_handle_config_double(_logic_stacking_config.secondary_handle, SR_CONF_VTH, v_th) && ok;

    int filter = 0;
    if (get_config_int16(SR_CONF_FILTER, filter))
        ok = set_handle_config_int16(_logic_stacking_config.secondary_handle, SR_CONF_FILTER, filter) && ok;

    uint64_t ext_samplerate = 0;
    if (get_config_uint64(SR_CONF_EXT_SAMPLERATE, ext_samplerate))
        ok = set_handle_config_uint64(_logic_stacking_config.secondary_handle,
                                      SR_CONF_EXT_SAMPLERATE,
                                      ext_samplerate) && ok;

    return ok;
}

 int DeviceAgent::get_hardware_operation_mode()
 {
    assert(_dev_handle);

    int mode_val = 0;
    if (get_config_int16(SR_CONF_OPERATION_MODE, mode_val)){                  
        return mode_val;
    }
    return -1;
 }

 bool DeviceAgent::is_stream_mode()
 { 
    return get_hardware_operation_mode() == LO_OP_STREAM;
 }

 bool DeviceAgent::check_firmware_version()
 {
    assert(_dev_handle);

    int st = -1;
    if (ds_get_actived_device_init_status(&st) == SR_OK){
        if (st == SR_ST_INCOMPATIBLE){
            return false;
        }
    }
    return true;
 }

 QString DeviceAgent::get_demo_operation_mode()
 {
    assert(_dev_handle);

    if (is_demo() == false){
        assert(false);
    }        
    
    QString pattern_mode;
    if(get_config_string(SR_CONF_PATTERN_MODE, pattern_mode) == false)
    {
        assert(false);
    }
    return pattern_mode;
 }

GVariant* DeviceAgent::get_config_list(const sr_channel_group *group, int key)
{
    assert(_dev_handle);

    GVariant *data = NULL;

    int ret = ds_get_actived_device_config_list(group, key, &data);
    if (ret != SR_OK){
        if (ret != SR_ERR_NA)
            dsv_detail("%s%d", "WARNING: Failed to get config list, key:", key); 
        
        if (data != NULL){
            dsv_warn("%s%d", "WARNING: Failed to get config list, but data is not null. key:", key); 
        }
        data = NULL;
    }

    return data;
}

GVariant* DeviceAgent::get_config(int key, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle); 
    GVariant *data = NULL;
    
    int ret = ds_get_actived_device_config(ch, cg, key, &data);
    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR:DeviceAgent::get_config, Failed to get value of config id:", key);
    }
    return data;
}

bool DeviceAgent::have_config(int key, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);

    if (gvar != NULL){
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config(int key, GVariant *data, const sr_channel *ch, const sr_channel_group *cg)
 {
    assert(_dev_handle);

    int ret = ds_set_actived_device_config(ch, cg, key, data);
    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR:DeviceAgent::set_config, Failed to set value of config id:", key);
        return false;
    }

    config_changed();
    return true;
 }

bool DeviceAgent::set_stacking_shared_config(int key, GVariant *data)
{
    assert(_dev_handle);
    assert(data);

    if (!is_logic_stacking_ready())
        return set_config(key, data);

    g_variant_ref_sink(data);

    int ret = ds_set_actived_device_config(NULL, NULL, key, data);
    if (ret != SR_OK){
        g_variant_unref(data);
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR:DeviceAgent::set_stacking_shared_config, Failed to set master config id:", key);
        return false;
    }

    ret = ds_set_device_config_by_handle(_logic_stacking_config.secondary_handle,
                                         NULL,
                                         NULL,
                                         key,
                                         data);
    g_variant_unref(data);

    if (ret != SR_OK){
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR:DeviceAgent::set_stacking_shared_config, Failed to set secondary config id:", key);
        config_changed();
        return false;
    }

    config_changed();
    return true;
}

 bool DeviceAgent::get_config_int32(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
 {  
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_int32(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
 }

 bool DeviceAgent::set_config_int32(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
 {
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_int32(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_int32, Failed to set value of config id:", key);
        return false;
    }
    return true;
 }

 bool DeviceAgent::get_config_string(int key, QString &value, const sr_channel *ch, const sr_channel_group *cg)
 {
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        const gchar *s = g_variant_get_string(gvar, NULL);
        value = QString(s);
        g_variant_unref(gvar);
        return true;
    }

    return false;
 }

 bool DeviceAgent::set_config_string(int key, const char *value, const sr_channel *ch, const sr_channel_group *cg)
 {
    assert(value);
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_string(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_string, Failed to set value of config id:", key);
        return false;
    }
    return true;
 }

bool DeviceAgent::get_config_bool(int key, bool &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        gboolean v = g_variant_get_boolean(gvar);
        value = v > 0;
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_bool(int key, bool value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_boolean(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_bool, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

bool DeviceAgent::get_config_uint64(int key, uint64_t &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_uint64(int key, uint64_t value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_uint64(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_uint64, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

bool DeviceAgent::get_config_uint16(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_uint16(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_uint16(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_uint16(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_uint16, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

bool DeviceAgent::get_config_uint32(int key, uint32_t &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_uint32(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_uint32(int key, uint32_t value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_uint32(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_uint32, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

bool DeviceAgent::get_config_int16(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_int16(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_int16(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_int16(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_int16, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

bool DeviceAgent::get_config_byte(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_byte(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_byte(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_byte((uint8_t)value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_byte, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

bool DeviceAgent::get_config_double(int key, double &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant* gvar = get_config(key, ch, cg);
    
    if (gvar != NULL) {
        value = g_variant_get_double(gvar);
        g_variant_unref(gvar);
        return true;
    }

    return false;
}

bool DeviceAgent::set_config_double(int key, double value, const sr_channel *ch, const sr_channel_group *cg)
{
    assert(_dev_handle);

    GVariant *gvar = g_variant_new_double(value);
    int ret = ds_set_actived_device_config(ch, cg, key, gvar);

    if (ret != SR_OK)
    {
        if (ret != SR_ERR_NA)
            dsv_err("%s%d", "ERROR: DeviceAgent::set_config_double, Failed to set value of config id:", key);
        return false;
    }
    return true;
}

//---------------device config end -----------/
