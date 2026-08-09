@echo off
chcp 65001 >nul
echo ====================== SVG批量转PNG工具 ======================
echo 说明：将本bat放入svg文件同目录，双击运行自动转换全部svg
echo ==============================================================
echo.

:: Inkscape程序路径，如安装位置不同请修改此处
set "INKSCAPE=E:\Program Files\Inkscape\bin\inkscape.exe"

:: 检测Inkscape是否存在
if not exist "%INKSCAPE%" (
    echo 错误：未找到Inkscape，请修改脚本内INKSCAPE路径！
    echo 默认安装路径：C:\Program Files\Inkscape\bin\inkscape.exe
    pause
    exit /b 1
)

:: 遍历当前文件夹所有.svg文件
for %%i in (*.svg) do (
    echo 正在转换：%%i
    :: -w 宽度 1000px，可自行修改；输出同文件名png
    "%INKSCAPE%" %%i --export-type=png --export-width=1000 --export-filename="%%~ni.png"
)

echo.
echo 全部转换完成！
pause