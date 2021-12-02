#include <Common.h>
#include "stdlib.h"
#include "stdio.h"
#include <libnotify/notify.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

typedef struct mydeviceentry {
    struct mydeviceentry *next;
    unsigned short deviceID;
    char *deviceName;
    uint8_t notifiedAtLeastOnce;
    uint8_t lastNotifyCharging;
    uint8_t lastNotifyPercentage;
} mydeviceentry;


static volatile mydeviceentry *devices=0;

static pthread_mutex_t deviceListMutex;

static void showNotification(char *msg)
{
    NotifyNotification* n = notify_notification_new ("jabrac", msg,0);
    notify_notification_set_timeout(n, 3000); // show for 3 seconds

    if ( ! notify_notification_show(n, 0) )
    {
        syslog(LOG_ERR,"Failed to show notification %s",msg);
    }
}

static void lockDeviceList()
{
    if (0 != (errno = pthread_mutex_lock(&deviceListMutex)))
    {
        perror("pthread_mutex_lock failed");
        exit(EXIT_FAILURE);
    }
}

static void unlockDeviceList() {
    if (0 != (errno = pthread_mutex_unlock(&deviceListMutex)))
    {
        perror("pthread_mutex_unlock failed");
        exit(EXIT_FAILURE);
    }
}

static void checkBatteryStatus() {

    lockDeviceList();

    if ( devices )
    {
        // count devices
        int deviceCount = 0;
        mydeviceentry *current = devices;
        while ( current ) {
            current = current->next;
            deviceCount++;
        }

        // copy IDs so we can poll battery status without having to hold the lock
        unsigned short *ids = calloc(deviceCount,sizeof(unsigned short));
        current = devices;
        for( int i = 0 ; current ; i++, current = current->next ) {
            ids[i] = current->deviceID;
        }
        unlockDeviceList();

        // poll battery status while not locking the deviceInfoList
        Jabra_BatteryStatus* batteryStatus;
        for ( int i = 0 ; i < deviceCount ; i++) {
              // LIBRARY_API Jabra_ReturnCode Jabra_GetBatteryStatusV2(unsigned short deviceID, Jabra_BatteryStatus** batteryStatus);
              Jabra_ReturnCode rc = Jabra_GetBatteryStatusV2( ids[i], &batteryStatus );
              if ( rc == Return_Ok )
              {
                  // find entry and notify if necessary
                  lockDeviceList();
                  current = devices;
                  while ( current && current->deviceID != ids[i] ) {
                      current = current->next;
                  }
                  if ( current )
                  {
                    uint8_t level = batteryStatus->levelInPercent;
                    uint8_t charging = batteryStatus->charging;

                    if ( ! current->notifiedAtLeastOnce || current->lastNotifyCharging != charging || ( current->lastNotifyPercentage != level && (level % 10 ) == 0 ) )
                    {
                        char msg[200];
                        const char *format;
                        if ( charging ) {
                            format = "Battery of '%s' is now at %d %% (charging)";
                        } else {
                            format = "Battery of '%s' is now at %d %%";
                        }
                        snprintf(msg,sizeof(msg),format,current->deviceName,level);
                        showNotification( msg );

                        current->notifiedAtLeastOnce=1;
                        current->lastNotifyPercentage=level;
                        current->lastNotifyCharging=charging ? 1:0;
                    }
                  }
                  Jabra_FreeBatteryStatus(batteryStatus);
                  unlockDeviceList();
              } else if ( rc != Not_Supported) { // device has no battery
                  syslog(LOG_ERR,"Failed to query battery status for device %04x: error %d", ids[i], rc );
              }
        }


        // free memory
        free(ids);
    } else {
        unlockDeviceList();
    }
}

static void delDevice(unsigned short deviceID)
{
    syslog(LOG_INFO,"DETACHED: device with ID %04x", deviceID);

    lockDeviceList();

    mydeviceentry *previous=0;
    mydeviceentry *current=devices;
    while ( current )
    {
      if ( current->deviceID == deviceID )
      {
          if ( previous == 0 ) {
            devices = current->next;
          } else {
            previous->next = current->next;
          }
          free(current->deviceName);
          free(current);
          break;
      }
      previous = current;
      current = current->next;
    }

    unlockDeviceList();
}

static void addDevice(Jabra_DeviceInfo* info) {

    syslog(LOG_INFO,"ATTACHED: device with ID %04x (%s)", info->deviceID, info->deviceName);

    lockDeviceList();

    mydeviceentry *newEntry = calloc(1,sizeof(mydeviceentry));
    newEntry->deviceID = info->deviceID;
    newEntry->deviceName = strdup(info->deviceName);

    if ( devices )  {
      newEntry->next = devices;
      devices = newEntry;
    } else {
      devices = newEntry;
    }

    unlockDeviceList();
}

static void daemonize()
{
    pid_t pid = fork();

    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    umask(0);

    chdir("/");

    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close (x);
    }

    openlog ("jabrac", LOG_PID, LOG_DAEMON);
}

static void deviceAttached(Jabra_DeviceInfo deviceInfo) {
    addDevice(&deviceInfo);
    Jabra_FreeDeviceInfo(deviceInfo);
}

static void deviceRemoved(unsigned short deviceID) {
    delDevice(deviceID);
}

int main(int argc, char** args) {

  int runAsDaemon=0;
  if ( argc > 0 )
  {

    for ( int i = 0 ; i < argc ; i++) {
      if ( strcmp("-h", args[i]) == 0 || strcmp("--help",args[i]) == 0 ) {
        printf("Usage: [-h|--help] [-d|--daemon]\n");
        return 1;
      } else if ( strcmp("-d", args[i]) == 0 || strcmp("--daemon",args[i]) == 0 ) {
        runAsDaemon=1;
      }
    }
  }

  if ( runAsDaemon ) {
    daemonize();
  }

  notify_init("jabrac");

  Jabra_SetAppID("fb56-2b8723b1-9b05-4b1c-a3b6-960b79b75f03");
  
  /*
    void(*FirstScanForDevicesDoneFunc)(void),
    void(*DeviceAttachedFunc)(Jabra_DeviceInfo deviceInfo),
    void(*DeviceRemovedFunc)(unsigned short deviceID),
    void(*ButtonInDataRawHidFunc)(unsigned short deviceID, unsigned short usagePage, unsigned short usage, bool buttonInData),
    void(*ButtonInDataTranslatedFunc)(unsigned short deviceID, Jabra_HidInput translatedInData, bool buttonInData),
  */
  if ( ! Jabra_InitializeV2(0,deviceAttached,deviceRemoved,0,0,false,0) ) {
    if ( runAsDaemon ) {
      syslog(LOG_ERR,"Failed to initialize library\n");
    } else {
      printf("Failed to initialize library\n");
    }
    return 1;
  }

  showNotification("jabrac started");
  sleep(2);

  while(1)
  {
    checkBatteryStatus();
    sleep(60);
  }
  return 0;
}
