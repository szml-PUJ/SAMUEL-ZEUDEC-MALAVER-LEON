#define _POSIX_C_SOURCE 200809L //Habilita las funciones POSIX (getopt, getline, etc.)

/*
Autores: Juan Camilo Maldonado, Samuel Zeudec, Daniel Plazas
Fichero: agentePais.c
Objetivo del fichero: Agente de lectura de encuestas por pais. Lee el fichero encuestas_<PAIS>.csv que
produce generadorCSV, se conecta al servidor ISE por el pipe nominal y envia las encuestas mostrando
el progreso por lotes.

Formato del CSV (el que escribe generadorCSV):
  ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento
Protocolo con el servidor (servidorISE.h): MSG_CONEXION al iniciar, un MSG_DATO por encuesta y MSG_FIN al terminar.
*/

#include <stdio.h> //Para printf, fprintf, fopen, getline, rewind y snprintf
#include <stdlib.h> //Para malloc, free, strtol, strtod, EXIT_SUCCESS y EXIT_FAILURE
#include <string.h> //Para memset, memcpy, strchr, strlen, strcmp y strerror
#include <unistd.h> //Para getopt, write y close
#include <fcntl.h> //Para open, fcntl, O_WRONLY y O_NONBLOCK
#include <errno.h> //Para errno y sus codigos (EINTR, EPIPE, ENXIO, ERANGE, EIO)
#include <ctype.h> //Para isspace
#include <limits.h> //Para PIPE_BUF (tamanio maximo de una escritura atomica en un pipe)
#include <signal.h> //Para signal, SIGPIPE y SIG_IGN
#include <sys/types.h> //Para el tipo ssize_t
#include <sys/stat.h> //Para stat y S_ISFIFO

#include "servidorISE.h" //Protocolo compartido con el servidor (MensajeEncuesta, TipoMensaje, ISE_MAX_*)

//Constantes que usa el archivo para validar y leer el CSV y para mostrar el progreso

#define NUM_SECTORES      5      //Cantidad de sectores validos (ID_Sector va de 0 a 4)
#define TAM_LOTE          500    //Cada cuantas encuestas enviadas se muestra el progreso
#define MAX_SENTIMIENTO   100.0  //Valor maximo permitido para el sentimiento (porcentaje)
#define NUM_COLUMNAS      6      //Cantidad de columnas que debe tener cada linea del CSV
#define COL_PAIS          2      //Posicion de la columna Pais dentro de la linea del CSV
#define COL_SECTOR        3      //Posicion de la columna Sector (nombre) dentro de la linea del CSV
#define COL_ID_SECTOR     4      //Posicion de la columna ID_Sector dentro de la linea del CSV
#define COL_SENTIMIENTO   5      //Posicion de la columna Sentimiento dentro de la linea del CSV
#define ENCABEZADO_CSV    "ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento" //Primera linea que debe tener el CSV

_Static_assert(sizeof(MensajeEncuesta) <= PIPE_BUF, //Comprueba al compilar que el mensaje cabe en una escritura atomica
    "MensajeEncuesta debe caber en PIPE_BUF"); //Si no cabe, mensajes de varios agentes podrian mezclarse en el pipe

//Encuesta que el agente guarda en memoria antes de enviarla

typedef struct { //Estructura que guarda una encuesta
    int id_sector; //Identificador numerico del sector (0 a 4)
    char sector[ISE_MAX_SECTOR_LEN]; //Nombre del sector tal como viene en el CSV
    double sentimiento; //Valor del sentimiento (0 a 100)
} Encuesta; //Nombre del tipo para usarlo en el resto del archivo

/*
 * Muestra como se debe invocar el agente.
 */
static void mostrar_uso(const char* programa) { //Recibe el nombre con el que se ejecuto el programa
    fprintf(stderr, "Uso: %s -n <encuestas_XX.csv> -p <pipeNom>\n", programa); //Imprime el uso correcto por la salida de errores
}

/*
 * Lee los flags -n y -p en cualquier orden y valida que esten completos.
 * Retorna 0 si todo esta bien y -1 si hay algun error.
 */
static int parsear_argumentos(int argc, char** argv, const char** nombre_archivo, const char** nombre_pipe) { //Recibe los argumentos del main y entrega el archivo y el pipe
    int bandera_leida; //Guarda el flag que va devolviendo getopt

    opterr = 0; //Evita que getopt imprima sus propios mensajes de error, los manejamos nosotros
    while ((bandera_leida = getopt(argc, argv, ":n:p:")) != -1) { //Recorre los flags; ':' inicial permite distinguir flag sin valor
        switch (bandera_leida) { //Decide que hacer segun el flag encontrado
        case 'n': //Flag del nombre del fichero de encuestas
            if (*nombre_archivo != NULL) { //Si ya se habia recibido -n, esta repetido
                fprintf(stderr, "Error: la bandera -n se indico mas de una vez\n"); //Avisa el error
                return -1; //Termina con error
            }
            *nombre_archivo = optarg; //Guarda el valor que acompana a -n
            break; //Sale del switch
        case 'p': //Flag del nombre del pipe nominal
            if (*nombre_pipe != NULL) { //Si ya se habia recibido -p, esta repetido
                fprintf(stderr, "Error: la bandera -p se indico mas de una vez\n"); //Avisa el error
                return -1; //Termina con error
            }
            *nombre_pipe = optarg; //Guarda el valor que acompana a -p
            break; //Sale del switch
        case ':': //getopt devuelve ':' cuando a un flag le falta su valor
            fprintf(stderr, "Error: la bandera -%c requiere un valor\n", optopt); //Indica cual flag quedo sin valor
            return -1; //Termina con error
        default: //Cualquier otro caracter es un flag desconocido
            fprintf(stderr, "Error: bandera desconocida\n"); //Avisa el error
            return -1; //Termina con error
        }
    }

    if (optind != argc) { //Si sobraron argumentos despues de los flags
        fprintf(stderr, "Error: argumentos sobrantes\n"); //Avisa el error
        return -1; //Termina con error
    }
    if (*nombre_archivo == NULL || *nombre_pipe == NULL) { //Si falta alguno de los dos flags obligatorios
        fprintf(stderr, "Error: faltan las banderas obligatorias -n y -p\n"); //Avisa el error
        return -1; //Termina con error
    }
    if (**nombre_archivo == '\0' || **nombre_pipe == '\0') { //Si alguno de los valores es una cadena vacia
        fprintf(stderr, "Error: los valores de -n y -p no pueden estar vacios\n"); //Avisa el error
        return -1; //Termina con error
    }
    return 0; //Todos los argumentos son validos
}

/*
 * Quita los saltos de linea (\n y \r) del final de una linea leida.
 */
static void quitar_fin_linea(char* linea) { //Recibe la linea que se va a limpiar
    size_t largo_linea = strlen(linea); //Largo actual de la linea

    while (largo_linea > 0 && (linea[largo_linea - 1] == '\n' || linea[largo_linea - 1] == '\r')) //Mientras el ultimo caracter sea un salto de linea
        linea[--largo_linea] = '\0'; //Se corta la cadena un caracter antes
}

/*
 * Indica si una linea solo tiene espacios o saltos de linea.
 */
static int linea_vacia(const char* linea) { //Recibe la linea que se va a revisar
    for (; *linea != '\0'; linea++) //Recorre la linea caracter por caracter
        if (!isspace((unsigned char)*linea)) //Si encuentra un caracter que no es espacio
            return 0; //La linea no esta vacia
    return 1; //Todos los caracteres eran espacios, la linea esta vacia
}

/*
 * Separa una linea del CSV en sus columnas y valida pais, sector y sentimiento.
 * Retorna 0 si la linea es valida y -1 si tiene un formato incorrecto.
 */
static int interpretar_linea(char* linea, char* pais, Encuesta* encuesta) { //Recibe la linea y entrega el pais y la encuesta ya validados
    char* columnas[NUM_COLUMNAS]; //Punteros al inicio de cada columna dentro de la linea
    char* inicio_columna = linea; //Posicion desde donde se busca la siguiente coma

    for (int indice_columna = 0; indice_columna < NUM_COLUMNAS; indice_columna++) { //Recorre cada una de las columnas esperadas
        columnas[indice_columna] = inicio_columna; //Guarda donde empieza esta columna
        char* posicion_coma = strchr(inicio_columna, ','); //Busca la coma que cierra la columna
        if (indice_columna < NUM_COLUMNAS - 1) { //Si no es la ultima columna, debe haber una coma
            if (posicion_coma == NULL) //Si no hay coma, faltan columnas
                return -1; //Linea invalida
            *posicion_coma = '\0'; //Convierte la coma en fin de cadena para aislar la columna
            inicio_columna = posicion_coma + 1; //La siguiente columna empieza despues de la coma
        } else if (posicion_coma != NULL) { //En la ultima columna no puede haber mas comas
            return -1; //Linea invalida: sobran columnas
        }
    }

    size_t largo_pais = strlen(columnas[COL_PAIS]); //Mide el codigo del pais
    if (largo_pais == 0 || largo_pais >= ISE_MAX_PAIS_TAM) //Si esta vacio o no cabe (con su '\0') en el campo del mensaje
        return -1; //Linea invalida

    size_t largo_sector = strlen(columnas[COL_SECTOR]); //Mide el nombre del sector
    if (largo_sector == 0 || largo_sector >= ISE_MAX_SECTOR_LEN) //Si esta vacio o no cabe (con su '\0') en el campo del mensaje
        return -1; //Linea invalida

    char* texto_sobrante; //Lo que sobra despues de convertir un numero (debe quedar vacio)

    errno = 0; //Limpia errno para poder detectar errores de strtol
    long sector_leido = strtol(columnas[COL_ID_SECTOR], &texto_sobrante, 10); //Convierte el sector a numero entero
    if (texto_sobrante == columnas[COL_ID_SECTOR] || *texto_sobrante != '\0' || errno == ERANGE) //Si no era un numero completo o se desbordo
        return -1; //Linea invalida
    if (sector_leido < 0 || sector_leido >= NUM_SECTORES) //Si el sector esta fuera del rango 0 a 4
        return -1; //Linea invalida

    errno = 0; //Limpia errno para poder detectar errores de strtod
    double sentimiento_leido = strtod(columnas[COL_SENTIMIENTO], &texto_sobrante); //Convierte el sentimiento a numero decimal
    if (texto_sobrante == columnas[COL_SENTIMIENTO] || *texto_sobrante != '\0' || errno == ERANGE) //Si no era un numero completo o se desbordo
        return -1; //Linea invalida
    if (!(sentimiento_leido >= 0.0 && sentimiento_leido <= MAX_SENTIMIENTO)) //Si esta fuera de 0 a 100 (tambien rechaza NaN)
        return -1; //Linea invalida

    memcpy(pais, columnas[COL_PAIS], largo_pais + 1); //Copia el pais incluyendo el caracter de fin de cadena
    memcpy(encuesta->sector, columnas[COL_SECTOR], largo_sector + 1); //Copia el nombre del sector incluyendo el fin de cadena
    encuesta->id_sector = (int)sector_leido; //Entrega el sector ya validado
    encuesta->sentimiento = sentimiento_leido; //Entrega el sentimiento ya validado
    return 0; //La linea es valida
}

/*
 * Lee el CSV ya abierto: valida el encabezado, cuenta las encuestas, reserva la memoria justa
 * y carga las encuestas validas. Tambien obtiene el codigo del pais.
 * Retorna 0 si todo salio bien y -1 si hubo algun error. No cierra el fichero ni libera la linea.
 */
static int leer_encuestas(FILE* archivo_csv, const char* nombre_archivo, char** linea, size_t* capacidad_linea, //Recibe el fichero abierto, su nombre y el buffer de getline
    Encuesta** encuestas, size_t* total, size_t* descartadas, char* pais) { //Recibe el fichero abierto y entrega las encuestas, cuantas son, cuantas se descartaron y el pais
    if (getline(linea, capacidad_linea, archivo_csv) == -1) { //Lee la linea de encabezado; si falla, el fichero esta vacio
        fprintf(stderr, "Error: el fichero '%s' esta vacio\n", nombre_archivo); //Avisa el error
        return -1; //Termina con error
    }
    quitar_fin_linea(*linea); //Quita el salto de linea del encabezado para poder compararlo
    if (strcmp(*linea, ENCABEZADO_CSV) != 0) { //Si el encabezado no es el que escribe generadorCSV
        fprintf(stderr, "Error: '%s' no tiene el encabezado esperado (%s)\n", nombre_archivo, ENCABEZADO_CSV); //Avisa el formato esperado
        return -1; //Termina con error
    }

    size_t lineas_con_contenido = 0; //Cantidad de lineas con contenido despues del encabezado
    while (getline(linea, capacidad_linea, archivo_csv) != -1) //Primera pasada: recorre el resto del fichero
        if (!linea_vacia(*linea)) //Si la linea tiene contenido
            lineas_con_contenido++; //La cuenta como una encuesta
    if (ferror(archivo_csv)) { //Si la lectura termino por un error y no por fin de fichero
        fprintf(stderr, "Error al leer '%s': %s\n", nombre_archivo, strerror(errno)); //Avisa el error
        return -1; //Termina con error
    }
    if (lineas_con_contenido == 0) { //Si no hay ninguna encuesta despues del encabezado
        fprintf(stderr, "Error: el fichero '%s' no contiene encuestas\n", nombre_archivo); //Avisa el error
        return -1; //Termina con error
    }

    Encuesta* lista_encuestas = malloc(lineas_con_contenido * sizeof(Encuesta)); //Reserva memoria para todas las encuestas contadas
    if (lista_encuestas == NULL) { //Si no hay memoria disponible
        fprintf(stderr, "Error: no hay memoria para %zu encuestas\n", lineas_con_contenido); //Avisa el error
        return -1; //Termina con error
    }

    rewind(archivo_csv); //Vuelve al inicio del fichero para la segunda pasada
    if (getline(linea, capacidad_linea, archivo_csv) == -1) { //Salta de nuevo la linea de encabezado
        fprintf(stderr, "Error al releer '%s'\n", nombre_archivo); //Avisa el error
        free(lista_encuestas); //Libera la memoria reservada antes de salir
        return -1; //Termina con error
    }

    size_t encuestas_validas = 0; //Cantidad de encuestas validas cargadas
    size_t lineas_descartadas = 0; //Cantidad de lineas descartadas por formato incorrecto
    pais[0] = '\0'; //Marca que todavia no se conoce el pais del fichero
    while (getline(linea, capacidad_linea, archivo_csv) != -1) { //Segunda pasada: procesa cada linea
        char pais_linea[ISE_MAX_PAIS_TAM]; //Pais leido en esta linea
        Encuesta encuesta_actual; //Encuesta leida en esta linea

        if (linea_vacia(*linea)) //Si la linea esta vacia
            continue; //La ignora
        if (encuestas_validas + lineas_descartadas >= lineas_con_contenido) //Si el fichero crecio entre las dos pasadas
            break; //Para, para no salirse del arreglo reservado

        quitar_fin_linea(*linea); //Quita el salto de linea del final
        if (interpretar_linea(*linea, pais_linea, &encuesta_actual) != 0) { //Si la linea tiene formato incorrecto
            lineas_descartadas++; //La cuenta como descartada
            continue; //Pasa a la siguiente linea
        }
        if (pais[0] == '\0') { //Si es la primera linea valida
            memcpy(pais, pais_linea, strlen(pais_linea) + 1); //Su pais pasa a ser el pais del fichero
        } else if (strcmp(pais, pais_linea) != 0) { //Si esta linea es de un pais distinto al del fichero
            lineas_descartadas++; //La cuenta como descartada
            continue; //Pasa a la siguiente linea
        }
        lista_encuestas[encuestas_validas] = encuesta_actual; //Guarda la encuesta en el arreglo
        encuestas_validas++; //Cuenta una encuesta valida mas
    }

    if (encuestas_validas == 0) { //Si ninguna linea resulto valida
        fprintf(stderr, "Error: el fichero '%s' no contiene encuestas validas\n", nombre_archivo); //Avisa el error
        free(lista_encuestas); //Libera la memoria reservada antes de salir
        return -1; //Termina con error
    }

    *encuestas = lista_encuestas; //Entrega el arreglo a quien llamo la funcion
    *total = encuestas_validas; //Entrega la cantidad de encuestas validas
    *descartadas = lineas_descartadas; //Entrega la cantidad de lineas descartadas
    return 0; //Lectura exitosa
}

/*
 * Abre el fichero CSV, lo lee con leer_encuestas y lo cierra.
 * Retorna 0 si todo salio bien y -1 si hubo algun error.
 */
static int cargar_encuestas(const char* nombre_archivo, Encuesta** encuestas, size_t* total, size_t* descartadas, char* pais) { //Recibe el nombre del fichero CSV y entrega las encuestas, cuantas son, cuantas se descartaron y el pais
    FILE* archivo_csv = fopen(nombre_archivo, "r"); //Abre el fichero CSV en modo lectura
    if (archivo_csv == NULL) { //Si no se pudo abrir
        fprintf(stderr, "Error: no se pudo abrir '%s': %s\n", nombre_archivo, strerror(errno)); //Avisa el motivo
        return -1; //Termina con error (todavia no hay nada que liberar)
    }

    char* linea = NULL; //Buffer donde getline guarda cada linea (getline lo reserva y lo agranda solo)
    size_t capacidad_linea = 0; //Tamanio actual del buffer de getline
    int resultado = leer_encuestas(archivo_csv, nombre_archivo, &linea, &capacidad_linea, encuestas, total, descartadas, pais); //Hace toda la lectura del fichero

    free(linea); //Libera el buffer de getline
    fclose(archivo_csv); //Cierra el fichero CSV
    return resultado; //Retorna 0 si fue exitoso o -1 si hubo error
}

/*
 * Abre el pipe nominal solo para escritura.
 * Retorna el descriptor del pipe o -1 si no se pudo abrir.
 */
static int abrir_pipe(const char* nombre_pipe) { //Recibe el nombre del pipe nominal
    struct stat informacion_pipe; //Informacion del fichero que se va a abrir

    if (stat(nombre_pipe, &informacion_pipe) == -1) { //Si el pipe no existe o no se puede acceder
        fprintf(stderr, "Error: no se puede acceder al pipe '%s': %s\n", nombre_pipe, strerror(errno)); //Avisa el motivo
        return -1; //Termina con error
    }
    if (!S_ISFIFO(informacion_pipe.st_mode)) { //Si existe pero no es un pipe nominal
        fprintf(stderr, "Error: '%s' no es un pipe nominal\n", nombre_pipe); //Avisa el error
        return -1; //Termina con error
    }

    int descriptor_pipe = open(nombre_pipe, O_WRONLY | O_NONBLOCK); //Abre sin bloquear: falla con ENXIO si el servidor no esta leyendo
    if (descriptor_pipe == -1) { //Si no se pudo abrir
        if (errno == ENXIO) //ENXIO significa que ningun proceso tiene el pipe abierto para lectura
            fprintf(stderr, "Error: no hay un servidor escuchando en el pipe '%s'\n", nombre_pipe); //Explica que falta el servidor
        else //Cualquier otro error
            fprintf(stderr, "Error al abrir el pipe '%s': %s\n", nombre_pipe, strerror(errno)); //Muestra el motivo
        return -1; //Termina con error
    }

    int banderas_actuales = fcntl(descriptor_pipe, F_GETFL); //Lee las banderas actuales del descriptor
    if (banderas_actuales == -1 || fcntl(descriptor_pipe, F_SETFL, banderas_actuales & ~O_NONBLOCK) == -1) { //Quita O_NONBLOCK para que write espere si el pipe se llena
        fprintf(stderr, "Error al configurar el pipe '%s': %s\n", nombre_pipe, strerror(errno)); //Avisa el motivo
        close(descriptor_pipe); //Cierra el descriptor antes de salir
        return -1; //Termina con error
    }
    return descriptor_pipe; //Retorna el descriptor listo para escribir
}

/*
 * Arma un MensajeEncuesta y lo escribe completo en el pipe con una sola llamada a write.
 * Retorna 0 si se envio y -1 si fallo (errno queda con el motivo).
 */
static int enviar_mensaje(int descriptor_pipe, TipoMensaje tipo, const char* pais, const char* sector, int id_sector, double sentimiento) { //Recibe el pipe y los datos del mensaje
    MensajeEncuesta mensaje; //Mensaje que se va a enviar al servidor

    memset(&mensaje, 0, sizeof(mensaje)); //Pone todo en cero para no enviar basura (incluye el relleno de la estructura)
    mensaje.tipo = tipo; //Tipo de mensaje: conexion, dato o fin
    snprintf(mensaje.pais, sizeof(mensaje.pais), "%s", pais); //Copia el pais asegurando el fin de cadena
    snprintf(mensaje.sector, sizeof(mensaje.sector), "%s", sector); //Copia el nombre del sector asegurando el fin de cadena
    mensaje.id_sector = id_sector; //Identificador numerico del sector
    mensaje.sentimiento = sentimiento; //Sentimiento de la encuesta

    for (;;) { //Repite solo si la escritura es interrumpida por una senal
        ssize_t bytes_escritos = write(descriptor_pipe, &mensaje, sizeof(mensaje)); //Escribe el mensaje completo en el pipe
        if (bytes_escritos == (ssize_t)sizeof(mensaje)) //Si se escribio todo
            return 0; //Envio exitoso
        if (bytes_escritos == -1 && errno == EINTR) //Si una senal interrumpio la escritura
            continue; //Reintenta
        if (bytes_escritos >= 0) //Si se escribio solo una parte (no deberia pasar con escrituras atomicas)
            errno = EIO; //Se trata como error de entrada/salida
        return -1; //Envio fallido
    }
}

/*
 * Muestra el motivo por el que fallo una escritura en el pipe.
 */
static void informar_fallo_envio(void) { //No recibe parametros, usa errno
    if (errno == EPIPE) //EPIPE significa que el servidor cerro el pipe
        fprintf(stderr, "Error: el servidor cerro el pipe, se aborta el envio\n"); //Explica que el servidor ya no esta
    else //Cualquier otro error de escritura
        fprintf(stderr, "Error al escribir en el pipe: %s\n", strerror(errno)); //Muestra el motivo
}

/*
 * Envia al servidor la conexion, todas las encuestas por lotes y el mensaje de fin.
 * Retorna 0 si todo se envio y -1 si hubo algun error (el motivo ya se mostro).
 */
static int enviar_encuestas(int descriptor_pipe, const char* pais, const Encuesta* encuestas, size_t total) { //Recibe el pipe abierto, el pais y las encuestas a enviar
    if (enviar_mensaje(descriptor_pipe, MSG_CONEXION, pais, "", 0, 0.0) != 0) { //Envia el mensaje inicial que identifica al agente
        informar_fallo_envio(); //Muestra el motivo del fallo
        return -1; //Termina con error
    }
    printf("Conectado.\n"); //Confirma que el mensaje de conexion fue enviado

    printf("Enviando...\n"); //Avisa que empieza el envio de encuestas
    for (size_t indice_encuesta = 0; indice_encuesta < total; indice_encuesta++) { //Recorre todas las encuestas cargadas
        if (enviar_mensaje(descriptor_pipe, MSG_DATO, pais, encuestas[indice_encuesta].sector, encuestas[indice_encuesta].id_sector, encuestas[indice_encuesta].sentimiento) != 0) { //Envia una encuesta
            informar_fallo_envio(); //Muestra el motivo del fallo
            return -1; //Termina con error
        }
        if ((indice_encuesta + 1) % TAM_LOTE == 0 && indice_encuesta + 1 < total) //Si se completo un lote y todavia faltan encuestas
            printf("  %zu/%zu\n", indice_encuesta + 1, total); //Muestra el progreso
    }
    printf("Enviadas %zu/%zu encuestas.\n", total, total); //Informa que se enviaron todas las encuestas

    if (enviar_mensaje(descriptor_pipe, MSG_FIN, pais, "", 0, 0.0) != 0) { //Envia el mensaje que indica al servidor que no hay mas encuestas
        informar_fallo_envio(); //Muestra el motivo del fallo
        return -1; //Termina con error
    }
    printf("Mensaje de finalizacion enviado.\n"); //Confirma que el mensaje de fin fue enviado
    return 0; //Todo el envio termino bien
}

int main(int argc, char** argv) { //Punto de entrada del agente
    const char* nombre_archivo = NULL; //Nombre del fichero CSV (flag -n)
    const char* nombre_pipe = NULL; //Nombre del pipe nominal (flag -p)

    if (parsear_argumentos(argc, argv, &nombre_archivo, &nombre_pipe) != 0) { //Valida los flags -n y -p
        mostrar_uso(argv[0]); //Si hay error, recuerda como se usa el programa
        return EXIT_FAILURE; //Termina con error
    }

    signal(SIGPIPE, SIG_IGN); //Ignora SIGPIPE para que escribir sin servidor devuelva EPIPE en vez de matar el proceso

    Encuesta* encuestas = NULL; //Encuestas cargadas en memoria
    size_t total_encuestas = 0; //Cantidad de encuestas validas a enviar
    size_t lineas_descartadas = 0; //Cantidad de lineas del CSV descartadas
    char pais[ISE_MAX_PAIS_TAM] = ""; //Codigo del pais de este agente
    if (cargar_encuestas(nombre_archivo, &encuestas, &total_encuestas, &lineas_descartadas, pais) != 0) //Lee el CSV y deja las encuestas en memoria
        return EXIT_FAILURE; //Si fallo, termina con error (cargar_encuestas ya mostro el motivo)

    printf("==============================================\n"); //Linea superior del encabezado
    printf("      AGENTE LECTURA - ENVIO DE ENCUESTAS\n"); //Titulo del programa
    printf("==============================================\n"); //Linea inferior del encabezado
    printf("Pais: %s\n", pais); //Muestra el pais que se va a enviar
    printf("Pipe: %s\n", nombre_pipe); //Muestra el pipe que se va a usar
    printf("Leidas %zu encuestas.\n", total_encuestas); //Muestra cuantas encuestas validas se leyeron
    if (lineas_descartadas > 0) //Si hubo lineas con formato incorrecto
        printf("Lineas descartadas por formato invalido: %zu\n", lineas_descartadas); //Informa cuantas fueron

    printf("Conectando al proceso principal...\n"); //Avisa que se intenta la conexion
    int descriptor_pipe = abrir_pipe(nombre_pipe); //Abre el pipe nominal para escribir
    if (descriptor_pipe == -1) { //Si no se pudo abrir
        free(encuestas); //Libera las encuestas en memoria antes de salir
        return EXIT_FAILURE; //Termina con error
    }

    int resultado_envio = enviar_encuestas(descriptor_pipe, pais, encuestas, total_encuestas); //Envia la conexion, las encuestas y el fin
    close(descriptor_pipe); //Cierra el pipe, haya salido bien o mal el envio
    free(encuestas); //Libera las encuestas en memoria

    if (resultado_envio != 0) //Si el envio fallo
        return EXIT_FAILURE; //Termina con error (enviar_encuestas ya mostro el motivo)

    printf("Envio %s finalizado satisfactoriamente!!.\n", pais); //Mensaje final con el nombre del pais
    return EXIT_SUCCESS; //Termina sin errores
}
