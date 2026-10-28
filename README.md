# PendraBoot

Formatea y crea USB booteables en FLinux (32 bits).

- Detecta solo memorias USB extraíbles: el disco interno nunca aparece en la lista.
- Formatea en FAT32, NTFS, ext4 y btrfs.
- Crea USB booteables de tres formas: descargando la ISO oficial de FLinux (con wifi o sin wifi), usando una ISO local que ya tengas guardada, o creando una ISO de tu sistema instalado con todos sus programas.
- Todas las ISO se hacen híbridas, para que la USB arranque escrita en crudo con dd, Rufus o Etcher, y también por Ventoy.

## Instalación

Copiar `pendraboot.tcz`, `pendraboot.tcz.dep` y `pendraboot.tcz.md5.txt` (de la Release) a `tce/optional/`, añadir `pendraboot` a `onboot.lst` (o `tce-load -i pendraboot.tcz`) y ejecutar `pendraboot` desde el menú o el terminal. Se ejecuta como root (setuid).

## Autores

Idea original: Nicolás Longardi, creador de FLinux, que dio la idea en un directo en Kick y con ella inspiró este proyecto. PendraBoot fue desarrollado por Juan Carlos (Juanca) para la comunidad FLinux.

## Estado

v1.0 en prerelease: probado con prueba de humo en FLinux 17.1 sin wifi. Verificación completa pendiente (formatos, dd, descarga, opción 3, modo claro, arranque desde USB grabada).
