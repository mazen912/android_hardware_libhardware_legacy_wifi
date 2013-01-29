/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/wireless.h>
#include "hardware_legacy/wifi.h"
#include "libwpa_client/wpa_ctrl.h"

#define LOG_TAG "RTL8188_WiFiHW10"
#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif

extern int ifc_init();
extern int ifc_up(char *name);
extern void ifc_close();

static int load_driver(const char *module_path, const char *module_name, const char *module_arg);

/*
 * Extern from wifi.c
 */
extern int insmod(const char *filename, const char *args);
extern int rmmod(const char *modname);
extern int check_driver_loaded();
extern int is_wifi_driver_loaded();

#define WIFI_DRIVER_IFNAME_AP           "wl0.1"
#define DRIVER_MODULE_ARG_AP            "ifname="WIFI_DRIVER_IFNAME_AP
static char g_ifname[PROPERTY_VALUE_MAX];

#define WIFI_DRIVER_MODULE_PATH         "/system/lib/modules/wlan.ko"
#define WIFI_DRIVER_MODULE_NAME         "wlan"
#define WIFI_DRIVER_MODULE_ARG          ""

static const char DRIVER_MODULE_NAME[]  = WIFI_DRIVER_MODULE_NAME;
static const char DRIVER_MODULE_TAG[]   = WIFI_DRIVER_MODULE_NAME " ";
static const char DRIVER_PROP_NAME[]    = "wlan.driver.status";

int get_priv_func_num(int sockfd, const char *ifname, const char *fname) {
        struct iwreq wrq;
        struct iw_priv_args *priv_ptr;
        int i, ret;
        char *buf;

        if( NULL == (buf=(char *)malloc(4096)) ) {
                ret = -ENOMEM;
                goto exit;
        }

        strncpy(wrq.ifr_name, ifname, sizeof(wrq.ifr_name));
        wrq.u.data.pointer = buf;
        wrq.u.data.length = 4096 / sizeof(struct iw_priv_args);
        wrq.u.data.flags = 0;
        if ((ret = ioctl(sockfd, SIOCGIWPRIV, &wrq)) < 0) {
                LOGE("SIOCGIPRIV failed: %d %s", ret, strerror(errno));
                goto exit;
        }

        ret =-1;
        priv_ptr = (struct iw_priv_args *)wrq.u.data.pointer;
        for(i=0;(i < wrq.u.data.length);i++) {
                if (strcmp(priv_ptr[i].name, fname) == 0) {
                        ret = priv_ptr[i].cmd;
                        break;
                }
        }

exit:
	if(buf)
                free(buf);

        return ret;
}

int rtl871x_drv_rereg_nd_name_fd(int sockfd, const char *ifname, const int fnum, const char * new_ifname)
{
        struct iwreq wrq;
        int ret;
        char ifname_buf[IFNAMSIZ];
        strncpy(wrq.ifr_name, ifname, sizeof(wrq.ifr_name));
        strncpy(ifname_buf, new_ifname, IFNAMSIZ);
        ifname_buf[IFNAMSIZ-1] = 0;
        wrq.u.data.pointer = ifname_buf;
        wrq.u.data.length = strlen(ifname_buf)+1;
        wrq.u.data.flags = 0;
		LOGE("rtl8188CU change interface %s -> %s, fnum = 0x%02x", ifname, new_ifname, fnum);
        ret = ioctl(sockfd, fnum, &wrq);

        if (ret) {
                LOGE("ioctl - failed: %d %s", ret, strerror(errno));
        }
        return ret;
}

int rtl871x_drv_rereg_nd_name(const char *ifname, const char *new_ifname)
{
        int sockfd;
        int ret;

        sockfd = socket(PF_INET, SOCK_DGRAM, 0);
        if (sockfd< 0) {
                perror("socket[PF_INET,SOCK_DGRAM]");
                ret = -1;
                goto bad;
        }

        ret = rtl871x_drv_rereg_nd_name_fd(
                sockfd
                , ifname
                , get_priv_func_num(sockfd, ifname, "rereg_nd_name")
                , new_ifname
        );

        close(sockfd);
bad:
        return ret;
}

int DetectWifiIfNameFromProc()
{
        char linebuf[1024];
        FILE *f = fopen("/proc/net/wireless", "r");

        g_ifname[0] = '\0';
        if (f) {
                while(fgets(linebuf, sizeof(linebuf)-1, f)) {

                        if (strchr(linebuf, ':')) {
                                char *dest = g_ifname;
                                char *p = linebuf;

                                while(*p && isspace(*p))
                                        ++p;
                                while (*p && *p != ':') {
                                        *dest++ = *p++;
                                }
                                *dest = '\0';
                                LOGD("DetectWifiIfNameFromProc: %s\n", g_ifname);
                                fclose(f);
                                return 0;
                        }
                }
                fclose(f);
        }
        return -1;
}

char *getWifiIfname()
{
        DetectWifiIfNameFromProc();
        return g_ifname;
}

static int load_driver(const char *module_path, const char *module_name, const char *module_arg)
{
        pid_t pid;
        char drvPath[PROPERTY_VALUE_MAX];
        char defIfname[256];
        char local_ifname[PROPERTY_VALUE_MAX];

        #ifdef USE_DRIVER_PROP_PATH_NAME
        if ( !property_get(DRIVER_PROP_PATH_NAME, drvPath, module_path)) {
                LOGD("Cannot get driver path property\n");
                goto err_exit;
        }
        #else
        snprintf(drvPath, sizeof(drvPath)-1, "%s", module_path);
        #endif

        LOGD("driver path %s",drvPath );

        if (insmod(drvPath, module_arg)!=0) {
                LOGD("Fail to insmod driver\n");
                goto err_exit;
        }

        if(*getWifiIfname() == '\0') {
                LOGD("load_driver: getWifiIfname fail");
                goto err_rmmod;
        }

        strncpy(local_ifname,getWifiIfname(),PROPERTY_VALUE_MAX);
        local_ifname[PROPERTY_VALUE_MAX-1]=0;


        LOGD("insmod %s done, ifname:%s",drvPath, local_ifname);
		pid = vfork();
        if (pid!=0) {
                //wait the child to do ifup and check the result...

                int ret;
                int status;
                int cnt = 10;

                while ( (ret=waitpid(pid, &status, WNOHANG)) == 0 && cnt-- > 0 ) {
                        LOGD("still waiting...\n");
                        sleep(1);
                }

                LOGD("waitpid finished ret %d\n", ret);
                if (ret>0) {
                        if (WIFEXITED(status)) {
                                LOGD("child process exited normally, with exit code %d\n", WEXITSTATUS(status));
                        } else {
                                LOGD("child process exited abnormally\n");
                                goto err_rmmod;
                        }
                        return 0;
                }
                goto err_rmmod;


        } else {
                //do ifup here, and let parent to monitor the result...
		if (strcmp(local_ifname, "sta") == 0)
                        _exit(0);

                if (ifc_init() < 0)
                        _exit(-1);

                if (ifc_up(local_ifname)) {
                        LOGD("failed to bring up interface %s: %s\n", local_ifname, strerror(errno));
                        _exit(-1);
                }
                ifc_close();
                _exit(0);
        }

err_rmmod:
        rmmod(module_name);
err_exit:
        return -1;
}

int wifi_load_ap_driver()
{
    char driver_status[PROPERTY_VALUE_MAX];
    int ret;

#if !defined(CONFIG_WIFI_BUILT_IN_KERNEL)

//    if (check_driver_loaded(DRIVER_MODULE_TAG, DRIVER_PROP_NAME)) {
    if (is_wifi_driver_loaded(DRIVER_MODULE_TAG, DRIVER_PROP_NAME)) {

        if(*getWifiIfname() == '\0') {
                LOGD("wifi_load_ap_driver: getWifiIfname fail");
                return -1;
        }
        rtl871x_drv_rereg_nd_name(getWifiIfname(), WIFI_DRIVER_IFNAME_AP);
        return 0;
    }
    if ( (ret=load_driver(WIFI_DRIVER_MODULE_PATH, DRIVER_MODULE_NAME, DRIVER_MODULE_ARG_AP))==0 ) {
        property_set(DRIVER_PROP_NAME, "ok");
        LOGI("wifi_load_ap_driver: return 0\n");
        return 0;
    }

    property_set(DRIVER_PROP_NAME, "timeout");

    LOGI("wifi_load_ap_driver: return -1\n");
    return -1;
#else
	// Could do wifi power switch here, ex:
        // 1. insmod specific .ko to do power switch or
        // 2. open and write to power switch file...
        //

        // After power on, we should got wifi interface here
        if(*getWifiIfname() == '\0') {
                LOGD("wifi_load_ap_driver: getWifiIfname fail");
                return -1;
        }
        rtl871x_drv_rereg_nd_name(getWifiIfname(), WIFI_DRIVER_IFNAME_AP);
        return 0;
#endif
}
