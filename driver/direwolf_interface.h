#pragma once

// Shared by the driver, the application, and the installer.
#define DIREWOLF_USB_INSTANCE L"USB\\VID_045E&PID_028E\\Flydigi_Direwolf_4"
#define DIREWOLF_USB_HARDWARE_ID L"USB\\VID_045E&PID_028E&REV_0104"
#define DIREWOLF_INTERFACE_GUID \
    {0x3f096fb6, 0xc38c, 0x443b, {0xae, 0x61, 0x99, 0x4e, 0xb1, 0x4c, 0x05, 0x6e}}
#define DIREWOLF_QUERY_BATTERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define DIREWOLF_BATTERY_VERSION 1u

typedef struct _DIREWOLF_BATTERY {
    ULONG Version;
    UCHAR Level;
    UCHAR Charging;
    USHORT Reserved;
} DIREWOLF_BATTERY;
