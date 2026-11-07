@echo off
REM ============================================================
REM  ESP32 浇水器 - 远程仪表盘 静态服务 + cloudflared 隧道
REM  在「常开的桌面机」上双击运行（需本机已装 cloudflared + python）
REM  启动后控制台会打印一个 https://xxxx.trycloudflare.com 地址
REM  手机用浏览器打开该地址即可，可「添加到主屏幕」当 App
REM ============================================================
cd /d "%~dp0"

REM 1) 静态服务：把 docs/remote 这个自包含单文件页面挂到本地 8088
echo [1/2] 启动静态服务 docs/remote :8088 ...
start "aw-static" python -m http.server 8088 --directory docs/remote
timeout /t 2 >nul

REM 2) 隧道：临时隧道（URL 每次重启会变，无需登录）
echo [2/2] 启动 cloudflared 临时隧道（trycloudflare）...
echo 注：想要固定不变域名，先在本机执行一次 cloudflared login，
echo     再用：cloudflared tunnel run --url http://localhost:8088 awdash
echo ---------------------------------------------------------
cloudflared tunnel --url http://localhost:8088

REM 退出后顺手关掉静态服务窗口
taskkill /im python.exe /fi "windowtitle eq aw-static*" >nul 2>&1
