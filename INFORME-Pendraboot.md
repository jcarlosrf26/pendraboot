# INFORME — PendraBoot 1.0 (.tcz para prueba del usuario)

Fecha: 2026-10-28. Entrega por orden del usuario: ".tcz YA, prueba de humo primero;
verificación completa y foto real DESPUÉS". Nada publicado en ningún repo.

## Entrega

- `pendraboot.tcz`: **139 264 B, MD5 `2985eba79c8ac6c6ce9ab1ec411fe869`**
- `pendraboot.tcz.dep` (148 B): fltk-1.3.tcz, dosfstools.tcz, ntfs-3g.tcz,
  e2fsprogs.tcz, btrfs-progs.tcz, curl.tcz, ca-certificates.tcz, gcc_libs.tcz,
  syslinux.tcz, perl5.tcz, cdrtools.tcz
- `pendraboot.tcz.md5.txt` (49 B, MD5 idéntico al verificado)
- Contenido verificado con unsquashfs -ll: `usr/local/bin/pendraboot`
  **-rwsr-xr-x root/root (setuid 4755)**, `usr/local/share/pixmaps/pendraboot.png`
  (42 416 B, RGBA transparente), `pendraboot-sun.png` (286 B),
  `pendraboot-moon.png` (179 B), `usr/local/share/applications/pendraboot.desktop`.
- Binario: ELF32 Intel 80386, dinámico con libc (libfltk.so.1.3, libX11, libXext,
  libXcursor, libstdc++, libgcc_s, libc), 231 180 B, compilado en el invitado
  FLinux 17.1 sin wifi con `g++ rc=0` (registro del invitado). Símbolo GLIBC
  máximo 2.38 (compatible con glibc 2.42 de la 17.2).

## Prueba de humo superada (invitado FLinux 17.1 SIN wifi, QEMU, 2026-10-28)

Base: ISO oficial local 47 828 992 B, MD5 `c2a014dfc32925205444cf41bb1a1acb`.
Capturas en `~/workspace/flusb/shots/`:

1. **El programa abre** (`g171d-450s.jpg`): ventana PendraBoot, tema oscuro por
   defecto, título dorado + icono pendrive pegado a su lado, botón «i», botón
   sol dibujado, botón Actualizar, relieve marcado (degradados + biseles de
   alto contraste en los 5 paneles y 6 botones), pie «Pendraboot · FLinux 32
   bits · Se ejecuta como root».
2. **Lista la USB** (`g171d-450s.jpg`): `sdb — QEMU HARDDISK — 1.1 GB — sin
   formato`, seleccionada: «/dev/sdb — QEMU HARDDISK 1.1 GB (sin formato)».
   El disco interno (sda) NO aparece. Solo extraíbles.
3. **Ventana de ayuda** (`g171d-ayuda.jpg`): pulsando «i» (foco verificado con
   recorte: marco punteado sobre el botón «i» tras Shift+Tab×3) se abre
   «Ayuda de PendraBoot» con logo 96×96, «PendraBoot», «versión 1.0», lema,
   pestañas Qué es / Cómo se usa / Autores (Qué es por defecto) y el texto
   exacto ordenado, más botón Cerrar. Tema oscuro.
4. **Sin cuelgues**: la alerta «Elige primero una ISO local (Examinar...)»
   (disparada al probar Alt+C, ver detalle) se cerró con Enter
   (`g171d-alerta.jpg`); la ayuda quedó abierta y el programa siguió vivo.
   Invitado detenido limpiamente por monitor `quit`; binario extraído del
   disco diag sin corrupción.

## Detalle observado (NO bloquea la entrega; queda para el pulido posterior)

Alt+C con la ventana de ayuda abierta NO cerró la ayuda: activó «Crear
booteable» de la ventana principal (su atajo &C), porque la ayuda no tomó el
foco del teclado al abrirse. Con ratón, el botón Cerrar funciona (botón FLTK
normal). Queda como detalle de foco de teclado a pulir después de la entrega,
junto a la verificación completa.

## SOLO con prueba de humo / FALTA por verificar (después de entregar el .tcz)

- Los 4 formatos en ESTA compilación (FAT32 sí se probó en UI plana y en UI 3D
  inicial, no en esta compilación XPM; NTFS, ext4 y btrfs solo están
  implementados y revisados, sin ejecutar por GUI).
- dd de ISO local y de ISO oficial por GUI (no ejecutados en esta compilación).
- Descarga oficial (URL embebida: nivel raíz de SourceForge del proyecto
  flinux-distro; verificación por fetch: plausible, NO concluyente, y por
  instrucción posterior NO se reintentó. La app valida curl -f + tamaño > 1 MB
  antes de cualquier dd, así que una URL errónea falla sin escribir).
- Opción 3 «Crear ISO de mi sistema instalado» (implementada, sin ejecutar).
- Modo claro con luna (botón existe y alterna por código; no capturado en
  claro en esta compilación), relieve marcado en claro, navegación Tab con las
  ~200 cajas nuevas, y el segundo invitado arrancando desde la USB grabada
  (prueba decisiva de la opción 3 y del MBR híbrido).
- Hardware real: USB física, descarga completa, arranque en la Dell.
- isohybrid en el invitado: NO se pudo comprobar en esta pasada porque el
  pool del invitado (repo local) no trae syslinux; el .dep SÍ incluye
  syslinux.tcz + perl5.tcz, y en el host isohybrid 6.04 ya se verificó
  funcional sobre copias de la ISO. En la máquina del usuario, con las
  dependencias instaladas, isohybrid estará presente.
- Nota: el .tcz lleva también pendraboot-sun.png y pendraboot-moon.png aunque
  el programa ya usa los iconos XPM embebidos (se dejaron para no tocar más
  scripts).

## Cómo instalarlo (para el usuario)

Copiar `pendraboot.tcz` a `tce/optional/`, añadir `pendraboot.tcz` a
`onboot.lst` (o cargarlo con `tce-load -i pendraboot.tcz`), y ejecutar
`pendraboot` desde el menú (System/Utility) o desde terminal.
