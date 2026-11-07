@echo off
rem SPDX-License-Identifier: LGPL-2.1-or-later
rem Copyright (c) 2026 Joaquin Villodre
rem
rem Runs build.ps1 without changing the PowerShell execution policy
rem (scripts extracted from a downloaded ZIP are blocked by default).
rem Usage: build.cmd [-Install] [-Setup] [-Clean] [-Config Debug] ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
