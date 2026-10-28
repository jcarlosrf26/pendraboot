# PendraBoot

<img src="pendraboot.png" width="128" alt="Icono de PendraBoot">

Formatea y crea USB booteables en FLinux (32 bits).

## Cómo se hizo

Escrito en C++ con FLTK, compilado dentro de FLinux 17.1 de 32 bits y empaquetado como extensión `.tcz`. Se ejecuta como root (setuid) para poder formatear y grabar dispositivos. Las ISO se hacen híbridas antes de grabarlas, para que la USB arranque escrita en crudo con dd, Rufus o Etcher, y también por Ventoy.

## Funciones

- Detecta solo memorias USB extraíbles: el disco interno nunca aparece en la lista.
- Formatea en FAT32, NTFS, ext4 y btrfs, con etiqueta y confirmación antes de borrar.
- Crea USB booteables de tres formas:
  1. Descargando la ISO oficial de FLinux (con wifi o sin wifi).
  2. Usando una ISO local que ya tengas guardada (sirve cualquier ISO booteable, no solo FLinux).
  3. Creando una ISO de tu sistema instalado, con todos sus programas.
- La opción 3 detecta automáticamente el kernel y el core del sistema: particiones montadas (en `/boot` y en la raíz), particiones sin montar (montaje solo lectura) y dentro de archivos `.iso` por loop (caso Ventoy).
- La opción 3 también permite elegir a mano el kernel/core y la carpeta de extensiones, con dos cuadros y botón Examinar. Si los dos cuadros se dejan vacíos, se usa la detección automática.
- La USB destino queda excluida por completo de la búsqueda de la opción 3: no hace falta formatearla antes, y una versión vieja instalada en ella no puede colarse en la ISO.
- Modo oscuro (por defecto) y modo claro en gris claro estilo flfm.
- Botón de ayuda con pestañas: Qué es, Cómo se usa (pasos ordenados por lista) y Autores.
- Al instalar, deja su icono en el wbar.

## Imágenes

Modo oscuro:

![PendraBoot en modo oscuro](images/pendraboot-oscuro.jpg)

Modo claro:

![PendraBoot en modo claro](images/pendraboot-claro.jpg)

Ventana de ayuda:

![Ayuda de PendraBoot](images/pendraboot-ayuda.jpg)

Cabecera con los iconos centrados:

![Cabecera de PendraBoot](images/pendraboot-cabecera.jpg)

Icono en el wbar al instalar:

![Icono de PendraBoot en el wbar](images/pendraboot-wbar.jpg)

Ayuda «Cómo se usa» por lista, en modo oscuro y en modo claro:

![Ayuda por lista en modo oscuro](images/pendraboot-ayuda-lista-oscuro.jpg)

![Ayuda por lista en modo claro](images/pendraboot-ayuda-lista-claro.jpg)

Sección de la opción 3 con los cuadros manuales:

![Sección opción 3 con cuadros manuales](images/pendraboot-seccion3-1.3.jpg)

## Créditos

Idea original: Nicolás Longardi, creador de FLinux, que dio la idea en un directo en Kick y con ella inspiró este proyecto. PendraBoot fue desarrollado por Juan Carlos (Juanca) para la comunidad FLinux.

## Instalación

Copiar `pendraboot-1.3.tcz`, `pendraboot-1.3.tcz.dep` y `pendraboot-1.3.tcz.md5.txt` (de la Release) a `tce/optional/`, añadir `pendraboot-1.3` a `onboot.lst` (o `tce-load -i pendraboot-1.3.tcz`) y ejecutar `pendraboot`.

## Estado

Versión 1.3 (prerelease): probada en invitado FLinux 17.1 sin wifi (opción 3 con boot manual, con carpeta tce manual y regresión con los cuadros vacíos; la detección automática sigue intacta). Pendiente: prueba en hardware real (el fallo original era en la Dell con Ventoy), arranque desde USB grabada en máquina real, formatos, dd y descarga en la 1.3.

## Licencia

PendraBoot se distribuye bajo la Licencia Pública General de GNU, versión 3 (GPL-3.0). Ver el archivo `LICENSE`.
