# Protocolo de Comunicacion Bluetooth (Car-ESP32 <-> App Android)

Este documento sirve como la unica "fuente de verdad" para la comunicacion entre el dispositivo ESP32 y la aplicacion Android. **Cualquier cambio en la entrada/salida de datos en `main.cpp` debe reflejarse aqui de inmediato.**

## Configuracion Base
- **Nombre del dispositivo Bluetooth:** `Car-ESP32`
- **Timeouts del buffer (ESP32):** `120ms`
- **Longitud maxima del buffer (ESP32):** `260` caracteres
- **Terminador de linea (Comandos Estandar):** `\n`

## Hardware
- **Controlador de motores:** L298N (Puente H)
- **Pines motor:** IN1=GPIO16, IN2=GPIO17, IN3=GPIO18, IN4=GPIO19
- **LED (Ojos):** GPIO 5
- **LCD I2C:** 20x4, SDA=GPIO21, SCL=GPIO22
- **Botones fisicos:** Hora=GPIO25, Clima=GPIO26, Nombre=GPIO27

---

## 1. Comandos de Motor y Movimiento (App -> ESP32)

### Via Rapida (Fast-Path)
Estos comandos se ejecutan instantaneamente al recibir **un solo caracter**, ignorando el timeout del buffer. Solo se incluyen caracteres que NO son prefijo de ningun comando largo.

| Char | Accion                  |
|------|-------------------------|
| `F`  | Mover hacia Adelante    |
| `B`  | Mover hacia Atras       |
| `L`  | Girar a la Izquierda    |
| `R`  | Girar a la Derecha      |
| `X`  | Movimiento Explorar     |
| `V`  | Movimiento Vigilar      |
| `P`  | Movimiento Susto        |

> Case-insensitive (`f` = `F`).

### Comandos de Accion (Requieren `\n` o timeout)
| Comando | Accion |
|---------|--------|
| `S`     | Detener motores |
| `G`     | Giro preprogramado ~90 a la Izquierda (400ms) |
| `H`     | Giro preprogramado ~90 a la Derecha (400ms) |
| `D`     | Movimiento Bailar (no fast-path: colisiona con DRIVING) |

> `S`, `G`, `H`, `D` no entran en fast-path porque son prefijo de `STANDBY`, `GREETING:`, `HORA:`, `DRIVING` respectivamente.

---

## 2. Movimientos Especiales / Novedosos (App -> ESP32)

Secuencias preprogramadas ejecutadas de forma **no bloqueante** mediante maquina de estados. El robot puede recibir y procesar otros comandos mientras ejecuta la secuencia. Cualquier comando de movimiento basico (`F`, `B`, `L`, `R`, `S`, `G`, `H`) **cancela automaticamente** el movimiento especial en curso.

| Comando | Nombre    | Descripcion |
|---------|-----------|-------------|
| `X`     | Explorar  | Secuencia aleatoria de 4-6 segmentos avance+giro. Cada ejecucion genera una ruta diferente. |
| `D`     | Bailar    | Coreografia de 12 pasos: avances cortos alternados con giros variados (izq/der/largos). |
| `V`     | Vigilar   | Escaneo 360: 4 giros de ~90 con pausas de 800ms de "observacion" entre cada uno. |
| `P`     | Susto     | Ojos ON -> avance rapido 900ms -> frenazo brusco -> retroceso sutil -> pausa dramatica -> Ojos OFF. |

### Respuestas del ESP32 durante movimientos especiales:
- Al iniciar: `MOVE:EXPLORE\r\n`, `MOVE:DANCE\r\n`, `MOVE:SCAN\r\n`, o `MOVE:PRANK\r\n`
- Al finalizar (normal o cancelado): `MOVE:DONE\r\n`

### LCD durante movimientos especiales:
- Linea 0: `>>> ESPECIAL <<<`
- Linea 2: Nombre del movimiento (`Explorando...`, `Bailando!`, `Vigilando...`, `SUSTO!`)
- Al finalizar: se restaura la pantalla del modo actual.

---

## 3. Telemetria y Pantalla LCD (App -> ESP32)

### Control de Modos

| Comando    | Modo                   | LCD                              |
|------------|------------------------|----------------------------------|
| `STANDBY`  | Inicio pasivo          | "Paga 0.50 ctvs / por un saludo"|
| `DRIVING`  | Control manual         | "Modo Control / Listo..."        |
| `MODE1`    | Mensajes desplazables  | Texto scrolling en 4 lineas      |
| `MODE2`    | Telemetria             | "Presiona un boton" / datos      |

### Actualizacion de Datos Telemetria (Modo 2)
- `SET_HORA:[valor]` o `HORA:[valor]`
- `SET_TIEMPO:[valor]` o `TIEMPO:[valor]`
- `SET_NOMBRE:[valor]` o `NOMBRE:[valor]`

### Mensajes Desplazables (Modo 1)
- `SALUDO:[texto]`, `GREETING:[texto]`, `TEXT:[texto]`, `MSG:[texto]`

### Control LED (Ojos)
- `ON` -> Respuesta: `OK:ON\r\n`
- `OFF` -> Respuesta: `OK:OFF\r\n`

### Limpieza
- `CLEAR`: Limpia el mensaje desplazable actual.

---

## 4. Respuestas del Dispositivo (ESP32 -> App)

Textos enviados a traves de `SerialBT.println()`, que incluyen automaticamente `\r\n` al final.

| Respuesta             | Contexto                                       |
|-----------------------|------------------------------------------------|
| `OK:ON\r\n`          | LED encendido exitosamente                     |
| `OK:OFF\r\n`         | LED apagado exitosamente                       |
| `TTS:[texto]\r\n`    | Saludo recibido, reenviado para sintesis de voz |
| `MOVE:EXPLORE\r\n`   | Movimiento Explorar iniciado                   |
| `MOVE:DANCE\r\n`     | Movimiento Bailar iniciado                     |
| `MOVE:SCAN\r\n`      | Movimiento Vigilar iniciado                    |
| `MOVE:PRANK\r\n`     | Movimiento Susto iniciado                      |
| `MOVE:DONE\r\n`      | Movimiento especial terminado/cancelado        |

### Nota sobre TTS:
Al recibir un `SALUDO`/`TEXT`/`GREETING`/`MSG`, si no se ha enviado el mismo mensaje en los ultimos `1500ms`, el ESP32 reenvia a la app: `TTS:[texto_limpio]\r\n`. La app usa esto para activar la sintesis de voz Wall-E (Piper TTS + DSP).

---

## 5. Mapeo Android -> ESP32

### Desde el Joystick virtual (modo DRIVING):
| Direccion joystick | Comando enviado |
|--------------------|-----------------|
| Arriba             | `F\n`           |
| Abajo              | `B\n`           |
| Izquierda          | `L\n`           |
| Derecha            | `R\n`           |
| Soltar / centro    | `S\n`           |

### Desde botones de giro:
| Boton     | Comando |
|-----------|---------|
| 90 Izq    | `G\n`   |
| 90 Der    | `H\n`   |

### Desde tarjetas de movimientos especiales:
| Tarjeta   | Comando |
|-----------|---------|
| Explorar  | `X\n`   |
| Bailar    | `D\n`   |
| Vigilar   | `V\n`   |
| Susto     | `P\n`   |

### Desde control de voz (VoiceControlManager):
| Frase de voz                | Comando |
|-----------------------------|---------|
| "adelante", "avanza"        | `F\n`   |
| "atras", "retrocede"        | `B\n`   |
| "izquierda"                 | `L\n`   |
| "derecha"                   | `R\n`   |
| "para", "stop", "detente"   | `S\n`   |
| "explorar", "explora"       | `X\n`   |
| "bailar", "baila"           | `D\n`   |
| "vigilar", "vigila"         | `V\n`   |
| "susto", "asustar"          | `P\n`   |
| "enciende ojos"             | `ON\n`  |
| "apaga ojos"                | `OFF\n` |

### Cambio de modo (automatico al seleccionar tab):
| Tab Android  | Comando        |
|--------------|----------------|
| Inicio       | `STANDBY\n`    |
| Control      | `DRIVING\n`    |
| Saludos      | `MODE1\n`      |
| Telemetria   | `MODE2\n` + datos |
