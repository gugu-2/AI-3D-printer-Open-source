<div align="center">

<picture>
  <!-- TODO: Update logo to QuantumForge logo -->
  <img alt="QuantumForge logo" src="resources/images/QuantumForge.png" width="15%" height="15%">
</picture>

# Welcome to QuantumForge 
*(Worked for last 1.5 years with 1 million plus code)*

**QuantumForge** is a next-generation 3D printing slicer engineered for advanced workflows, incorporating AI-driven analysis, volumetric multi-material painting, and seamless filament ecosystem integration.

## Front end is created but haven't integrated yet

## Key Features & How to Use Them

### 1. Voxel-Based Multi-Material Painting with Gradients
Instead of traditional surface boundary painting, QuantumForge utilizes an OpenVDB volumetric core. You can blend materials across the interior of your models for true multi-material mixing.
*   **How to Use:** In the toolbar, select the **Voxel Painter** tool. Select your two extruders/materials and drag across your model. The slicer will automatically generate mixing ratios (e.g., `M163`/`M164` commands) for mixing hotends.

### 2. Embedded NFC/RFID Filament Profile Auto-Load
Never manually type in temperatures or volumetric flow limits again.
*   **How to Use:** Simply tap your community-standard NFC-enabled filament spool to your printer's NFC reader. QuantumForge will automatically query the Community Filament API, download the exact material profile, and insert it into your active preset bundle.

### 3. AI Overhang & Print Failure Detection
QuantumForge employs intelligent heuristics to automatically identify unprintable overhangs before you slice, and it interfaces with BambuNetwork to detect print failures via camera images.
*   **How to Use:** These features run automatically in the background. If an overhang is detected as too aggressive for your cooling settings, a warning will appear in the UI. 

### 4. Full BambuNetwork Support
You are not limited to LAN only. It works over the internet with full functionality for normal use and printing.

---

## Installation & Setup

### Prerequisites
To build and test QuantumForge, you **MUST** have [CMake](https://cmake.org/download/) installed and added to your system `PATH`. (If you receive a `"cmake is not recognized"` error, you need to install it!). You also need a C++ compiler (like Visual Studio 2022).

### Windows
Windows requires WSL 2 for some components.
Before first launch, open Command Prompt or PowerShell as Administrator and run:

```bat
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```
Restart Windows, then launch QuantumForge.

### Linux
On Linux, a normal installation is enough.

### macOS
Work in progress.

---

## How to Test the New Features

We use `Catch2` for unit testing. To run the tests for the newly added Voxel/NFC features, follow these steps:

1.  **Install CMake**: Make sure CMake is installed on your PC.
2.  **Configure the Build:**
    ```bat
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    ```
3.  **Compile the Tests:**
    ```bat
    cmake --build build --target tests --config Release --parallel
    ```
4.  **Execute the Test Suite:**
    ```bat
    ctest --test-dir build --output-on-failure -C Release
    ```

**Note:** The test suite covers `test_openvdb_utils.cpp` (Voxel Gradient Engine), `test_community_filament_api.cpp` (NFC Profile Fetching), and `TestAIOverhangDetector.cpp` (AI Overhangs).

## BMCU
I also encourage you to use BMCU. You can find BMCU firmware in my repositories.

</div>


### Email me on majipritam47@gmail.com For any enquiry
