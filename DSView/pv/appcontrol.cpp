/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#include "appcontrol.h"

#include <libsigrok.h>
#include <libsigrokdecode.h>
#include <QDir>
#include <QCoreApplication>
#include <QWidget>
#include <string>
#include <assert.h>
#include "sigsession.h"
#include "dsvdef.h"
#include "config/appconfig.h"
#include "log.h"
#include "utility/path.h"
#include "utility/encoding.h"

AppControl::AppControl()
{
    _topWindow = NULL; 
    _session = new pv::SigSession();
}

AppControl::AppControl(AppControl &o)
{
    (void)o;
}
 
AppControl::~AppControl()
{ 
   // DESTROY_OBJECT(_session);
}

AppControl* AppControl::Instance()
{
    static AppControl *ins = NULL;
    if (ins == NULL){
        ins = new AppControl();
    }
    return ins;
}

void AppControl::Destroy(){
     
} 

bool AppControl::Init()
{  
    pv::encoding::init();

    QString qs;
    std::string cs;

    qs = GetAppDataDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetAppDataDir:\"%s\"", cs.c_str());
    cs = pv::path::ConvertPath(qs);
    ds_set_user_data_dir(cs.c_str());

    qs = GetFirmwareDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetFirmwareDir:\"%s\"", cs.c_str());

    qs = GetUserDataDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetUserDataDir:\"%s\"", cs.c_str());

    qs = GetDecodeScriptDir();
    cs = pv::path::ToUnicodePath(qs);
    dsv_info("GetDecodeScriptDir:\"%s\"", cs.c_str());
    //---------------end print directorys.

    _session->init();

    srd_log_set_context(dsv_log_context());

#if defined(_WIN32)
    // The packaged app carries its own copy of the Python standard library
    // next to DSView.exe (see the "pylib" folder produced by the Windows CI
    // build), since CPython's own relative-path auto-detection isn't
    // reliable once the interpreter DLL is copied out of its original
    // MSYS2/mingw64 prefix layout into a flat distribution folder. Without
    // PYTHONHOME pointing at it, Py_InitializeEx() fails to find the stdlib
    // and calls Py_FatalError(), which aborts the process before any window
    // is shown and before any of our own logging can run.
    QString pythonHome = QCoreApplication::applicationDirPath() + "/pylib";
    QDir pydir;
    if (pydir.exists(pythonHome)){
        const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(pythonHome.utf16());
        srd_set_python_home(pyhome);

        // PYTHONHOME alone isn't enough: MSYS2's Python is built with a
        // Unix-style prefix layout, so Py_SetPythonHome() makes CPython look
        // for the stdlib under "<home>/lib/pythonX.Y/...", not directly
        // inside "<home>/". Our bundled copy sits flat in "pylib/" (its
        // encodings/, os.py etc. are direct children), so point PYTHONPATH
        // straight at it too - this is read during interpreter bootstrap,
        // before Py_SetPythonHome's own (mismatched) landmark search would
        // otherwise fail to find "encodings" and abort the process.
        QString libDynload = pythonHome + "/lib-dynload";
        QString pythonPath = pythonHome;
        if (pydir.exists(libDynload))
            pythonPath += ";" + libDynload;
        qputenv("PYTHONPATH", pythonPath.toUtf8());
    }
#endif
    
    //the python script path of decoder
    char path[256] = {0};
    QString dir = GetDecodeScriptDir();   
    strcpy(path, dir.toUtf8().data());

    // Initialise libsigrokdecode
    if (srd_init(path) != SRD_OK)
    { 
        dsv_err("ERROR: libsigrokdecode init failed.");
        return false;
    }

    // Load the protocol decoders
    if (srd_decoder_load_all() != SRD_OK)
    {
        dsv_err("ERROR: load the protocol decoders failed.");
        return false;
    }
 
    return true;
}

bool AppControl::Start()
{  
    _session->Open(); 
    return true;
}

 void AppControl::Stop()
 {
    _session->Close();  
 }

void AppControl::UnInit()
{  
    // Destroy libsigrokdecode
    srd_exit();

    _session->uninit();
}

bool AppControl::TopWindowIsMaximized()
{
    if (_topWindow != NULL){
        return _topWindow->isMaximized();
    }
    return false;
}