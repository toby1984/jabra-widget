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
#include <time.h>

#define PID_LOCK_FILE "/var/lock/jabrac.lock"

typedef struct mydeviceentry {
    struct mydeviceentry *next;
    unsigned short deviceID;
    char *deviceName;
    uint8_t notifiedAtLeastOnce;
    uint8_t lastNotifyCharging;
    uint8_t lastNotifyPercentage;
} mydeviceentry;


static void lockDeviceList();
static void unlockDeviceList();
static void freeDeviceEntry(mydeviceentry *entry);

static volatile int libraryInitialized;
static int verbose=0;
static int runAsDaemon=0;

static volatile int inMainLoop = 0;
static volatile int shutdown = 0;
static volatile int finalReturnCode=0;

static int pollingIntervalSeconds = 5*60;

static int notificationThreshold = 5;

static volatile int weCreatedLockFile = 0;

static volatile mydeviceentry *devices=0;

static pthread_mutex_t deviceListMutex;

static volatile int wakeUpFromSleep = 0;
static volatile int forcedWakeUpFromSleep = 0;

static pthread_cond_t sleep_condition;
static pthread_mutex_t sleep_mutex;

static int resolveLinkTarget(char *link, char *targetBuffer, size_t targetBufferSize)
{
    char exePath[PATH_MAX];

    if ( readlink(link,exePath,sizeof(exePath) ) > 0 ) {
        char *path = realpath(exePath,NULL);
        if ( path ) {
          size_t len = strlen(path);
          if ( (len+1) < targetBufferSize ) {
            strncpy(targetBuffer,path,targetBufferSize);
            free(path);
            return 1;
          }
          free(path);
        }
    }
    return 0;
}

static int isAlreadyRunning() {

   int isRunning = 0;
   FILE *in = fopen( PID_LOCK_FILE, "r");
   if ( in ) {
       char buffer[200];
       //        size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
       size_t bytesRead = fread(buffer,1,sizeof(buffer)-1,in);
       if ( bytesRead > 0 && bytesRead < 200 ) {
         buffer[bytesRead]=0;
         int pid = atoi(buffer);
         if ( pid > 0 ) {
           snprintf(buffer,sizeof(buffer),"/proc/%d/exe",pid);

           //        ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
           char exePath[PATH_MAX];
           if ( resolveLinkTarget( buffer,exePath,sizeof(exePath) ) )
           {
            isRunning = 1;
            char pathToSelf[PATH_MAX];
            if ( resolveLinkTarget( "/proc/self/exe",pathToSelf,sizeof(pathToSelf) ) ) {
              if ( strcmp(exePath,pathToSelf) != 0 ) {
                if ( runAsDaemon ) {
                  syslog(LOG_WARNING,"Found PID file %s but it points to running process %d (%s), expected %s",PID_LOCK_FILE,pid,exePath,pathToSelf);
                } else {
                  printf("WARNING: Found PID file %s but it points to running process %d (%s), expected %s",PID_LOCK_FILE,pid,exePath,pathToSelf);
                }
              }
            }
           }
         }
       }
       fclose(in);
   }
   return isRunning;
}

static int createLockFile()
{
    FILE *out = fopen( PID_LOCK_FILE, "w");
    if ( out == NULL ) {
      return 0;
    }
    weCreatedLockFile = 1;
    char buffer[200];
    snprintf(buffer,sizeof(buffer),"%d",getpid());

    size_t toWrite = strlen(buffer);
    size_t written = fwrite(&buffer,1,toWrite,out);
    if ( written != toWrite) {
        if ( runAsDaemon ) {
            syslog(LOG_WARNING,"Failed to write %ld bytes to PID file %s",toWrite, PID_LOCK_FILE);
        } else {
            printf("WARNING: Failed to write %ld bytes to PID file %s",toWrite, PID_LOCK_FILE);
        }
    }
    fclose(out);
    return toWrite == written;
}

static void deleteLockFile()
{
  if ( weCreatedLockFile ) {
    unlink( PID_LOCK_FILE );
  }
}

// return: whether the wakup was forced by a SIGHUP or not
static int sleepInterruptibly(int seconds)
{
    struct timespec time_to_wait;
    clock_gettime(CLOCK_MONOTONIC,&time_to_wait);
    time_to_wait.tv_sec += seconds;

    int forcedWakeup;

    pthread_mutex_lock(&sleep_mutex);
    while ( ! shutdown && ! wakeUpFromSleep ) {
      int rc = pthread_cond_timedwait(&sleep_condition, &sleep_mutex, &time_to_wait);
      if ( rc == ETIMEDOUT ) {
          break;
      }
      if ( rc == EINVAL ) {
        if ( runAsDaemon ) {
            syslog(LOG_WARNING,"timedwait() failed");
        } else {
            printf("ERROR: timedwait() failed");
        }
      }
    }
    syslog(LOG_DEBUG,"background thread woke up from pthread_cond_timedwait()");
    forcedWakeup = forcedWakeUpFromSleep;
    forcedWakeUpFromSleep = 0;
    wakeUpFromSleep = 0;
    pthread_mutex_unlock(&sleep_mutex);
    return forcedWakeup;
}

static void wakeup(int forced)
{
    syslog(LOG_DEBUG,"About to wake background thread");
    pthread_mutex_lock(&sleep_mutex);
    wakeUpFromSleep = 1;
    forcedWakeUpFromSleep = forced;
    pthread_cond_broadcast(&sleep_condition);
    pthread_mutex_unlock(&sleep_mutex);
    syslog(LOG_DEBUG,"Woke up background thread");
}

static void showNotification(char *msg)
{
    NotifyNotification* n = notify_notification_new ("jabrac", msg,0);
    notify_notification_set_timeout(n, 3000); // show for 3 seconds

    if ( ! notify_notification_show(n, 0) )
    {
        syslog(LOG_ERR,"Failed to show notification %s",msg);
    }
    if ( ! runAsDaemon ) {
      printf("%s\n",msg);
    }
}

static void cleanup(int callExit)
{
      lockDeviceList();

      mydeviceentry *current= (mydeviceentry*) devices;
      devices = 0;

      mydeviceentry *next;
      while(current) {
          next=current->next;
          freeDeviceEntry(current);
          current=next;
      }

      unlockDeviceList();

      deleteLockFile();

      if ( callExit ) {
        if ( inMainLoop ) {
          shutdown = 1;
          wakeup(0);
        } else {
          exit(1);
        }
      }
}

static void handleSignal(const char*msg, int callExit) {
   if ( verbose ) {
        if ( runAsDaemon ) {
          syslog(LOG_INFO,"%s",msg);
        } else {
          printf("%s\n",msg);
        }
    }
    cleanup(callExit);
}

static void sigTermHandler(int signal) {
  handleSignal("Received SIGTERM",1);
}

static void sigIntHandler(int signal) {
  handleSignal("Received SIGINT",1);
}

static void sigHupHandler(int signal) {
  syslog(LOG_DEBUG,"Received SIGHUP");
  wakeup(1);
}

static void sigQuitHandler(int signal) {
  handleSignal("Received SIGQUIT",0);
}

static void installSignalHandlers() {
  signal(SIGTERM, sigTermHandler);
  signal(SIGINT, sigIntHandler);
  signal(SIGQUIT, sigQuitHandler);
  signal(SIGHUP, sigHupHandler);
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

static void checkBatteryStatus(int force) {

    lockDeviceList();

    if ( devices )
    {
        // count devices
        int deviceCount = 0;
        mydeviceentry *current = (mydeviceentry*) devices;
        while ( current ) {
            current = current->next;
            deviceCount++;
        }

        // copy IDs so we can poll battery status without having to hold the lock
        unsigned short *ids = calloc(deviceCount,sizeof(unsigned short));
        current = (mydeviceentry*) devices;
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

                  current = (mydeviceentry*) devices;
                  while ( current && current->deviceID != ids[i] ) {
                      current = current->next;
                  }
                  if ( current )
                  {
                    uint8_t level = batteryStatus->levelInPercent;
                    uint8_t charging = batteryStatus->charging;

                    uint8_t notified = current->notifiedAtLeastOnce;
                    uint8_t chargingStateChanged = notified && current->lastNotifyCharging != charging;
                    uint8_t levelNotNotifiedYet = notified && current->lastNotifyPercentage != level;
                    uint8_t levelIsMultipleOfThreshold = (level % notificationThreshold ) == 0;
                    uint8_t deltaExceedsThreshold = notified && abs(current->lastNotifyPercentage - level) >= notificationThreshold;

                    if ( force || ! notified || chargingStateChanged || ( levelNotNotifiedYet && ( levelIsMultipleOfThreshold || deltaExceedsThreshold ) ) )
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

                        if ( ! force ) {
                            current->notifiedAtLeastOnce=1;
                            current->lastNotifyPercentage=level;
                            current->lastNotifyCharging=charging ? 1:0;
                        }
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

static void freeDeviceEntry(mydeviceentry *entry) {
    free(entry->deviceName);
    free(entry);
}

static void delDevice(unsigned short deviceID)
{
    syslog(LOG_INFO,"DETACHED: device with ID %04x", deviceID);

    lockDeviceList();

    mydeviceentry *previous=0;
    mydeviceentry *current=(mydeviceentry*) devices;
    while ( current )
    {
      if ( current->deviceID == deviceID )
      {
          if ( previous == 0 ) {
            devices = current->next;
          } else {
            previous->next = current->next;
          }
          freeDeviceEntry(current);
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
      newEntry->next = (mydeviceentry*) devices;
      devices = newEntry;
    } else {
      devices = newEntry;
    }

    unlockDeviceList();

    wakeup(0);
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

  if ( argc > 0 )
  {

    for ( int i = 0 ; i < argc ; i++) {
      if ( strcmp("-h", args[i]) == 0 || strcmp("--help",args[i]) == 0 ) {
        printf("Usage: [-h|--help] [-d|--daemon] [-v|--verbose] [--notify-step <battery level percentage delta>] [--polling-interval <seconds>]\n");
        return 1;
      } else if ( strcmp("-d", args[i]) == 0 || strcmp("--daemon",args[i]) == 0 ) {
        runAsDaemon=1;
      } else if ( strcmp("-v", args[i]) == 0 || strcmp("--verbose",args[i]) == 0 ) {
        verbose=1;
      } else if ( strcmp("--polling-interval", args[i]) == 0 ) {
        if ( (i+1) < argc ) {
            pollingIntervalSeconds = atoi(args[i+1]);
            if ( pollingIntervalSeconds < 1 ) {
              printf("ERROR: %d is an invalid argument for--polling, must be > 0\n", pollingIntervalSeconds);
              return 1;
            }
            i++;
        } else {
          printf("ERROR: --notify-step requires an argument\n");
          return 1;
        }
      } else if ( strcmp("--notify-step", args[i]) == 0 ) {
        if ( (i+1) < argc ) {
            notificationThreshold = atoi(args[i+1]);
            if ( notificationThreshold < 1 || notificationThreshold > 100 ) {
              printf("ERROR: %d is an invalid argument for--notify-step, must be > 0 and <= 100\n", notificationThreshold);
              return 1;
            }
            i++;
        } else {
          printf("ERROR: --notify-step requires an argument\n");
          return 1;
        }
      }
    }
  }

  if ( isAlreadyRunning() )
  {
    printf("ERROR: Another instance is already running, terminate that one first.\n");
    return 1;
  }

  if ( verbose ) {
    printf("Will notify about battery level changes every %d percent.\n",notificationThreshold);
    printf("Will poll battery status every %d seconds.\n",pollingIntervalSeconds);
  }

  if ( runAsDaemon ) {
    if ( verbose ) {
      printf("Running as daemon\n");
    }
    daemonize();
  }

  if ( ! createLockFile() )
  {
    if ( runAsDaemon ) {
      syslog(LOG_ERR, "Failed to create lock file %s\n", PID_LOCK_FILE);
    } else {
      printf("ERROR: Failed to create lock file %s\n", PID_LOCK_FILE);
    }
    return 1;
  }

  installSignalHandlers();

  pthread_mutex_init(&deviceListMutex,NULL);
  pthread_mutex_init(&sleep_mutex,NULL);

  // setup timed_wait() to use CLOCK_MONOTONIC
  // clock as the default CLOCK_REALTIME is prone to
  // jumps as it may be adjusted by the user/NTPD etc.
  pthread_condattr_t attr;

  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);

  pthread_cond_init(&sleep_condition, &attr);

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
  libraryInitialized = 1;

  showNotification("jabrac started");

  int forcedWakeup = 0;
  while( ! shutdown )
  {
    inMainLoop=1;
    checkBatteryStatus(forcedWakeup);
    forcedWakeup = sleepInterruptibly(60);
  }
  inMainLoop=0;
  if ( verbose ) {
    if ( runAsDaemon ) {
      syslog(LOG_INFO,"Program is terminating.\n");
    } else {
      printf("Program is terminating.\n");
    }
  }
  Jabra_Uninitialize();
  return finalReturnCode;
}
