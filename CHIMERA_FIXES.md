# Chimera Hardened — Fixes actuales

Branch: `chimera-hardened`  
Actualizado: 2026-08-14

## Conexión, bookmarks y servidores

- Validación segura de direcciones de servidor, puertos, contraseñas y comandos `connect` generados.
- Soporte de resolución DNS IPv4/IPv6 con `AF_UNSPEC` y recorrido de resultados utilizables.
- Manejo de errores y timeouts en creación de sockets, `setsockopt()`, `sendto()` y `recvfrom()`.
- Validación de tamaño y terminación de respuestas de consulta de servidor.
- Limpieza segura de sockets y sanitización de caracteres de control en respuestas.
- Persistencia correcta de ediciones de bookmarks en `bookmark.txt`.
- Formateo IPv6-aware de conexiones y escape de `\\`/`\"` en contraseñas.
- Máquina de estados para conexiones iniciadas por bookmark que serializa solicitudes y evita entrar dos veces en la rutina de conexión de Halo.
- Solicitudes repetidas al mismo bookmark se ignoran mientras ya se está negociando esa conexión.
- Una solicitud a otro bookmark se encola y la solicitud más reciente pasa a ser el destino pendiente.
- El cambio de servidor usa la ruta normal de `disconnect` de Halo y espera estados conectados/desconectados válidos antes de avanzar.
- Una desconexión no resuelta no fuerza una segunda conexión.
- Recuperación de conexión fallida con timeout limitado a 15 segundos.
- Protección contra contraseñas nulas en el hook de pre-conexión.
- Consultas de bookmark/history publican resultados mediante estado atómico y mutex sin desbloqueos cruzados entre threads.
- Resultados de consulta se construyen localmente y se intercambian de forma sincronizada con el frame thread.
- Conservación de la semántica de presencia de `sappflags` usada por `chimera_spam_to_join`.

## Descargador de mapas

- Estado compartido del downloader protegido frente a acceso concurrente.
- Snapshots seguros de configuración para el worker thread.
- Validación y limpieza del URL escaping.
- Sustitución literal de placeholders de mirrors.
- Estado de retry, handles y recursos temporales limpiados en rutas de fallo.
- Detección de fallos de apertura, escritura y escrituras parciales.
- Validación de inicialización de cURL y opciones críticas.
- Manejo de errores HTTP, redirects, timeout de conexión y timeout total.
- Contención de excepciones dentro del worker thread.
- Comprobación de `joinable()` y manejo de fallo al crear threads.
- Callback de escritura usa `size * nmemb` con protección de overflow.
- Cancelación real del transfer cuando el estado de cancelación lo solicita.
- Cálculo de porcentaje protegido cuando el tamaño total todavía es cero.

## Mapas, compresión y memoria

- Validación de tamaño mínimo, headers, seeks, lecturas y rangos antes de procesar mapas.
- Lecturas desde mapas precargados en RAM limitadas por `decompressed_size`.
- Validación de tag data, scenario data, BSP table, BSP size y model data.
- Conversión de direcciones virtuales a tag data con comprobaciones de región/rango.
- Lecturas de resource maps y precache de assets endurecidas frente a punteros/rangos inválidos.
- Prevención de underflow durante preload de `ui.map`.
- Instalación/rename de mapas descargados con manejo no-exception mediante `std::error_code`.
- Una unión no continúa si un mapa requerido no pudo instalarse correctamente.
- Zstd valida input/output, tamaño declarado, overflow y finalización completa del stream.
- Streaming Zstd conserva input no consumido entre llamadas y detecta streams truncados/incompletos.
- Estado de Zstd y archivos comprimidos se liberan en todas las rutas de salida.
- CRC conserva el orden BSP -> model vertex data -> tag data usando un buffer de trabajo fijo de 64 KiB para BSP/model data.
- Tamaños corruptos se rechazan antes de reservar o leer regiones grandes.
- Nombres de mapa deben terminar en NUL dentro del campo de 32 bytes antes de normalizar/copiar.
- Offsets de seek se validan antes de reducirlos a `long`.
- `LoadedMap::buffer_size` conserva la capacidad total y `loaded_size` registra el contenido ocupado.
- `memory.max_tmp_files` se interpreta como cantidad de archivos, sin conversión accidental a MiB.
- Entradas de mapas requieren archivos regulares y las operaciones de filesystem usan `std::error_code` donde corresponde.
- Checks de índices de tags rechazan `index == tag_count` y accesos fuera de región.
- `TagBlock` valida address, count e index antes de devolver datos.

## Hooks y escritura de código x86

- Relocación de instrucciones relativas copiadas a trampolines para `CALL rel32`, `JMP rel32`, saltos condicionales near, `JMP` short y saltos condicionales short.
- Mapeo source-to-trampoline para preservar destinos internos dentro del bloque copiado.
- Expansión de branches cortos cuando la relocalización lo requiere.
- El decoder falla de forma segura ante instrucciones no soportadas en lugar de terminar el proceso.
- Detección de decoder sin progreso y limpieza de salida antes de cada intento.
- Validación de targets, funciones, punteros de salida, tamaños y estado de `Hook`.
- Trampolines usan asignación `nothrow` y limpian estado ante fallos.
- La relocalización se construye y valida antes de sobrescribir bytes del ejecutable.
- `VirtualProtect()` se valida y la protección original se restaura después de escribir.
- Se ejecuta `FlushInstructionCache()` después de modificar código ejecutable.
- Helpers genéricos de overwrite rechazan targets/datos nulos y longitudes cero.

## Configuración, filesystem e inicialización

- Creación de directorios de Chimera, temporales y mapas mediante variantes con `std::error_code` y `create_directories()`.
- Errores comunes de permisos/path no escapan inesperadamente desde la inicialización.
- `Ini::set_value()` reemplaza valores existentes sin duplicar la misma clave.
- Claves y valores nulos de INI se rechazan.
- `halo.client_port` y `halo.server_port` se validan en el rango válido antes de convertir a `uint16_t`.
- Un `halo.path` demasiado largo se ignora de forma segura sin finalizar Halo.
- Funciones opcionales no soportadas, como hash personalizado o múltiples instancias, fallan de forma local sin cerrar el proceso completo.
- Conversión UTF-8 -> wide -> ANSI usa tamaños coherentes y verifica resultados de las APIs de Windows.

## Comandos y parsing de entrada

- `execute_command()` rechaza comandos nulos/vacíos, inicializa `found_command` y contiene fallos de parsing/ejecución.
- `Command::call()` valida function pointers, `argc/argv` y conversiones antes de invocar comandos.
- Parsing numérico endurecido contra texto parcial, overflow, `NaN` e infinitos en controles, mouse, FPS/TPS, safe-zone, video mode y comandos de depuración.
- Canales de chat se aceptan únicamente cuando el valor completo es un entero dentro del rango soportado.
- Deadzones evitan divisiones por rangos inválidos.
- Contadores/porcentajes de debug evitan división por cero.
- `show_fps` y `throttle_fps` validan `QueryPerformanceFrequency()`/`QueryPerformanceCounter()` y estados de timer inválidos.
- IDs, índices y parámetros de `apply_damage`, `teleport`, `player_info`, `spectate` y comandos relacionados se validan antes de acceder a objetos/jugadores.
- Formateo de equipos en `player_list` usa `snprintf()` con tamaño de buffer.
- Hotkeys eliminan la entrada duplicada de F5 y endurecen el manejo de teclas/acciones inválidas.

## Consola, chat, salida y localización

- Buffers de errores de signatures limitan correctamente el offset producido por `snprintf()`.
- Helpers C de signatures rechazan contexto, nombres o punteros de salida nulos.
- Consola y custom chat validan tamaños, índices, posición, colores y valores no finitos antes de usarlos.
- Colores configurables de chat se limitan a rangos válidos.
- Rutas de salida de texto aceptan strings nulos de forma segura y separan mensajes literales del formateo variádico.
- `show_error_box()` normaliza header/text nulos.
- Iteración/carga de fuentes maneja fallos sin finalizar todo el cliente.
- Localización rechaza claves nulas y protege el índice de idioma antes de indexar la tabla.

## Render, cámara, HUD y video

- FOV, widescreen, Lua screen helpers y medición de texto evitan dividir por una altura de resolución igual a cero.
- First-person model mantiene un estado explícito de frustum válido y valida `global_window_parameters`/parámetros de lens flare antes de copiar frustums.
- Safe zones y escalado de HUD validan dimensiones y placement pointers antes de calcular offsets.
- Video mode valida resolución, refresh rate y valores configurados antes de aplicarlos.
- Fog/rasterizer valida device caps, globals, shaders, vertex buffers y opciones antes de acceder a ellos.
- Índices de vertex shaders fuera de rango fallan de forma segura y los shaders liberados se ponen en `nullptr`.
- Transparent geometry rechaza índices/buffers dinámicos inválidos sin dereferencias inseguras.
- HUD fonts valida tags, tag data y paths antes de resolver fuentes.

## Lua

- Escrituras Lua validan address, size, overflow y rango permitido del sandbox antes de copiar memoria.
- Lecturas numéricas preservan la semántica raw existente pero rechazan dirección cero.
- `read_string` preserva el comportamiento de devolver `nil` para dirección cero.
- Escrituras de strings validan longitud + terminador y overflow antes de copiar.
- `read_bit`/`write_bit` restringen el bit a `0..31` y `write_bit` acepta únicamente booleano o `0/1`.
- Registro de funciones I/O evita operar con un `lua_State` nulo.

## Optimización general actual

- Los dispatchers de `preframe`, `frame`, `pretick`, `tick`, `precamera`, `camera` y `D3D9 EndScene` reutilizan un snapshot interno de eventos en vez de crear/destruir una asignación de `std::vector` en cada dispatch estable.
- Se conserva el orden exacto por prioridad y FIFO dentro de cada prioridad.
- Se conserva la capacidad de agregar/eliminar eventos desde un callback porque cada dispatch sigue ejecutando un snapshot independiente de la lista fuente.
- La reentrada del mismo dispatcher usa el camino de copia original para no sobrescribir el snapshot que ya está activo.
- El fix del first-person model cachea la dirección de su signature durante el setup y evita repetir el lookup por nombre en cada pretick; el puntero dinámico de datos FP sigue leyéndose en cada tick.
