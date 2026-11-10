# ¡Hola! Soy Samuel Zeudec Malaver León

### Estudiante de Ingeniería de Sistemas | Futuro ingeniero de sistemas

Soy estudiante de Ingeniería de Sistemas en la Pontificia Universidad Javeriana. Este repositorio es parte de mi camino aprendiendo a construir software de verdad: en este caso, un sistema en C para Linux que usa procesos, hilos, semáforos y pipes.

-  Estudiante de Ingeniería de Sistemas.
-  Me interesa la programación y el desarrollo de software.
-  Aprendiendo cada día, y mejor cuando me toca explicar lo que hice.

Más abajo está el proyecto. Si buscas solo lo técnico, salta a [Indicador de Sentimiento Económico (ISE)](#indicador-de-sentimiento-económico-ise).

---

# Indicador de Sentimiento Económico (ISE)

Proyecto de *Sistemas Operativos 2026-30*, Pontificia Universidad Javeriana.
Profesor: John Jairo Corredor Franco.

Sistema en C para Linux que recibe encuestas de varios países al mismo tiempo, las procesa con procesos, hilos y semáforos POSIX, y calcula el ISE, un promedio ponderado del sentimiento económico que recomienda *vender, **comprar* o mantenerse *neutro*.

## Autores

- Samuel Zeudec Malever León
- Daniel Mauricio Plazas López
- Juan Camilo Maldonado

## ¿Qué hace?

Hay tres países (ES, FR, IT) y cinco sectores. Cada encuesta trae un sentimiento entre 0 y 100. El servidor promedia cada sector y calcula el ISE con estos pesos:

| Sector | Peso |
|---|---|
| Industria | 40 % |
| Servicios | 30 % |
| Consumidores | 20 % |
| Comercio | 5 % |
| Construcción | 5 % |

- ISE *mayor que 50*: vender.
- ISE *menor que 50*: comprar.
- ISE *igual a 50*: neutro.

## Arquitectura

Son tres programas que se comunican por un *pipe nominal*:

```
generadorCSV ──> encuestas_XX.csv ──> agentePais (uno por país) ──┐
                                                                  │ pipe nominal
                                                                  v
                                   servidorISE: padre (mkfifo, fork, waitpid, unlink)
                                                  └─ hijo ── hilo receptor (lee el pipe)
                                                          ├─ hilo temporizador (-t)
                                                          └─ un hilo consumidor por agente
                                                             (buffer circular + semáforos)
```

| Archivo | Qué hace |
|---|---|
| generadorCSV.c | Crea el CSV de un país con la cantidad de encuestas que indica el porcentaje. |
| agentePais.c | Lee el CSV, valida cada línea y envía las encuestas al servidor por el pipe. |
| servidorISE.c / .h | Recibe de varios agentes a la vez, acumula por sector y calcula el ISE. |
| main.c | Lee y valida las banderas del servidor. |
| medir_tiempos.c | Programa extra que mide cuánto tarda el sistema (ver más abajo). |
| Makefile | Compila los tres programas. |

### Técnicas de sistemas operativos que usa

- *Procesos:* fork y waitpid. El padre crea el pipe y lo borra al final; el hijo hace el trabajo.
- *Pipe nominal:* mkfifo. Cada mensaje es de tamaño fijo y menor que PIPE_BUF, así que cada write es atómico y los mensajes de varios agentes no se mezclan.
- *Hilos POSIX:* un receptor, un temporizador y un consumidor por agente.
- *Semáforos POSIX:* dos por agente (espacios libres y datos disponibles) para el buffer circular, con sem_timedwait de un segundo.
- *Mutex:* uno protege la asignación de casillas y otro la bandera de cierre.
- *poll:* el receptor revisa el pipe cada 500 ms para poder cerrar a tiempo.

## Requisitos

- Linux
- gcc y make
- Soporte para hilos POSIX (-pthread)

## Compilación

bash
make


Compila con gcc -Wall -Wextra -pthread y deja los ejecutables servidorISE, agentePais y generadorCSV. Para limpiar:

bash
make clean


## Uso

### 1. Generar las encuestas

bash
./generadorCSV -p ES -e 100


| Bandera | Significado |
|---|---|
| -p | País: ES, FR o IT |
| -e | Porcentaje de encuestas, entero de 1 a 100 |

Crea encuestas_ES.csv. Las banderas pueden ir en cualquier orden.

### 2. Iniciar el servidor

bash
./servidorISE -b 10 -p ise_fifo -t 30 -a 3


| Bandera | Significado | Rango |
|---|---|---|
| -b | Tamaño del buffer de cada agente | mayor que 0 |
| -p | Nombre del pipe nominal | texto |
| -t | Segundos que espera el servidor | 30 a 120 |
| -a | Máximo de agentes | 1 a 4 |

### 3. Lanzar los agentes (uno por terminal)

bash
./agentePais -n encuestas_ES.csv -p ise_fifo
./agentePais -n encuestas_FR.csv -p ise_fifo
./agentePais -n encuestas_IT.csv -p ise_fifo


| Bandera | Significado |
|---|---|
| -n | Archivo CSV con las encuestas |
| -p | Nombre del pipe del servidor |

El servidor muestra un *reporte parcial* por país cuando cada agente termina y, al cumplirse -t, el *resumen general* con el ISE y la recomendación.

> El servidor siempre espera su -t completo, así que -t debe ser mayor que el tiempo que tardan los agentes.

## Pruebas

Se hicieron 10 pruebas funcionales: tres de generación, la prueba integral con tres agentes, el rechazo de un agente que supera -a, banderas fuera de rango, agente sin servidor, CSV sin encabezado y valores extremos del generador. Están documentadas en la Tabla 5 del documento de sustentación.

Ejemplo de la prueba integral (España 76 %, Italia 38 %, Francia 14 %): el servidor recibió 5001, 4978 y 5130 encuestas, sin perder ninguna, y el ISE dio un valor cercano a 50.

## Medición de tiempos (extra)

medir_tiempos.c repite cada medición tres veces y promedia. Se compila aparte y se corre después de make:

bash
gcc -Wall -Wextra -pthread medir_tiempos.c -o medir_tiempos
./medir_tiempos


Guarda los resultados en resultados_tiempos.csv. Resultados principales (un agente, buffer de 10):

| Encuestas | Tiempo total |
|---|---|
| 5000 | 0,025 s |
| 6000 | 0,026 s |
| 36640 | 0,141 s |

Con un buffer de tamaño 1, 6000 encuestas tardan 0,267 s, unas diez veces más que con buffer de 10.

## Límites conocidos

- El pipe es de un solo sentido: un agente rechazado por superar -a no se entera.
- Si un agente llega después de que el servidor cumplió su tiempo, sus encuestas no se procesan.
- Un sector sin encuestas cuenta como 0 con su peso completo, por lo que un CSV recortado con head baja el ISE.

## Documentación

-  *Video de la sustentación:* [ver en YouTube](https://youtu.be/QvJGfooIVXc)
-  La carpeta Docs(Sustentacion) contiene el documento del proyecto (PDF) y el video de la sustentación.

---

##  Sobre mí

-  Estudiante de Ingeniería de Sistemas en la Pontificia Universidad Javeriana.
-  Construyendo proyectos académicos y personales.
-  Quiero desarrollar habilidades técnicas y crear soluciones tecnológicas que reflejen mi crecimiento académico y profesional.

##  Actualmente aprendiendo

- Sistemas operativos: procesos, hilos, sincronización y comunicación entre procesos.
- Programación en C.
- Linux y herramientas de terminal.
- Git y GitHub.

##  Otros proyectos

- *Sistemas Operativos:* prácticas, ejercicios y trabajos de la asignatura.

Esta sección crecerá a medida que desarrolle nuevos proyectos.

##  Contacto

- GitHub: [szml-PUJ](https://github.com/szml-PUJ)

---

Aprendiendo, construyendo y mejorando continuamente.
