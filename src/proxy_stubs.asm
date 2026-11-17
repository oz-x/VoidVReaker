; Export stubs of the version.dll proxy: jump to the system version.dll (pointers in proxy.cpp).

FORWARD MACRO name
    EXTERN g_real_&name:QWORD
proxy_&name PROC
    jmp QWORD PTR [g_real_&name]
proxy_&name ENDP
ENDM

.code
FORWARD GetFileVersionInfoA
FORWARD GetFileVersionInfoByHandle
FORWARD GetFileVersionInfoExA
FORWARD GetFileVersionInfoExW
FORWARD GetFileVersionInfoSizeA
FORWARD GetFileVersionInfoSizeExA
FORWARD GetFileVersionInfoSizeExW
FORWARD GetFileVersionInfoSizeW
FORWARD GetFileVersionInfoW
FORWARD VerFindFileA
FORWARD VerFindFileW
FORWARD VerInstallFileA
FORWARD VerInstallFileW
FORWARD VerLanguageNameA
FORWARD VerLanguageNameW
FORWARD VerQueryValueA
FORWARD VerQueryValueW
END
