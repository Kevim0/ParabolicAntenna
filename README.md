# Controlador Pan/Tilt con ESP32

Este repositorio contiene el firmware para un ESP32 diseñado para controlar un mecanismo **Pan/Tilt** mediante motores paso a paso.

El objetivo principal del sistema es permitir el **rastreo y análisis de redes WiFi utilizando movimiento físico automatizado**, permitiendo orientar sensores, antenas o módulos inalámbricos mientras el ESP32 continúa ejecutando tareas de comunicación y escaneo de red en tiempo real.

En la arquitectura original de este proyecto, el ESP32 actúa como un **microcontrolador esclavo conectado a una Raspberry Pi**. Mientras la Raspberry Pi se encarga de la lógica principal, análisis de datos y coordinación de alto nivel, el ESP32 maneja tareas críticas de tiempo real, como:
- generación precisa de pulsos para motores,
- control de movimiento,
- lectura de periféricos,
- y operaciones WiFi asíncronas.

Esto permite que el sistema:
- continúe moviéndose,
- escaneando redes,
- y respondiendo comandos
sin bloqueos ni interrupciones perceptibles.

La comunicación entre la Raspberry Pi y el ESP32 se realiza de forma asíncrona mediante una **API de comandos JSON a través de Serial**.

---

# Objetivo del Proyecto

El firmware fue diseñado para aplicaciones donde es necesario:
- mover físicamente un sistema Pan/Tilt,
- mientras se monitorean o rastrean señales WiFi en tiempo real.

Ejemplos:
- orientación automática de antenas,
- análisis direccional de intensidad de señal,
- rastreo de redes inalámbricas,
- sistemas robóticos de escaneo,
- plataformas IoT móviles.

La arquitectura multitarea permite ejecutar simultáneamente:
- movimiento de motores,
- comunicación serial,
- lectura de RPM,
- y operaciones WiFi,
sin utilizar `delay()` ni bloquear el `loop()` principal.

---

# Nota sobre el Control del Ventilador

Este firmware incluye rutinas completas para controlar un ventilador de 4 pines:
- señal PWM a 25kHz,
- lectura de RPM mediante interrupciones de hardware.

Sin embargo, **en el diseño original del proyecto, el ventilador está conectado y controlado directamente por la Raspberry Pi**, no por el ESP32.

El código de control del ventilador se mantiene en este repositorio para ofrecer flexibilidad en caso de:
- utilizar el ESP32 de forma independiente,
- o reestructurar el hardware en proyectos derivados.

---

# Características Principales

- **Arquitectura de coprocesador:** descarga a la Raspberry Pi de tareas sensibles al tiempo real.
- **Movimiento y WiFi simultáneos:** capacidad de mover motores mientras se realizan tareas de conectividad o escaneo inalámbrico.
- **Multitarea cooperativa (no bloqueante):** uso de máquinas de estado (`tickStepper`, `tickWifi`, etc.) en lugar de `delay()`.
- **Control preciso de motores paso a paso:** manejo de posición, velocidad y dirección.
- **Comunicación JSON:** interfaz simple y robusta para integraciones con Python, Node.js u otros lenguajes.
- **Doble opción de Serial:** comunicación vía USB o UART hardware dedicado.
- **Control opcional de ventilador:** PWM y lectura de RPM.
- **WiFi asíncrono opcional:** conexión y operaciones WiFi sin bloquear la ejecución de motores.

---

# Asignación de Pines (Pinout)

| Componente | Pin ESP32 | Descripción |
|---|---|---|
| **Pan Stepper** | `25` | STEP (Paso) |
|  | `26` | DIR (Dirección) |
| **Tilt Stepper** | `32` | STEP (Paso) |
|  | `33` | DIR (Dirección) |
| **Ventilador (Opcional)** | `27` | PWM Out (25kHz) |
|  | `34` | TACH In (Lectura RPM) |
| **UART (Hacia RPi)** | `16` | RX2 |
|  | `17` | TX2 |

---

# Configuración e Instalación

## 1. Dependencias

Asegúrate de tener instalada la librería:

- `ArduinoJson` (v6 o v7)

Compatible con:
- Arduino IDE
- PlatformIO

---

## 2. Selección de Puerto Serie

Por defecto, el firmware utiliza el puerto Serial principal (USB).

Para integración con Raspberry Pi mediante UART hardware:

```cpp
bool UART_CON = true;
```

Activa esta opción en la línea correspondiente del código fuente.

---

## 3. Baudrate

La Raspberry Pi debe abrir la conexión serie utilizando:

```txt
115200 baudios
```

Cada comando JSON debe terminar con:

```txt
\n
```

---

# API de Comandos JSON (Serial)

La Raspberry Pi (o cualquier controlador maestro) debe enviar objetos JSON al ESP32.

El ESP32 responderá siempre con:
- confirmaciones,
- estados,
- o eventos asíncronos.

---

# 1. Control de Motores (Pan / Tilt)

## Mover eje Pan

```json
{
  "stepPanSteps": 200,
  "stepPanClockwise": true,
  "stepPanStepDelayus": 500
}
```

### Parámetros

- `stepPanSteps`: cantidad de pasos.
- `stepPanClockwise`: dirección.
- `stepPanStepDelayus`: tiempo entre medio pasos en microsegundos.

Rango válido:
```txt
200 - 1000
```

---

## Mover eje Tilt

```json
{
  "stepTiltSteps": 150,
  "stepTiltClockwise": false,
  "stepTiltStepDelayus": 300
}
```

---

## Mover Pan y Tilt simultáneamente

```json
{
  "mptPanSteps": 400,
  "mptTiltSteps": 200,
  "mptPanCW": true,
  "mptTiltCW": false,
  "mptStepDelayus": 400
}
```

---

## Detener ambos motores inmediatamente

```json
{
  "stopMotors": true
}
```

---

## Obtener estado de los motores

```json
{
  "getStepperInfo": true
}
```

---

# 2. Control del Ventilador (Opcional)

> Estos comandos solo aplican si el ventilador está conectado al ESP32.

## Ajustar velocidad PWM

Rango:
```txt
0 - 255
```

```json
{
  "pwmValue": 128
}
```

---

## Obtener información del ventilador

```json
{
  "getFanInfo": true
}
```

---

# 3. Conectividad WiFi (Opcional)

## Conectar a red WiFi

```json
{
  "connectToWifi": true,
  "ssid": "MI_RED_WIFI",
  "password": "mi_password_secreto"
}
```

---

# Formato de Respuestas y Eventos

La arquitectura es completamente asíncrona.

La respuesta inmediata solo confirma la recepción del comando. Posteriormente, el firmware enviará eventos cuando una acción física termine.

---

## Respuesta de éxito

```json
{
  "status": 200,
  "command": "stepPan",
  "steps": 200,
  "clockwise": true,
  "stepDelayUs": 500
}
```

---

## Evento asíncrono (movimiento completado)

```json
{
  "status": 200,
  "event": "moveDone",
  "motor": "pan",
  "position": 200
}
```

---

# Lógica Interna del Firmware

El firmware funciona sin sistema operativo (*Bare Metal*), utilizando multitarea cooperativa dentro del `loop()` principal mediante el patrón `tick`.

---

## `tickSerial()`

Responsabilidades:
- acumulación no bloqueante de caracteres,
- detección de JSON completos,
- procesamiento de comandos Serial.

---

## `tickStepper()`

Es el núcleo del sistema de movimiento.

Responsabilidades:
- control de dirección,
- generación precisa de pulsos STEP,
- sincronización mediante `micros()`,
- temporización de motores sin bloqueos.

Utiliza máquinas de estado para permitir:
- movimiento simultáneo,
- comunicación continua,
- y ejecución paralela de otras tareas.

---

## `tickWifi()`

Gestiona operaciones WiFi de forma asíncrona.

Esto permite:
- escaneo de redes,
- conectividad,
- monitoreo inalámbrico,
- y rastreo WiFi
sin detener el movimiento físico del sistema.

---

## `tickFanRpm()`

Responsabilidades:
- lectura de RPM,
- procesamiento de interrupciones,
- actualización periódica mediante `millis()`.

---