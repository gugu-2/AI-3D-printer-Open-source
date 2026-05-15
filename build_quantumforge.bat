@echo off
echo ========================================================
echo QuantumForge Build Script (Dependencies + Main Project)
echo ========================================================

echo.
echo [1/3] Building Dependencies... (This will take 30-60 minutes)
if not exist "deps\build" mkdir "deps\build"
cd deps\build
cmake ..
cmake --build . --config Release --parallel
if %errorlevel% neq 0 (
    echo [ERROR] Failed to build dependencies!
    pause
    exit /b %errorlevel%
)
cd ..\..

echo.
echo [2/3] Configuring QuantumForge...
if not exist "build" mkdir "build"
cd build
cmake .. -B . -DCMAKE_PREFIX_PATH="%CD%\..\deps\build\destdir\usr\local" -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed!
    pause
    exit /b %errorlevel%
)

echo.
echo [3/3] Compiling Tests...
cmake --build . --target tests --config Release --parallel
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile tests!
    pause
    exit /b %errorlevel%
)

echo.
echo [4/4] Running Catch2 Test Suite...
ctest --test-dir . --output-on-failure -C Release

echo.
echo ========================================================
echo Build complete. Press any key to exit...
pause
