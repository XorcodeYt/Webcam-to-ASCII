# ASCII Webcam

**Real-time ASCII-art webcam viewer for Windows**  
Captures frames from your webcam, converts them into ASCII characters, and displays the result in a simple Win32 GUI. Built entirely with native Win32 API, Media Foundation, and WIC — no external dependencies.

---

## Features

- 🎥 Live capture from MJPEG-compatible webcam  
- 🧮 Real-time grayscale-to-ASCII conversion  
- 🪟 Native Win32 window and UI (Start/Stop button + ASCII preview)  
- 🔒 Thread-safe buffer handling and clean shutdown  

---

## Requirements

- Windows 10+  
- A webcam that supports MJPEG output  
- MSVC (Visual Studio) or compatible compiler  
- Windows SDK  

---

## Build Instructions

1. Clone the repo:  
   ```
   git clone https://github.com/yourusername/ascii-webcam.git
   ```

2. Open the `.sln` file with Visual Studio **or** compile manually:  
   ```
   cl /EHsc /std:c++17 ascii_webcam.cpp /link Mfplat.lib Mfreadwrite.lib Mf.lib Mfuuid.lib windowscodecs.lib Shlwapi.lib user32.lib gdi32.lib ole32.lib
   ```

3. Run `ascii_webcam.exe` and click **Start** to begin capturing.

---

## Notes

- ASCII resolution is fixed to 59×48 characters  
- Frame rate is ~30 FPS with adjustable delay in `ConvertLoop()`  
- Designed for educational and nostalgic purposes  
