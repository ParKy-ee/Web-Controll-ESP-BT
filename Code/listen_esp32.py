import ctypes, time, sys, io
from serial import win32

# Set stdout to UTF-8
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

print("Connecting to COM3...")
h = win32.CreateFile(r'\\.\COM3', win32.GENERIC_READ | win32.GENERIC_WRITE, 0, None, win32.OPEN_EXISTING, 0, 0)
if h == win32.INVALID_HANDLE_VALUE:
    print("Failed to open COM3:", ctypes.WinError())
    sys.exit(1)

print("COM3 Opened successfully! Listening for ESP32...")

timeouts = win32.COMMTIMEOUTS()
timeouts.ReadIntervalTimeout = 50
timeouts.ReadTotalTimeoutConstant = 200
timeouts.ReadTotalTimeoutMultiplier = 0
win32.SetCommTimeouts(h, ctypes.byref(timeouts))

buf = ctypes.create_string_buffer(1024)
read = win32.DWORD()
start_time = time.time()

try:
    while time.time() - start_time < 30: # ฟังต่อเนื่อง 30 วินาที
        if win32.ReadFile(h, buf, 1024, ctypes.byref(read), None) and read.value > 0:
            text = buf.raw[:read.value].decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()
            start_time = time.time() # ยืดเวลาถอยหลังถ้ามีข้อมูลส่งเข้ามา
        time.sleep(0.02)
finally:
    win32.CloseHandle(h)
    print("\nSession finished.")
