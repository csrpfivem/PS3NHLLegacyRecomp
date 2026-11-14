/* padprobe: print DualSense/DS4 HID report rate once per second (diagnostic for #324 BT).
 * Usage: padprobe [seconds]   (default 20). Read-only except the BT full-report feature read. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

int main(int argc, char** argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    HANDLE h = INVALID_HANDLE_VALUE; HIDP_CAPS caps; int ds5 = 0;
    for (DWORD k = 0; SetupDiEnumDeviceInterfaces(di, NULL, &g, k, &ifd); k++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE raw[2048]; } u;
        u.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, &u.d, sizeof u, NULL, NULL)) continue;
        WCHAR low[1024]; size_t i = 0;
        for (; u.d.DevicePath[i] && i < 1023; i++) low[i] = towlower(u.d.DevicePath[i]);
        low[i] = 0;
        if (!wcsstr(low, L"054c")) continue;
        HANDLE t = CreateFileW(u.d.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (t == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof at }; PHIDP_PREPARSED_DATA pp = NULL;
        if (HidD_GetAttributes(t, &at) && HidD_GetPreparsedData(t, &pp) &&
            HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5) {
            HidD_FreePreparsedData(pp);
            ds5 = at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2;
            printf("pad %04X:%04X in %u (%s)\n", at.VendorID, at.ProductID, caps.InputReportByteLength,
                   caps.InputReportByteLength > 64 ? "Bluetooth" : "USB");
            h = t; break;
        }
        if (pp) HidD_FreePreparsedData(pp);
        CloseHandle(t);
    }
    SetupDiDestroyDeviceInfoList(di);
    if (h == INVALID_HANDLE_VALUE) { printf("no DualSense/DS4 found\n"); return 1; }
    if (caps.InputReportByteLength > 64) {
        BYTE f[1024] = { (BYTE)(ds5 ? 0x05 : 0x02) };
        HidD_GetFeature(h, f, caps.FeatureReportByteLength < sizeof f ? caps.FeatureReportByteLength : sizeof f);
    }
    OVERLAPPED ov = { 0 }; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BYTE buf[1024];
    ULONGLONG start = GetTickCount64(), sec_t = start, last = 0; unsigned cnt = 0, gap = 0;
    printf("move the sticks; %d s...\n", secs);
    while (GetTickCount64() - start < (ULONGLONG)secs * 1000) {
        DWORD n = 0; ResetEvent(ov.hEvent);
        if (!ReadFile(h, buf, caps.InputReportByteLength, &n, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &n, FALSE)) {
            ULONGLONG t = GetTickCount64();
            if (last && t - last > gap) gap = (unsigned)(t - last);
            last = t; cnt++;
        } else { CancelIoEx(h, &ov); GetOverlappedResult(h, &ov, &n, TRUE); }
        ULONGLONG now = GetTickCount64();
        if (now - sec_t >= 1000) {
            printf("%3llu s: %4u reports/s, max gap %u ms\n", (now - start) / 1000, cnt, gap);
            fflush(stdout);
            cnt = 0; gap = 0; sec_t = now;
        }
    }
    CloseHandle(h);
    return 0;
}
