@echo off
rem Puente web para Koenigreich OS: dejalo abierto mientras usas el navegador de kng (host desde la VM: 10.0.2.2:8080)
python tools\kng-proxy.py 8080 || py tools\kng-proxy.py 8080
pause
