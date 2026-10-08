# Instrucciones de uso — ILU (todo lo de terminal)

Guía rápida para el día de la demo. Todos los comandos se ejecutan **desde la raíz del repo**:

```bash
cd ~/Documents/ILU
```

Orden normal para levantar todo:

1. Comprobar la IP del laptop (sección 1)
2. Comprobar que Mosquitto está corriendo (sección 2)
3. Encender los nodos y ver que llegan mensajes (sección 3)
4. Levantar el dashboard (sección 4)

---

## 1. La IP del laptop (lo primero que hay que revisar)

Los nodos tienen **grabada en el firmware** la dirección del broker (`MQTT_BROKER_URI` en
`shared/net/secrets.h`). Si el laptop cambia de IP, los nodos se conectan al Wi-Fi pero **no** al
broker, y el dashboard los marca offline.

### Ver la IP actual

```bash
ip -4 -br addr
```

Busca la interfaz que está `UP` en la red `192.168.1.x` (la del router `CLARO_2.4GHz_768DE4`):

- Ethernet (normalmente `enp…` / `eth…`): esperada `192.168.1.110`
- Wi-Fi (`wlo1`): esperada `192.168.1.187`

> ⚠️ Si `wlo1` muestra algo como `172.20.10.x`, el laptop está conectado al hotspot del celular,
> no al router. Los nodos solo están en el router → conéctate al router primero.

### Ver qué IP tienen grabada los nodos

```bash
grep MQTT_BROKER_URI shared/net/secrets.h
```

### Si no coinciden

**Opción A (mejor):** reservar `192.168.1.110` para el laptop en el router (DHCP reservation)
y reiniciar la conexión. No hay que tocar firmware.

**Opción B:** cambiar la IP en el firmware:

```bash
nano shared/net/secrets.h
#   #define MQTT_BROKER_URI "mqtt://<IP-NUEVA>:1883"
```

Y luego **recompilar y flashear los dos nodos** (sección 5). Los dos usan el mismo `secrets.h`.

### Comprobar que el broker es alcanzable en esa IP

```bash
mosquitto_sub -h <IP-DEL-LAPTOP> -t 'ilu/#' -v
```

Si dice `Connection refused`, revisa la sección 2.

---

## 2. Broker MQTT (Mosquitto)

Corre como servicio del sistema; normalmente ya arranca solo con el laptop.

```bash
systemctl status mosquitto          # ¿está activo?
sudo systemctl restart mosquitto    # reiniciarlo
sudo systemctl start mosquitto      # si estaba parado
```

Tiene que escuchar en `0.0.0.0:1883` (toda la red), no solo en localhost:

```bash
ss -ltn | grep 1883
#   0.0.0.0:1883  → bien
#   127.0.0.1:1883 → falta la config del repo
```

Si falta la config (por ejemplo después de reinstalar Mosquitto):

```bash
sudo cp platform/mosquitto/ilu.conf /etc/mosquitto/conf.d/
sudo systemctl restart mosquitto
```

Si el firewall está activo (`sudo ufw status`), abrir los puertos:

```bash
sudo ufw allow 1883/tcp    # MQTT desde los nodos
sudo ufw allow 5000/tcp    # dashboard desde otro dispositivo
```

---

## 3. Ver el tráfico de los nodos

Con los nodos encendidos (tardan ~12–15 s en unirse al Wi-Fi):

```bash
mosquitto_sub -h 192.168.1.110 -t 'ilu/#' -v
```

Debe salir ~1 mensaje por segundo de cada nodo:

```
ilu/a/state {"node":"a","up":...,"warm":true,"motion":false,...}
ilu/b/state {"node":"b","up":...,"cm":40,"near":false,...}
```

Si no sale nada: IP (sección 1), broker (sección 2), o mira el monitor serie (sección 6).

`Ctrl+C` para salir.

---

## 4. Levantar el dashboard

```bash
env -u PYTHONPATH platform/.venv/bin/python platform/app.py
```

Luego abrir:

- En el laptop: <http://localhost:5000>
- Desde otro dispositivo de la misma red: `http://<IP-DEL-LAPTOP>:5000`

`Ctrl+C` para pararlo.

**¿Por qué `env -u PYTHONPATH`?** El perfil de la shell añade los paquetes de ROS Humble al
`PYTHONPATH` y se mezclan con los del venv. Esto los quita solo para este comando.

**Si el puerto 5000 está ocupado** (quedó un servidor viejo abierto):

```bash
ss -ltnp | grep 5000      # ver qué proceso lo tiene
pkill -f platform/app.py  # matar el servidor anterior
```

**El dashboard se conecta al broker en `localhost`** por defecto (el broker está en el mismo
laptop, así que no depende de la IP). Si algún día el broker estuviera en otra máquina:

```bash
ILU_BROKER=<IP-DEL-BROKER> env -u PYTHONPATH platform/.venv/bin/python platform/app.py
```

### Recrear el entorno de Python (si `platform/.venv` no existe o se rompió)

```bash
rm -rf platform/.venv
/usr/bin/python3 -m venv platform/.venv
platform/.venv/bin/pip install -r platform/requirements.txt
```

---

## 5. Recompilar y flashear los nodos (después de cualquier cambio)

### 5.1 Activar ESP-IDF (una vez por terminal)

```bash
get_idf
```

(Es un alias de `. $HOME/esp/esp-idf/export.sh`, definido en `~/.bashrc`.)
Sin esto, `idf.py` no existe en esa terminal.

### 5.2 Identificar los puertos

Los `ttyUSB0` / `ttyUSB1` cambian según el orden en que se conectan. Usa siempre la ruta por
adaptador:

```bash
ls /dev/serial/by-id/
```

```bash
PORT_A=/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
PORT_B=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_7a041be62e9cef118535af9061ce3355-if00-port0
```

Si hay dudas de qué placa es cuál, la MAC no miente:

```bash
esptool.py -p $PORT_A read_mac    # node_a → 78:42:1c:68:44:98
esptool.py -p $PORT_B read_mac    # node_b → f4:65:0b:c0:e0:a4
```

### 5.3 Compilar

```bash
idf.py -C node_a build
idf.py -C node_b build
```

Al final de cada build aparece cuánto espacio libre queda en la partición de la app
(hoy ~14 %). Si dice que no cabe, ver "Flash budget" en `CLAUDE.md`.

### 5.4 Flashear

```bash
idf.py -C node_a -p $PORT_A flash
idf.py -C node_b -p $PORT_B flash
```

`flash` también compila si hace falta, así que `build` + `flash` se puede hacer en un paso.

### 5.5 ¿Qué hay que recompilar según lo que cambié?

| Cambié… | Recompilar y flashear |
|---|---|
| `shared/net/secrets.h` (IP del broker, Wi-Fi) | **los dos nodos** |
| `shared/core/*` o `shared/net/*` | **los dos nodos** |
| `node_a/main/*` | solo node_a |
| `node_b/main/*` | solo node_b |
| `platform/*` (dashboard) | nada de firmware; solo reiniciar `app.py` (`Ctrl+C` y volver a lanzarlo) |
| `platform/mosquitto/ilu.conf` | copiarlo a `/etc/mosquitto/conf.d/` y reiniciar Mosquitto |

### 5.6 Si el build se pone raro

```bash
idf.py -C node_a fullclean
idf.py -C node_a build
```

### 5.7 Problemas típicos al flashear

- `Permission denied` en el puerto → el usuario no está en el grupo `dialout`:
  `sudo usermod -aG dialout $USER` y cerrar sesión / volver a entrar.
- `Failed to connect to ESP32` → mantener pulsado **BOOT** en la placa al empezar el flasheo.
- `Device or resource busy` → hay un `monitor` abierto en ese puerto; ciérralo (`Ctrl+]`).

---

## 6. Monitor serie (ver los logs de un nodo)

```bash
idf.py -C node_a -p $PORT_A monitor
idf.py -C node_b -p $PORT_B monitor
```

- `Ctrl+]` para salir.
- `Ctrl+T` y luego `Ctrl+R` reinicia la placa sin desconectarla.

Flashear y abrir el monitor en un solo comando:

```bash
idf.py -C node_a -p $PORT_A flash monitor
```

Qué buscar en los logs:

- Conexión a Wi-Fi (tarda ~12–15 s, con 2–3 reintentos es normal)
- `MQTT connected` ~0,4 s después de obtener IP
- Si sale Wi-Fi OK pero nunca MQTT → la IP del broker está mal (sección 1)

---

## 7. Tests del núcleo compartido (sin placas)

Compila `shared/core` en el PC (sin ESP-IDF) y corre los 15 tests del protocolo:

```bash
cmake -S . -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

---

## 8. Checklist rápido antes de la demo

```bash
cd ~/Documents/ILU
ip -4 -br addr                                   # ¿laptop en 192.168.1.110?
grep MQTT_BROKER_URI shared/net/secrets.h        # ¿coincide con la de arriba?
systemctl is-active mosquitto                    # → active
ss -ltn | grep 1883                              # → 0.0.0.0:1883
mosquitto_sub -h 192.168.1.110 -t 'ilu/#' -v     # ¿mensajes de a y b? (Ctrl+C)
env -u PYTHONPATH platform/.venv/bin/python platform/app.py   # → http://localhost:5000
```

Terminales recomendadas durante la demo:

1. `app.py` (dashboard)
2. `mosquitto_sub` (tráfico MQTT en crudo, buena prueba visual para los evaluadores)
3. (opcional) `monitor` de un nodo, si quieres enseñar los logs de ESP-NOW
