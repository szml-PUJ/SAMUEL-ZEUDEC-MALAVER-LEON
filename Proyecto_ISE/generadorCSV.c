#define _POSIX_C_SOURCE 200809L 
/*
Autores: Juan Camilo Maldonado, Samuel Zeudec Malaver , Daniel Plazas
Fichero: generadorCSV.c
Objetivo del fichero: Generar el fichero encuestas_<PAIS>.csv con encuestas aleatorias por sector,
segun los maximos de la Tabla 1 del enunciado y el porcentaje que pide el usuario.
*/

#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <ctype.h> // Libreria para toupper (pasar la sigla del pais a mayusculas)
#include <errno.h> 
#include <time.h> // Libreria para time(), que se usa para sembrar el generador aleatorio
#include <unistd.h> // Libreria para getopt (leer las banderas) y getpid

//Constantes que usamos para manejar sectores y paises

#define NUM_SECTORES    5   // Sectores Industria, Servicios, Consumidores, Comercio, Construccion
#define NUM_PAISES      3   // Paises ES, FR e IT

// Nombres de los sectores tal como se escriben dentro del CSV 
static const char* NOMBRE_SECTOR_CSV[NUM_SECTORES] = { "Industria", "Servicios", "Consumidores", "Comercio", "Construccion" };

// Nombres para mostrar en pantalla 
// (ya traen espacios para que las columnas queden alineadas)
static const char* NOMBRE_SECTOR_PANTALLA[NUM_SECTORES] = { "Industria    ", "Servicios    ", "Consumidores", "Comercio     ", "Construcci n" };

// Informacion que se guarda para cada pais
typedef struct {
    const char* sigla;              // Sigla del pais (ES, FR o IT)
    int max_sector[NUM_SECTORES]; // Maximo de encuestas por sector, en el mismo orden de NOMBRE_SECTOR_CSV
} PaisInfo;

// Valores maximos de la Tabla 1 del enunciado. La posicion en el arreglo es el ID_Pais (ES=0, FR=1, IT=2)
static const PaisInfo tabla_paises[NUM_PAISES] = {
    { "ES", { 1760, 1430, 2020, 1080,   290 } }, // Espana
    { "FR", { 3850, 4240, 1670, 2860, 24020 } }, // Francia
    { "IT", { 5440, 3100, 2000, 1560,  1000 } }  // Italia
};

/*
 * Muestra como se debe invocar el programa.
 */
static void mostrar_uso(const char* programa) {
    fprintf(stderr, "Uso: %s -p <ES|FR|IT> -e <porcentaje 1-100>\n", programa); // Imprime el modo de uso por la salida de errores
}

/*
 * Busca la sigla del pais en la Tabla 1.
 * Devuelve el indice del pais (que tambien es su ID_Pais) o -1 si no existe.
 */
static int buscar_pais(const char* sigla) {
    for (int indice_pais = 0; indice_pais < NUM_PAISES; indice_pais++) { // Recorre la tabla
        if (strcmp(tabla_paises[indice_pais].sigla, sigla) == 0) return indice_pais; // Si la sigla coincide, devuelve su posicion
    }
    return -1; // La sigla no esta en la tabla
}

/*
 * Convierte el texto del porcentaje en un numero entero entre 1 y 100.
 * Se usa strtol (y no atoi) porque atoi no avisa cuando el texto no es un numero: con "abc" devolveria 0 sin error.
 * Devuelve 0 si el texto es valido y -1 si no lo es.
 */

static int leer_porcentaje(const char* texto, int* porcentaje) {
    char* resto_texto; // strtol deja aqui el puntero al primer caracter que no pudo convertir
    errno = 0; // Se limpia errno para poder detectar un error de strtol (por ejemplo un numero demasiado grande)
    long valor_leido = strtol(texto, &resto_texto, 10); // Convierte el texto a numero en base 10
    if (errno != 0 || resto_texto == texto || *resto_texto != '\0') return -1; // No se convirtio nada, o sobraron caracteres como en "50x"
    if (valor_leido < 1 || valor_leido > 100) return -1; // El porcentaje debe estar entre 1 y 100
    *porcentaje = (int)valor_leido; // Devuelve el valor ya validado
    return 0; // Texto valido
}

/*
 * Devuelve un valor de sentimiento aleatorio entre 0.00 y 100.00
 */
static double sentimiento_aleatorio(void) {
    return ((double)rand() / (double)RAND_MAX) * 100.0; // rand()/RAND_MAX da un numero entre 0 y 1, y se multiplica por 100
}

/*
 * Escribe en el CSV todas las encuestas de un sector.
 * id_encuesta es un puntero para que el ID siga siendo consecutivo entre sectores (no se reinicia en cada uno).
 * Devuelve 0 si todo salio bien y -1 si fallo alguna escritura.
 */
static int escribir_sector(FILE* archivo_csv, int* id_encuesta, int id_pais, const char* sigla, int id_sector, int cantidad_encuestas) {
    for (int indice_encuesta = 0; indice_encuesta < cantidad_encuestas; indice_encuesta++) { // Una fila por cada encuesta del sector
        if (fprintf(archivo_csv, "%d,%d,%s,%s,%d,%.2f\n", *id_encuesta, id_pais, sigla,
            NOMBRE_SECTOR_CSV[id_sector], id_sector, sentimiento_aleatorio()) < 0) { // Escribe la fila, si fprintf falla (por ejemplo disco lleno)
            return -1; // Avisa al que llamo
        }
        (*id_encuesta)++; // Pasa al siguiente ID de encuesta
    }
    return 0; // Todas las filas del sector se escribieron
}

int main(int argc, char* argv[]) {
    char* pais_arg = NULL;       // Texto que el usuario escribio despues de -p
    char* porcentaje_arg = NULL; // Texto que el usuario escribio despues de -e
    int bandera_leida; // Guarda la bandera que getopt va encontrando

    while ((bandera_leida = getopt(argc, argv, "p:e:")) != -1) { // getopt lee las banderas en cualquier orden; los ':' indican que cada una lleva un valor
        switch (bandera_leida) { // Segun la bandera encontrada
        case 'p': pais_arg = optarg; break; // -p: guarda el texto del pais
        case 'e': porcentaje_arg = optarg; break; // -e: guarda el texto del porcentaje
        default: // Bandera desconocida
            mostrar_uso(argv[0]); // Muestra como se usa
            return 1; // terminar con error
        }
    }

    if (pais_arg == NULL || porcentaje_arg == NULL) { // Las dos banderas son obligatorias
        fprintf(stderr, "Error: faltan banderas.\n");
        mostrar_uso(argv[0]);
        return 1; // terminar con error
    }
    if (optind != argc) { // Si sobraron argumentos sin bandera
        fprintf(stderr, "Error: argumento inesperado '%s'.\n", argv[optind]);
        mostrar_uso(argv[0]);
        return 1; // terminar con error
    }

    int porcentaje; // Aqui queda el porcentaje ya validado
    if (leer_porcentaje(porcentaje_arg, &porcentaje) != 0) { // Valida que sea un entero entre 1 y 100
        fprintf(stderr, "Error: -e debe ser un numero entero entre 1 y 100 (se recibio '%s').\n", porcentaje_arg);
        return 1; // terminar con error
    }

    if (strlen(pais_arg) != 2) { // La sigla debe tener exactamente 2 letras
        fprintf(stderr, "Error: pais '%s' no reconocido. Use ES, FR o IT.\n", pais_arg);
        return 1; // terminar con error
    }
    char sigla[3]; // Sigla en mayusculas: 2 letras mas el '\0'
    sigla[0] = (char)toupper((unsigned char)pais_arg[0]); // Primera letra a mayuscula (acepta "es" o "ES")
    sigla[1] = (char)toupper((unsigned char)pais_arg[1]); // Segunda letra a mayuscula
    sigla[2] = '\0'; // Fin de la cadena

    int id_pais = buscar_pais(sigla); // El indice en la tabla es el ID_Pais que se escribe en el CSV
    if (id_pais < 0) { // Si la sigla no esta en la Tabla 1
        fprintf(stderr, "Error: pais '%s' no reconocido. Use ES, FR o IT.\n", pais_arg);
        return 1; // Termina con error
    }

    int cantidad_por_sector[NUM_SECTORES]; // Cuantas encuestas se generan en cada sector
    int total_encuestas = 0; // Total de encuestas del fichero
    for (int indice_sector = 0; indice_sector < NUM_SECTORES; indice_sector++) { // Recorre los 5 sectores
        cantidad_por_sector[indice_sector] = (tabla_paises[id_pais].max_sector[indice_sector] * porcentaje + 50) / 100; // Porcentaje del maximo, redondeado al entero mas cercano (el +50 hace el redondeo)
        total_encuestas += cantidad_por_sector[indice_sector]; // Suma al total
    }

    char nombre_archivo[64]; // Nombre del archivo: encuestas_XX.csv
    int caracteres_escritos = snprintf(nombre_archivo, sizeof(nombre_archivo), "encuestas_%s.csv", sigla); // Arma el nombre sin pasarse del tamano del arreglo
    if (caracteres_escritos < 0 || caracteres_escritos >= (int)sizeof(nombre_archivo)) { // Valida que el nombre cupo completo
        fprintf(stderr, "Error: no se pudo construir el nombre del fichero.\n");
        return 1; // Termina con error
    }

    printf("Generando fichero %s para pais %s con %d%% de encuestas\n", nombre_archivo, sigla, porcentaje); // Aviso de inicio

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid()); // Siembra el generador aleatorio; se mezcla el pid para que dos generadores lanzados en el mismo segundo no den los mismos numeros

    FILE* archivo_csv = fopen(nombre_archivo, "w"); // Crea el fichero (o lo reemplaza si ya existia)
    if (archivo_csv == NULL) { // Si no se pudo crear
        perror("Error al crear el fichero CSV"); // Muestra el motivo
        return 1; // Termina con error
    }

    int hubo_error = 0; // Se pone en 1 si alguna escritura falla
    if (fprintf(archivo_csv, "ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento\n") < 0) hubo_error = 1; // Escribe el encabezado del CSV

    int id_encuesta = 1; // El primer ID de encuesta es 1
    for (int indice_sector = 0; indice_sector < NUM_SECTORES && !hubo_error; indice_sector++) { // Recorre los sectores mientras no haya errores
        if (escribir_sector(archivo_csv, &id_encuesta, id_pais, sigla, indice_sector, cantidad_por_sector[indice_sector]) != 0) hubo_error = 1; // Escribe las encuestas del sector
    }

    if (fclose(archivo_csv) != 0) hubo_error = 1; // fclose tambien puede fallar: es cuando se escribe lo que quedaba pendiente en el buffer del archivo
    if (hubo_error) { // Si algo fallo en la escritura
        fprintf(stderr, "Error al escribir el fichero %s.\n", nombre_archivo);
        remove(nombre_archivo); // No se deja un CSV incompleto en disco
        return 1; // Termina con error
    }

    printf("Maximos por sector:\n"); // Encabezado del resumen
    for (int indice_sector = 0; indice_sector < NUM_SECTORES; indice_sector++) { // Una linea por sector
        printf("  %s : %d/%d encuestas (%d%% del maximo)\n", NOMBRE_SECTOR_PANTALLA[indice_sector],
            cantidad_por_sector[indice_sector], tabla_paises[id_pais].max_sector[indice_sector], porcentaje); // Sector, cantidad generada, maximo y porcentaje
    }
    printf("Total encuestas: %d\n", total_encuestas); // Total de encuestas generadas
    printf("Fichero CSV generado\n"); // Aviso de fin
    return 0; // Termina sin errores
}