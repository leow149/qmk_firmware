#!/usr/bin/env python3
"""Send PC metrics (CPU / GPU load + temperature, RAM, network) to the AULA F75 Max LCD over raw HID.

The firmware shows them on its second screen page: press Fn + knob to switch pages. USB only.

Install:      pip install hidapi psutil              (required)
              pip install nvidia-ml-py               (NVIDIA GPU)
              pip install wmi                        (Windows: temperatures / AMD+Intel GPU through LibreHardwareMonitor)
Run:          python f75max_metrics.py               (Ctrl+C to stop; reconnects by itself when the keyboard is replugged)
Try:          python f75max_metrics.py --once        (print what would be sent)
              python f75max_metrics.py --demo        (synthetic values, to test the display)

Sensor sources (anything missing is shown as "--" on the keyboard):
  CPU temp   Linux: psutil (coretemp / k10temp / zenpower).  Windows: LibreHardwareMonitor or OpenHardwareMonitor running
             (its WMI provider), else the ACPI thermal zone.
  GPU        NVIDIA: nvidia-ml-py (or nvidia-smi).  AMD on Linux: amdgpu sysfs.  Windows AMD/Intel: LibreHardwareMonitor WMI.

Packet (32 bytes after the report id): [0]=0xC0 [1]=1 [2]=cpu% [3]=cpu C [4]=gpu% [5]=gpu C [6]=ram% [8..11]=down kB/s (LE)
[12..15]=up kB/s (LE). 0xFF / 0xFFFFFFFF = not available.
"""
import argparse, glob, math, os, platform, shutil, struct, subprocess, sys, time

import hid
import psutil

VID, PID = 0x0C45, 0x800A
USAGE_PAGE, USAGE = 0xFF60, 0x61
NA8, NA32 = 0xFF, 0xFFFFFFFF
IS_WIN = platform.system() == "Windows"


# ---------------------------------------------------------------- sensors ----
class Sensors:
    def __init__(self):
        self._net = psutil.net_io_counters(pernic=True)
        self._t = time.monotonic()
        psutil.cpu_percent(None)
        self._nvml = self._init_nvml()
        self._wmi = self._init_wmi()
        self._amd = self._find_amd() if not IS_WIN else None

    # --- helpers
    @staticmethod
    def _init_nvml():
        try:
            import pynvml
            pynvml.nvmlInit()
            if pynvml.nvmlDeviceGetCount() > 0:
                return pynvml
        except Exception:
            pass
        return None

    @staticmethod
    def _init_wmi():
        if not IS_WIN:
            return None
        try:
            import wmi
        except Exception:
            return None
        for ns in ("root\\LibreHardwareMonitor", "root\\OpenHardwareMonitor"):
            try:
                c = wmi.WMI(namespace=ns)
                c.Sensor()  # raises if the provider is not running
                return c
            except Exception:
                continue
        return None

    @staticmethod
    def _find_amd():
        for card in sorted(glob.glob("/sys/class/drm/card[0-9]*/device")):
            try:
                if open(card + "/vendor").read().strip() == "0x1002" and os.path.exists(card + "/gpu_busy_percent"):
                    temps = glob.glob(card + "/hwmon/hwmon*/temp1_input")
                    return card, (temps[0] if temps else None)
            except OSError:
                pass
        return None

    def _wmi_sensor(self, kind, name_part):
        try:
            vals = [s.Value for s in self._wmi.Sensor() if s.SensorType == kind and name_part.lower() in s.Name.lower()]
            vals = [v for v in vals if v is not None]
            return max(vals) if vals else None
        except Exception:
            return None

    # --- CPU
    def cpu(self):
        pct = int(round(psutil.cpu_percent(None)))
        temp = None
        if self._wmi:
            temp = self._wmi_sensor("Temperature", "CPU Package") or self._wmi_sensor("Temperature", "Core (Tctl")
            if temp is None:
                temp = self._wmi_sensor("Temperature", "CPU")
        elif not IS_WIN:
            try:
                t = psutil.sensors_temperatures()
                for key in ("coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"):
                    if key in t and t[key]:
                        entries = t[key]
                        pref = [e for e in entries if any(w in (e.label or "") for w in ("Package", "Tctl", "Tdie"))]
                        temp = (pref or entries)[0].current
                        break
            except Exception:
                pass
        if temp is None and IS_WIN:
            try:  # ACPI thermal zone, in tenths of Kelvin; often absent or inaccurate
                import wmi
                z = wmi.WMI(namespace="root\\wmi").MSAcpi_ThermalZoneTemperature()
                temp = max(x.CurrentTemperature for x in z) / 10.0 - 273.15
            except Exception:
                pass
        return pct, (int(round(temp)) if temp is not None else None)

    # --- GPU
    def gpu(self):
        if self._nvml:
            try:
                h = self._nvml.nvmlDeviceGetHandleByIndex(0)
                u = self._nvml.nvmlDeviceGetUtilizationRates(h).gpu
                t = self._nvml.nvmlDeviceGetTemperature(h, self._nvml.NVML_TEMPERATURE_GPU)
                return int(u), int(t)
            except Exception:
                pass
        if self._amd:
            card, temp_path = self._amd
            try:
                u = int(open(card + "/gpu_busy_percent").read())
                t = int(open(temp_path).read()) // 1000 if temp_path else None
                return u, t
            except OSError:
                pass
        if self._wmi:
            u = self._wmi_sensor("Load", "GPU Core")
            t = self._wmi_sensor("Temperature", "GPU Core")
            if u is not None or t is not None:
                return (int(round(u)) if u is not None else None), (int(round(t)) if t is not None else None)
        if shutil.which("nvidia-smi"):
            try:
                out = subprocess.check_output(["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu",
                                               "--format=csv,noheader,nounits"], text=True, timeout=2).splitlines()[0]
                u, t = [int(x) for x in out.split(",")]
                return u, t
            except Exception:
                pass
        return None, None

    # --- RAM / network
    @staticmethod
    def ram():
        return int(round(psutil.virtual_memory().percent))

    def net(self):
        now = time.monotonic()
        cur = psutil.net_io_counters(pernic=True)
        dt = max(now - self._t, 1e-3)
        dn = up = 0
        for nic, c in cur.items():
            if nic.startswith("lo") or nic not in self._net:
                continue
            p = self._net[nic]
            dn += max(0, c.bytes_recv - p.bytes_recv)
            up += max(0, c.bytes_sent - p.bytes_sent)
        self._net, self._t = cur, now
        return int(dn / dt / 1000), int(up / dt / 1000)


# ----------------------------------------------------------------- packet ----
def build(cpu, cpu_t, gpu, gpu_t, ram, dn, up):
    b8 = lambda v: NA8 if v is None else max(0, min(254, int(v)))
    b32 = lambda v: NA32 if v is None else max(0, min(NA32 - 1, int(v)))
    p = bytearray(32)
    p[0], p[1] = 0xC0, 1
    p[2], p[3], p[4], p[5], p[6] = b8(cpu), b8(cpu_t), b8(gpu), b8(gpu_t), b8(ram)
    struct.pack_into("<II", p, 8, b32(dn), b32(up))
    return bytes(p)


def find_device():
    for d in hid.enumerate(VID, PID):
        if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE:
            return d["path"]
    return None


def open_device():
    path = find_device()
    if path is None:
        return None
    if hasattr(hid, "Device"):          # `hid` / newer `hidapi` packages
        return hid.Device(path=path)
    dev = hid.device()                  # classic cython-hidapi
    dev.open_path(path)
    dev.set_nonblocking(True)
    return dev


# ------------------------------------------------------------------- main ----
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--once", action="store_true", help="print one sample and exit (no keyboard needed)")
    ap.add_argument("--demo", action="store_true", help="send synthetic values")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between updates (default 1)")
    args = ap.parse_args()

    sensors = None if args.demo else Sensors()
    time.sleep(0.5)
    dev, t0 = None, time.monotonic()
    while True:
        if args.demo:
            ph = (time.monotonic() - t0) / 4.0
            cpu = 50 + 45 * math.sin(ph); gpu = 50 + 45 * math.sin(ph + 2); ram = 55 + 30 * math.sin(ph / 3)
            sample = (cpu, 45 + cpu / 3, gpu, 40 + gpu / 2, ram, abs(math.sin(ph)) * 90000, abs(math.sin(ph * 2)) * 800)
        else:
            cpu, cpu_t = sensors.cpu(); gpu, gpu_t = sensors.gpu(); dn, up = sensors.net()
            sample = (cpu, cpu_t, gpu, gpu_t, sensors.ram(), dn, up)
        if args.once:
            names = ("cpu %", "cpu C", "gpu %", "gpu C", "ram %", "down kB/s", "up kB/s")
            print("  ".join("%s=%s" % (n, "n/a" if v is None else int(v)) for n, v in zip(names, sample)))
            print("keyboard:", "found" if find_device() else "not found")
            return
        try:
            if dev is None:
                dev = open_device()
                if dev:
                    print("keyboard connected")
            if dev:
                dev.write(b"\x00" + build(*sample))
        except (OSError, ValueError, hid.HIDException if hasattr(hid, "HIDException") else OSError):
            print("keyboard lost, waiting for it ...")
            try:
                dev and dev.close()
            except Exception:
                pass
            dev = None
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
