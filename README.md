# VoidVReaker

Mod nativo de VR 6DoF para **VOID/BREAKER** (Steam, Unreal Engine 5.7, DirectX 12) con controles de movimiento: el arma va en tu mano derecha, apuntas y disparas con el mando, andas hacia donde miras, el HUD va en tu muñeca izquierda y lo esencial (vida, subtítulos, avisos) delante de ti; los paneles de los objetos flotan como hologramas en el mundo y los menús se ven en una pantalla virtual. Hecho para Meta Quest 3 con Virtual Desktop; usa OpenXR, así que debería valer cualquier visor con runtime OpenXR.

No usa UEVR: es una DLL propia (`version.dll`) que usa el propio renderizador estéreo del motor.

> ⚠️ **Fase experimental.** Se puede jugar, pero aún puede tener fallos, cierres o detalles sin pulir.

## Instalar

1. Descarga `VoidVReaker.zip` de la [última release](https://github.com/oz-x/VoidVReaker/releases).
2. Descomprímelo y haz doble clic en `instalar.bat`: encuentra el juego (Steam o Epic), pide confirmación y copia el mod.
3. Conecta el visor y abre VOID/BREAKER desde Steam.

Controles, ajustes y desinstalación: [`dist/LEEME.md`](dist/LEEME.md).

## Compilar

Requisitos: Visual Studio 2022 (C++ x64, MASM), CMake ≥ 3.24, Git.

```bash
mkdir third_party && cd third_party
git clone --depth 1 https://github.com/TsudaKageyu/minhook.git
git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git
cmake -S OpenXR-SDK -B build-openxr -G "Visual Studio 17 2022" -A x64 -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF
cmake --build build-openxr --config Release --target openxr_loader
cd ..
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
bash tools/package.sh   # build/VoidVReaker.zip
```

## Cómo funciona (resumen)

- Proxy de `version.dll`; la línea de comandos añade `-emulatestereo` y el mod sustituye el dispositivo estéreo falso del motor por las poses y proyecciones del visor.
- Cada ojo se dibuja como su propio fotograma de la vista del juego (vista estéreo 0, con su propio historial temporal).
- La interfaz se separa de los ojos y se recorta en capas OpenXR (muñeca, paneles fijos, pantalla virtual de menús).
- Los mandos se presentan al juego como un mando de Xbox (XInput); la rotación de control sigue al mando derecho.

## Licencia

MIT. Componentes de terceros: MinHook (BSD-2) y el cargador OpenXR (Apache 2.0); avisos en `TERCEROS.txt` dentro del ZIP.
