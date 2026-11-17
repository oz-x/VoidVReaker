# VoidVReaker: VOID/BREAKER en realidad virtual

Mod de realidad virtual nativa para **VOID/BREAKER** (Steam). No usa UEVR ni otros inyectores: es una DLL propia que convierte el juego en VR completo de 6 grados de libertad.

> ⚠️ **Fase experimental.** El mod funciona y se puede jugar, pero aún puede tener fallos, cierres o detalles sin pulir (HUD, menús, cambios de nivel). Si algo va mal, ponlo `Enabled=0` en `voidvr.ini` o desinstálalo.

## Qué incluye

- **Estéreo 6DoF a escala real**, a la frecuencia del visor (OpenXR).
- **Arma en la mano derecha**: apuntas y disparas con el mando, con una mira VR en el punto donde van las balas.
- **Andar hacia donde miras** con el stick izquierdo. Giro suave o por pasos con el stick derecho. Vibración en los mandos.
- **HUD en la muñeca izquierda** (recursos y munición, como un reloj). La vida, los subtítulos y los avisos van fijos abajo en tu vista. Los iconos del juego (objetivo, cofres…) aparecen sobre los objetos en el mundo.
- **Menús en una pantalla virtual** delante de ti, manejables con los mandos.
- El juego no se pausa al perder el foco la ventana del PC.

## Requisitos

- VOID/BREAKER de Steam (Windows, DirectX 12).
- Un visor con runtime OpenXR: Quest con Virtual Desktop (recomendado) o Quest Link, o SteamVR.
- Una GPU potente: el juego se dibuja dos veces por fotograma. Con una RTX 4090 va a unos 72 fps con `ResolutionScale=0.75`.

## Instalación

1. Descomprime el ZIP en cualquier carpeta.
2. Ejecuta `instalar.bat` (doble clic). Busca el juego en Steam/Epic, te pide confirmación y copia `version.dll`, `openxr_loader.dll` y `voidvr.ini` en `VOIDBREAKER\VOIDBREAKER\Binaries\Win64`.
3. Conecta el visor (Virtual Desktop / Link / SteamVR) **antes** de lanzar el juego.
4. Lanza VOID/BREAKER desde Steam como siempre.

Si no hay visor conectado al arrancar, el juego funciona en plano normal.

## Controles (mandos Touch)

| Mando | Acción |
|---|---|
| Stick izquierdo | Andar (hacia donde miras) |
| Stick derecho | Girar |
| Gatillo derecho | Disparar |
| Gatillo izquierdo | Gatillo izquierdo del mando (habilidad del juego) |
| Grip derecho / izquierdo | RB / LB |
| A / B | A / B |
| X / Y | X / Y |
| Pulsar sticks | L3 / R3 |
| Botón ☰ (izquierdo) | Pausa / menú |
| F6 o botón Meta mantenido | Recentrar la vista |

Los botones equivalen a los de un mando de Xbox en el juego, así que los menús y las acciones siguen las indicaciones de mando que muestra el propio juego.

## Ajustes

Todo está en `voidvr.ini`, junto al ejecutable del juego. Cada opción lleva su explicación. Las más útiles:

- `ResolutionScale`: menos = más fps.
- `SnapTurn=30`: giro por pasos si te mareas.
- `GunOffsetForward/Right/Up`, `GunYaw`: colocación del arma en la mano.
- `Enabled=0`: desactiva el mod sin desinstalarlo.

## Desinstalar

Borra `version.dll`, `openxr_loader.dll` y `voidvr.ini` de `VOIDBREAKER\VOIDBREAKER\Binaries\Win64`. Si el instalador guardó un `version.dll.bak-…` de otro mod, renómbralo de vuelta a `version.dll`.

## Notas

- Es un juego para un jugador sin anti-trampas; el mod no toca nada en línea.
- Tus partidas guardadas no se modifican.
