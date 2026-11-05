
#define _POSIX_C_SOURCE 200809L

/*
Autores: Juan Camilo Maldonado, Samuel Zeudec , Daniel Plazas
Fichero: servidorISE.c
Objetivo del fichero: Implementacion del Control Proceso Principal ISE. Toda la logica real
(hilos, semaforos , pipe nominal y calculo del ISE).
*/


#include <stdio.h>
#include <stdlib.h> //Libreria para funciones como atoi, calloc , free y otras funciones relacionada con memoria
#include <string.h>
#include <unistd.h>
#include <fcntl.h> //Librearia para trabajar con archivos como O_RDWR
#include <errno.h> //Libreria para saber si ocurrio algun error
#include <time.h>
#include <poll.h> //libreria que permite ver si hay datos disponibles para leer en un pipe
#include <sys/stat.h> //Librearia para trabajar con archivos y pipes
#include <sys/types.h> //para el tipo pid_t del proceso hijo
#include <sys/wait.h> //para que el padre espere al hijo con waitpid
#include <pthread.h> //Libreria para manejar y crear hilos
#include <semaphore.h>

#include "servidorISE.h"

//Aqui van las constantes que usa el archivo parea manejar el numero maximo de agentes y sectores para el calculo ISE

#define MAX_AGENTES    4   //Maximo numero de agentes que se pueden conectar al servidor
#define NUM_SECTORES   5  //Sectores Industria, Servicios, Consumidores, Comercio, Construccion


static const double PESOS_SECTOR[NUM_SECTORES] = { 0.40, 0.30, 0.20, 0.05, 0.05 }; //peso de cada sector para el calculo del ISE
static const char* NOMBRE_SECTOR[NUM_SECTORES] = { "Industria", "Servicios", "Consumidores", "Comercio", "Construccion" }; //nombres reportes

//Informacion que se guarda para cada agente

typedef struct {
	MensajeEncuesta* buffer; //es un buffer circular para guardar los mensajes que llegan de cada agente
	int tamanio; //tamano de lo que cabe en el buffer
	int head_receptor; // posicion donde el receptor escribe
	int tail_consumidor;//posicion donde el consumidor va a leer

	sem_t espacios_libres; //cuenta los huecos libres en el buffer circular
	sem_t items_disponibles; //cuenta los mensajes por leer

	char pais[ISE_MAX_PAIS_TAM];
	int ocupado; //pone 1 si el slot esta ocupado por un pais, 0 si esta libre
	int impresion_conexion; // pone 1 si ya se imprimio la conexion del pais, 0 si no

	double suma_sector[NUM_SECTORES];
	long cantidad_sector[NUM_SECTORES];
	long total_encuestas;
} AgenteSlot;


//Estas variables se usan solamente dentro de este archivo

static AgenteSlot agentes[MAX_AGENTES];
static int hilos_max;
static int tam_buffer;
static int tiempo_limite;
static char nombre_pipe[256];



//aqui el mutex prrotege la asignacion de slots, para no asiganar dos slots al mismo pais
static pthread_mutex_t mutex_asignacion = PTHREAD_MUTEX_INITIALIZER;
//es la bandera que indica que el tiempo del servidor termino (o que hubo error y hay q cerrar)
static int tiempo_terminado = 0; //0 activo , 1 si hay q cerrar
static pthread_mutex_t mutex_tiempo = PTHREAD_MUTEX_INITIALIZER; // mutex que protege la bandera tiempo_terminado




//devuelve 1 si el servidor debe cerrar o 0 si esta activo
static int hay_que_cerrar(void) {
	pthread_mutex_lock(&mutex_tiempo); // entra a la seccion critica de la bandera
	int valor_bandera = tiempo_terminado; // copia el valor de la bandera
	pthread_mutex_unlock(&mutex_tiempo); // sale de la seccion critica
	return valor_bandera; // devuelve la copia leida
}

//marca que el servidor debe cerrar (por que se acabo el tiempo o por un error)
static void marcar_cierre(void) {
	pthread_mutex_lock(&mutex_tiempo); // entra a la seccion critica de la bandera
	tiempo_terminado = 1; // marca que el servidor debe cerrar
	pthread_mutex_unlock(&mutex_tiempo); // sale de la seccion critica
}

//espera que haya espacio o datos en el POSIX semaforo
//devuelve 0 si se libero a tiempo, -1 si hubo error o timeout

static int esperar_con_limite(sem_t* semaforo) { //aqui recibimos el valor que queremos esperar
	while (!hay_que_cerrar()) {
		struct timespec espera_limite; // guarda el limite o tiempo maximo que se espera por el semaforo que se pasa como parametro
		if (clock_gettime(CLOCK_REALTIME, &espera_limite) != 0) return -1; // lee el reloj, si falla no se puede calcular el limite
		espera_limite.tv_sec += 1; // el limite espera maximo 1 segundo, si no se libera el semaforo en ese tiempo se retorna -1
		int espera_estado = sem_timedwait(semaforo, &espera_limite); // espera el semaforo como maximo 1 segundo
		if (espera_estado == 0) return 0; //aqui se libero el semaforo a tiempo, se retorna 0
		if (errno == EINTR) continue; // aqui se interrumpio la espera por una se al, se vuelve a esperar
		if (errno != ETIMEDOUT) return -1; // aqui cualquier error distinto de timeout es un error real
	}
	return -1; // el tiempo del servidor termino
}

/*
Funcion que busca un slot libre para un pais nuevo, o devuelve el slot ya asignado a ese pais si ya existe.
si el pais no existe y no hay slots libres, busca un espacio libre para asignarlo
*/

static int obtener_slot(const char* pais) {
	pthread_mutex_lock(&mutex_asignacion); //Bloqueamos el mutex para evitar que dos hilos accedan a la misma variable al mismo tiempo
	int primer_slot_libre = -1; //gaurda el indice del primer slot libre encontrado
	for (int indice_slot = 0; indice_slot < hilos_max; indice_slot++) { //recorre los slots habilitadoos
		if (agentes[indice_slot].ocupado && strcmp(agentes[indice_slot].pais, pais) == 0) { //mirar si el slot ya partenece al pais
			pthread_mutex_unlock(&mutex_asignacion); //desbloqueamos el mutex antes de retornar el indice del slot
			return indice_slot;
		}
		if (!agentes[indice_slot].ocupado && primer_slot_libre == -1) primer_slot_libre = indice_slot; //nos recuerda el primer slot libre encontrado, para asignarlo al pais si no existe
	}
	if (primer_slot_libre != -1) { //si encontramos un slot libre se asigna al pais nuevo
		strncpy(agentes[primer_slot_libre].pais, pais, ISE_MAX_PAIS_TAM - 1); //Se copia el nombre del pais
		agentes[primer_slot_libre].ocupado = 1;
	}
	pthread_mutex_unlock(&mutex_asignacion); //desbloquear el mutex antes de retornar el indice del slot libre
	return primer_slot_libre;//retorna el indice del slot libre encontrado, o -1 si no hay slots libres
}


/*
El hilo consumidor: extrae los mensajes del buffer circular de su respectivo agente
para procesar los datos. Si recibe el mensaje de fin, imprime el reporte y termina.
*/
typedef struct { int indice; } ArgConsumidor;
static void* hilo_consumidor(void* argumento_consumidor) {
	// Obtiene el indice del slot que le fue asignado a este hilo consumidor especifico
	int indice_slot = ((ArgConsumidor*)argumento_consumidor)->indice;
	// Obtiene un puntero directo a la estructura de datos (slot) del agente correspondiente
	AgenteSlot* slot = &agentes[indice_slot];



	//mientras el servidor no haya terminado su tiempo de ejecucion, el hilo consumidor sigue extrayendo mensajes del buffer circular
	while (1) {
		//si el limite de tiempo se acabo o hubo error, se sale del bucle y termina el hilo consumidor
		if (esperar_con_limite(&slot->items_disponibles) != 0) break;
		// Copia el mensaje que se encuentra en la posicion actual del consumidor (tail) dentro del buffer circular
		MensajeEncuesta mensaje = slot->buffer[slot->tail_consumidor];
		// Actualiza la posicion del tail de manera circular usando modulo para que vuelva al inicio si llega al tope
		slot->tail_consumidor = (slot->tail_consumidor + 1) % slot->tamanio;
		// Incrementa el semaforo de espacios libres, avisando al hilo receptor que ya hay un hueco disponible en el buffer
		sem_post(&slot->espacios_libres);

		//mira si el mensaje recibido es de tipo fin, en cuyo caso imprime el reporte parcial y termina el hilo consumidor
		if (mensaje.tipo == MSG_FIN) {
			// Imprime el encabezado del reporte parcial para este pais
			printf("\n[Reporte parcial - Agente %s]\n", slot->pais);
			// Recorre todos los sectores economicos definidos para calcular estadisticas individuales
			for (int indice_sector = 0; indice_sector < NUM_SECTORES; indice_sector++) {
				// Calcula el promedio de sentimiento del sector; si no hubo encuestas, asigna 0.0 para evitar division por cero
				double promedio = slot->cantidad_sector[indice_sector] > 0 ? slot->suma_sector[indice_sector] / slot->cantidad_sector[indice_sector] : 0.0;
				// Imprime en pantalla las encuestas totales y el sentimiento promedio de cada sector
				printf(" %-13s: %6ld encuestas, sentimiento promedio %.2f%%\n", NOMBRE_SECTOR[indice_sector], slot->cantidad_sector[indice_sector], promedio);
			}
			// Imprime la cantidad total de encuestas acumuladas por este agente antes de cerrar
			printf(" Total %s: %ld encuestas recibidas.\n", slot->pais, slot->total_encuestas);
			// Rompe el bucle porque el agente ya termino su trabajo y este hilo consumidor debe expirar
			break;
		}
		// Evalua si el mensaje recibido corresponde a una encuesta valida (dato numerico de sentimiento)
		else if (mensaje.tipo == MSG_DATO) {
			// Valida que el identificador del sector se encuentre dentro de los limites permitidos
			if (mensaje.id_sector >= 0 && mensaje.id_sector < NUM_SECTORES) {
				// Acumula la calificacion de sentimiento en el sector economico correspondiente
				slot->suma_sector[mensaje.id_sector] += mensaje.sentimiento;
				// Incrementa en uno el contador de encuestas recibidas para ese sector en particular
				slot->cantidad_sector[mensaje.id_sector]++;
				// Incrementa en uno el contador del total general de encuestas recibidas de este agente
				slot->total_encuestas++;
			}
		}
	}
	// Retorna NULL al finalizar la ejecucion del hilo consumidor
	return NULL;
}

/*
El hilo receptor: se encarga de leer continuamente el pipe nominal, identificar a
que agente pertenece cada mensaje y almacenarlo en su buffer circular correspondiente.
*/
static void* hilo_receptor(void* argumento_sin_usar) {
	// Evita advertencias de compilacion por variable no utilizada, cumpliendo la firma estandar de pthreads
	(void)argumento_sin_usar;
	// Abre el pipe nominal en modo lectura y escritura para evitar que devuelva EOF prematuro si ningun cliente escribe
	int descriptor_pipe = open(nombre_pipe, O_RDWR);

	// Valida si ocurrio un error critico al intentar abrir el archivo FIFO del pipe
	if (descriptor_pipe < 0) {
		perror("Error al abrir el pipe nominal");
		// Activa la bandera de cierre general para avisarle al resto del programa que debemos abortar
		marcar_cierre();
		return NULL;
	}

	// Bucle principal del receptor que se ejecuta mientras no se ordene el cierre del servidor
	while (!hay_que_cerrar()) {
		// Configura una estructura pollfd para monitorear el descriptor de archivo del pipe ante eventos de lectura (POLLIN)
		struct pollfd seguir_pipe = { .fd = descriptor_pipe, .events = POLLIN };
		// Ejecuta la funcion poll con un tiempo limite (timeout) de 500 milisegundos para no bloquearse indefinidamente
		int eventos_listos = poll(&seguir_pipe, 1, 500);
		// Evalua si la funcion poll arrojo un error de ejecucion
		if (eventos_listos < 0) {
			// Si el error no fue causado por una interrupcion de senal del sistema, imprime el error por consola
			if (errno != EINTR) perror("Error en poll");
			continue;
		}
		// Si el resultado es 0 significa que expiro el tiempo limite de 500ms sin datos nuevos, asi que reitera el bucle
		if (eventos_listos == 0) continue;

		// Declara la estructura local para almacenar el mensaje de encuesta que viene desde el pipe
		MensajeEncuesta mensaje;
		// Lee los bytes binarios provenientes del pipe nominal y los deposita en la estructura del mensaje
		ssize_t bytes_leidos = read(descriptor_pipe, &mensaje, sizeof(mensaje));
		// Valida si la lectura del archivo fallo
		if (bytes_leidos < 0) {
			// Ignora errores por interrupciones de senales y reporta cualquier otra falla real de lectura
			if (errno != EINTR) perror("Error al leer el pipe nominal");
			continue;
		}
		// Si los bytes leidos no coinciden exactamente con el tamano del struct esperado, descarta el paquete parcial
		if (bytes_leidos != (ssize_t)sizeof(mensaje)) continue;

		// Llama a la funcion auxiliar para ubicar o crear el slot correspondiente al pais que envio el mensaje
		int indice_slot = obtener_slot(mensaje.pais);
		// Valida si no hay slots disponibles (retorna -1)
		if (indice_slot == -1) {
			// Si el mensaje rechazado es de conexion inicial, imprime un aviso indicando que se alcanzo el limite maximo
			if (mensaje.tipo == MSG_CONEXION) {
				fprintf(stderr, "Aviso: se alcanzo el maximo de %d Agentes, se rechaza al Agente %s\n", hilos_max, mensaje.pais);
			}
			continue;
		}
		// Obtiene un puntero al slot exclusivo asignado al agente emisor
		AgenteSlot* slot = &agentes[indice_slot];
		// Si el mensaje es de conexion y aun no se habia impreso su mensaje de bienvenida, lo muestra en consola
		if (mensaje.tipo == MSG_CONEXION && !slot->impresion_conexion) {
			printf("Conexion establecida con Agente: %s\n", mensaje.pais);
			// Marca la bandera en 1 para evitar imprimir multiples veces el aviso de conexion de este mismo agente
			slot->impresion_conexion = 1;
			continue;
		}

		// Espera de forma limitada a que el semaforo de espacios libres del buffer del agente tenga un hueco disponible
		if (esperar_con_limite(&slot->espacios_libres) != 0) continue;
		// Almacena el mensaje recibido dentro del buffer circular en la posicion que marca el head actual
		slot->buffer[slot->head_receptor] = mensaje;
		// Actualiza el indice head de forma circular aplicando modulo para mantenerlo dentro de la capacidad del buffer
		slot->head_receptor = (slot->head_receptor + 1) % slot->tamanio;
		// Incrementa el semaforo de items disponibles para notificarle al hilo consumidor que ya puede leer un nuevo dato
		sem_post(&slot->items_disponibles);
	}
	// Cierra el descriptor de archivo del pipe nominal al salir del bucle de recepcion
	close(descriptor_pipe);
	return NULL;
}

/*
El hilo temporizador: controla la duracion total de ejecucion del servidor mediante
intervalos de un segundo, permitiendo una respuesta agil ante cierres forzosos o errores.
*/
static void* hilo_temporizador(void* argumento_no_usado) {
	// Evita advertencias por parametro no utilizado
	(void)argumento_no_usado;
	// Bucle que cuenta segundo a segundo hasta cumplir el tiempo limite configurado, validando siempre si se ordeno un cierre anticipado
	for (int segundos_transcurridos = 0; segundos_transcurridos < tiempo_limite && !hay_que_cerrar(); segundos_transcurridos++) {
		// Suspende la ejecucion de este hilo durante exactamente 1 segundo
		sleep(1);
	}
	// Una vez transcurrido el tiempo limite, invoca la funcion para marcar el cierre general del servidor
	marcar_cierre();
	return NULL;
}

/*
Libera la memoria asignada y destruye los semaforos correspondientes a los slots inicializados.
*/
static void liberar_slots(int cantidad_slots_inicializados) {
	// Recorre unicamente la cantidad de slots que alcanzaron a ser configurados exitosamente
	for (int indice_slot = 0; indice_slot < cantidad_slots_inicializados; indice_slot++) {
		// Destruye de forma segura el semaforo POSIX que controlaba los espacios libres del buffer
		sem_destroy(&agentes[indice_slot].espacios_libres);
		// Destruye de forma segura el semaforo POSIX que controlaba los items listos para consumir
		sem_destroy(&agentes[indice_slot].items_disponibles);
		// Libera la memoria dinamica reservada previamente para el buffer circular del agente
		free(agentes[indice_slot].buffer);
		// Asigna NULL al puntero liberado para prevenir errores de tipo dangling pointer
		agentes[indice_slot].buffer = NULL;
	}
}

/*
Inicializa los slots de los agentes reservando memoria dinamica para los buffers
y configurando los semaforos de control de concurrencia.
*/
static int inicializar_slots(void) {
	// Itera tantas veces como el numero maximo de hilos/agentes permitidos por parametro
	for (int indice_slot = 0; indice_slot < hilos_max; indice_slot++) {
		// Reserva memoria dinamica inicializada en ceros para el buffer circular de este agente usando calloc
		agentes[indice_slot].buffer = calloc(tam_buffer, sizeof(MensajeEncuesta));
		// Verifica si la asignacion de memoria fallo (retorno NULL)
		if (agentes[indice_slot].buffer == NULL) {
			perror("Error al reservar memoria para el buffer");
			// Libera los recursos de los slots que si habian alcanzado a inicializarse antes del fallo
			liberar_slots(indice_slot);
			return -1;
		}
		// Asigna el tamano maximo del buffer dentro de la estructura del slot
		agentes[indice_slot].tamanio = tam_buffer;
		agentes[indice_slot].head_receptor = 0;
		agentes[indice_slot].tail_consumidor = 0;
		// Marca el slot como desocupado inicialmente
		agentes[indice_slot].ocupado = 0;
		// Inicializa en 0 la bandera de impresion de conexion
		agentes[indice_slot].impresion_conexion = 0;
		agentes[indice_slot].total_encuestas = 0;
		// Limpia con ceros el arreglo de sumas acumuladas por sector economico
		memset(agentes[indice_slot].suma_sector, 0, sizeof(agentes[indice_slot].suma_sector));
		// Limpia con ceros el arreglo de contadores de encuestas por sector economico
		memset(agentes[indice_slot].cantidad_sector, 0, sizeof(agentes[indice_slot].cantidad_sector));

		// Inicializa el semaforo de espacios libres indicando que al inicio todos los espacios del buffer estan disponibles
		if (sem_init(&agentes[indice_slot].espacios_libres, 0, tam_buffer) != 0) {
			perror("Error al inicializar el semaforo espacios_libres");
			// Libera el buffer dinamico de este slot en particular
			free(agentes[indice_slot].buffer);
			agentes[indice_slot].buffer = NULL;
			// Libera todos los slots anteriores que ya se habian procesado correctamente
			liberar_slots(indice_slot);
			return -1;
		}
		// Inicializa el semaforo de items disponibles en 0, pq al arrancar no hay mensajes listos para consumir
		if (sem_init(&agentes[indice_slot].items_disponibles, 0, 0) != 0) {
			perror("Error al inicializar el semaforo items_disponibles");
			// Destruye el semaforo de espacios libres que si se habia creado en la instruccion anterior
			sem_destroy(&agentes[indice_slot].espacios_libres);
			// Libera el buffer de este slot
			free(agentes[indice_slot].buffer);
			agentes[indice_slot].buffer = NULL;
			// Libera los recursos de los slots anteriores ante el fallo actual
			liberar_slots(indice_slot);
			return -1;
		}
	}
	return 0;
}

/*
Genera el informe consolidado global recopilando la informacion de todos los agentes activos,
calculando los promedios por sector y aplicando los factores de ponderacion del ISE.
*/
static void imprimir_resumen_general(void) {
	// Declara e inicializa en ceros el arreglo para acumular la suma global de sentimientos por sector
	double suma_global_sector[NUM_SECTORES] = { 0 };
	// Declara e inicializa en ceros el arreglo para acumular la cantidad global de encuestas por sector
	long cantidad_global_sector[NUM_SECTORES] = { 0 };

	// Imprime el encabezado visual del reporte consolidado final del ISE en la consola
	printf("\n ------------------------- RESUMEN DEL ISE -----------------------------------\n");
	// Recorre todos los slots posibles controlados por el servidor
	for (int indice_slot = 0; indice_slot < hilos_max; indice_slot++) {
		// Si el slot no tiene un agente asignado (ocupado o libre) , lo salta con continue
		if (!agentes[indice_slot].ocupado) continue;
		// Recorre cada uno de los sectores economicos para sumar los acumuladores parciales de cada agente al consolidado global
		for (int indice_sector = 0; indice_sector < NUM_SECTORES; indice_sector++) {
			suma_global_sector[indice_sector] += agentes[indice_slot].suma_sector[indice_sector];
			cantidad_global_sector[indice_sector] += agentes[indice_slot].cantidad_sector[indice_sector];
		}
	}

	// Inicializa en CEROS la variable donde se calculara el valor final del indicador ISE global
	double indicador_ise = 0.0;
	// Recorre cada sector para calcular su promedio global ponderado en funcion de los pesos definidos
	for (int indice_sector = 0; indice_sector < NUM_SECTORES; indice_sector++) {
		// Calcula el promedio del sector validando que la cantidad global sea mayor a cero para evitar divisiones por cero
		double promedio_sector = cantidad_global_sector[indice_sector] > 0 ? suma_global_sector[indice_sector] / cantidad_global_sector[indice_sector] : 0.0;
		// Imprime en pantalla el nombre del sector, su peso porcentual asignado y el promedio de sentimiento obtenido
		printf(" %-13s (peso %4.0f%%): promedio %.2f%%\n", NOMBRE_SECTOR[indice_sector], PESOS_SECTOR[indice_sector] * 100, promedio_sector);
		// Suma el producto del promedio del sector por su peso porcentual correspondiente para construir el indicador ISE global
		indicador_ise += PESOS_SECTOR[indice_sector] * promedio_sector;
	}

	// Imprime el valor final del indicador ISE global calculado con dos decimales de precision
	printf("\n Indicador ISE global: %.2f%%\n", indicador_ise);

	// Declara un puntero para almacenar el texto de la recomendacion economica resultante
	const char* recomendacion;
	// Aplica la regla de negocio: si el indicador es mayor a 50.0, la recomendacion es VENDER
	if (indicador_ise > 50.0) recomendacion = "VENDER";
	// Si el indicador es menor a 50.0, la recomendacion economica es COMPRAR
	else if (indicador_ise < 50.0) recomendacion = "COMPRAR";
	// Si el indicador es exactamente igual a 50.0, la recomendacion es NEUTRO
	else recomendacion = "NEUTRO";

	// Imprime recomendaciones al final
	printf(" Recomendacion: %s\n", recomendacion);
	printf("===============================================================\n");
}

/*
Realiza la limpieza general de los recursos del proceso hijo antes de finalizar.
*/
static void limpiar_recursos(void) {
	// Libera los buffers y destruye los semaforos invocando la funcion auxiliar de liberacion de slots
	liberar_slots(hilos_max);
}

/*
Procesa los argumentos recibidos por linea de comandos utilizando la funcion getopt.
*/
int servidor_parsear_argumentos(int argc, char* argv[]) {
	// Declara la variable para almacenar la opcion leida por getopt
	int bandera_leida;
	// Declara banderas locales de control para verificar que se hayan recibido todos los parametros obligatorios
	int recibio_tam_buffer = 0, recibio_nombre_pipe = 0, recibio_tiempo = 0, recibio_cantidad_agentes = 0;

	// Bucle que analiza cada opcion de la linea de comandos mediante getopt utilizando los flags esperados (-b, -p, -t, -a)
	while ((bandera_leida = getopt(argc, argv, "b:p:t:a:")) != -1) {
		// Estructura de seleccion para procesar segun la letra de la bandera encontrada
		switch (bandera_leida) {
			// Opcion -b: asigna el tamano del buffer convirtiendo el argumento de texto a entero con atoi
		case 'b': tam_buffer = atoi(optarg); recibio_tam_buffer = 1; break;
			// Opcion -p: copia de forma segura el nombre del pipe recibido hacia la variable global correspondiente
		case 'p': strncpy(nombre_pipe, optarg, sizeof(nombre_pipe) - 1); recibio_nombre_pipe = 1; break;
			// Opcion -t: asigna el tiempo limite del servidor convirtiendo el argumento de texto a entero
		case 't': tiempo_limite = atoi(optarg); recibio_tiempo = 1; break;
			// Opcion -a: asigna el numero maximo de agentes permitidos convirtiendo el argumento a entero
		case 'a': hilos_max = atoi(optarg); recibio_cantidad_agentes = 1; break;
			// Caso por defecto si se ingresa una bandera desconocida: imprime la ayuda de uso y retorna un codigo de error (1)
		default:
			fprintf(stderr, "Uso: %s -b <tamB> -p <PipeN> -t <time> -a <hilos>\n", argv[0]);
			return 1;
		}
	}

	// Verifica si falto alguno de los argumentos obligatorios requeridos por el programa
	// Si alguna bandera obligatoria no fue recibida, imprime un mensaje de error indicando el uso correcto del programa y retorna 1
	if (!recibio_tam_buffer || !recibio_nombre_pipe || !recibio_tiempo || !recibio_cantidad_agentes) {
		// Imprime un mensaje de error por la salida de errores estandar indicando el uso correcto del programa
		fprintf(stderr, "Error: faltan banderas. Uso: %s -b <tamB> -p <PipeN> -t <time> -a <hilos>\n", argv[0]);
		return 1;
	}
	// Retorna 0 indicando que todos los parametros obligatorios fueron parseados de forma correcta
	return 0;
}

/*
Valida que los parametros ingresados se encuentren dentro de los rangos validos permitidos.
*/
int servidor_validar_argumentos(void) {
	// Valida que el tiempo limite configurado se encuentre estrictamente entre 30 y 120 segundos inclusive
	if (tiempo_limite < 30 || tiempo_limite > 120) {
		fprintf(stderr, "Error: -t debe estar entre 30 y 120 segundos.\n");
		return 1;
	}
	// Aqui se valida que la cantidad maxima de hilos/agentes este entre 1 y el limite global establecido (MAX_AGENTES)
	if (hilos_max < 1 || hilos_max > MAX_AGENTES) {
		fprintf(stderr, "Error: -a debe estar entre 1 y %d.\n", MAX_AGENTES);
		return 1;
	}
	// Aqui se valida que el tamano del buffer sea estrictamente mayor a 0 para garantizar capacidad de almacenamiento
	if (tam_buffer < 1) {
		fprintf(stderr, "Error: -b debe ser mayor a 0.\n");
		return 1;
	}
	// Retorna 0 indicando que todos los argumentos cumplen con las validaciones de rango establecidas
	return 0;
}

/*
Funcion principal del proceso hijo: inicializa la estructura, crea los hilos de trabajo,
controla el tiempo limite de ejecucion y procesa el cierre ordenado del servidor.
*/
static int servidor_hijo(void) {
	// Llama a la funcion de inicializacion de slots y verifica si ocurrio algun fallo critico
	if (inicializar_slots() != 0) {
		return 1;
	}

	// Imprime en consola los mensajes informativos de inicio de operacion del proceso principal del servidor
	printf("Control Proceso Principal ISE iniciado.\n");
	printf(" Pipe nominal : %s\n", nombre_pipe);
	printf(" Tiempo limite: %d segundos\n", tiempo_limite);
	printf(" Agentes max  : %d\n", hilos_max);
	printf(" Tam. buffer  : %d registros por agente\n", tam_buffer);
	printf("Esperando conexiones de Agentes...\n");

	// Declara los identificadores de tipo pthread_t para los hilos receptor y temporizador
	pthread_t id_hilo_receptor, id_hilo_temporizador;
	// Declara un arreglo de identificadores pthread_t para almacenar los IDs de los hilos consumidores segun el maximo de agentes
	pthread_t ids_hilos_consumidores[MAX_AGENTES];
	// Declara un arreglo de estructuras para pasar los argumentos correspondientes a cada hilo consumidor
	ArgConsumidor argumentos_consumidores[MAX_AGENTES];

	// Crea el hilo receptor encargado de leer del pipe y verifica si la creacion fallo
	if (pthread_create(&id_hilo_receptor, NULL, hilo_receptor, NULL) != 0) {
		fprintf(stderr, "Error al crear el hilo receptor\n");
		limpiar_recursos();
		return 1;
	}

	// Crea el hilo temporizador encargado de controlar el tiempo limite y verifica si ocurrio un error
	if (pthread_create(&id_hilo_temporizador, NULL, hilo_temporizador, NULL) != 0) {
		fprintf(stderr, "Error al crear el hilo temporizador\n");
		// Activa el cierre general para avisarle al hilo receptor que debe terminar
		marcar_cierre();
		// Espera mediante join a que el hilo receptor concluya su ejecucion
		pthread_join(id_hilo_receptor, NULL);
		limpiar_recursos();
		return 1;
	}

	// Declara e inicializa un contador para registrar cuantos hilos consumidores se lograron crear exitosamente
	int consumidores_creados = 0;
	// Ciclo for para lanzar un hilo consumidor independiente por cada agente habilitado hasta llegar a hilos_max
	for (int indice_slot = 0; indice_slot < hilos_max; indice_slot++) {
		// Asigna el indice del slot correspondiente dentro de la estructura de argumentos del consumidor
		argumentos_consumidores[indice_slot].indice = indice_slot;
		// Crea el hilo consumidor invocando pthread_create y pasando su respectiva estructura de argumentos
		if (pthread_create(&ids_hilos_consumidores[indice_slot], NULL, hilo_consumidor, &argumentos_consumidores[indice_slot]) != 0) {
			fprintf(stderr, "Error al crear el hilo consumidor %d\n", indice_slot);
			// Activa la bandera de cierre para notificar a los demas hilos que deben abortar
			marcar_cierre();
			break;
		}
		consumidores_creados++;
	}

	// El hilo principal del proceso hijo se bloquea esperando a que el hilo temporizador cumpla el tiempo limite establecido
	pthread_join(id_hilo_temporizador, NULL);

	// Si durante la creacion no se alcanzaron a crear todos los consumidores requeridos, ejecuta un cierre ordenado de emergencia
	if (consumidores_creados < hilos_max) {
		pthread_join(id_hilo_receptor, NULL);
		for (int indice_slot = 0; indice_slot < consumidores_creados; indice_slot++) {
			pthread_join(ids_hilos_consumidores[indice_slot], NULL);
		}
		limpiar_recursos();
		return 1;
	}

	// Imprime un mensaje informando que el tiempo limite del servidor expiro y se procede a cerrar la recepcion de datos
	printf("\nTiempo del servidor finalizado. Cerrando recepcion...\n");

	// Espera mediante pthread_join a que el hilo receptor termine por completo sus operaciones pendientes
	pthread_join(id_hilo_receptor, NULL);
	// Espera mediante pthread_join a que cada uno de los hilos consumidores creados finalice su ejecucion
	for (int indice_slot = 0; indice_slot < hilos_max; indice_slot++) {
		pthread_join(ids_hilos_consumidores[indice_slot], NULL);
	}

	// Una vez finalizados todos los hilos concurrentes, invoca la funcion para calcular y desplegar el resumen general consolidado
	imprimir_resumen_general();

	// Libera los recursos de memoria dinamica y semaforos utilizados
	limpiar_recursos();
	// Retorna 0 indicando que el proceso hijo finalizo de manera completamente exitosa
	return 0;
}

/*
Funcion de coordinacion general: crea el pipe nominal, genera el proceso hijo encargado
de la logica del servidor, espera su conclusion y elimina los recursos del sistema.
*/
int servidor_ejecutar(void) {
	// Crea el archivo FIFO del pipe nominal con permisos 0666; si ya existe de una corrida previa (EEXIST), lo ignora y reutiliza
	//Si la creacion del pipe falla por un error distinto a EEXIST, imprime el error y retorna 1
	if (mkfifo(nombre_pipe, 0666) < 0 && errno != EEXIST) {
		perror("Error al crear el pipe nominal");
		return 1;
	}

	//fflush para asegurar que todo lo que se haya impreso hasta ahora se muestre antes de crear el proceso hijo
	fflush(stdout);
	// Crea un proceso hijo mediante fork duplicando la ejecucion actual antes de levantar hilos
	pid_t pid_hijo = fork();
	// Valida si la llamada al sistema fork arrojo un error negativo
	if (pid_hijo < 0) {
		perror("Error al crear el proceso hijo");
		// Elimina el archivo FIFO creado en el disco antes de abortar
		unlink(nombre_pipe);
		return 1;
	}

	// Bloque de codigo ejecutado unicamente por el proceso hijo (porque fork retorna 0 al proceso hijo)
	//Si el proceso es el hijo, ejecuta la funcion principal del servidor y termina con su codigo de salida
	if (pid_hijo == 0) {
		// Invoca la funcion principal del servidor dentro del hijo y almacena su codigo de salida
		int codigo_salida_hijo = servidor_hijo();
		// Vacia nuevamente el buffer de salida estandar para garantizar que se imprima todo el texto acumulado
		fflush(stdout);
		// Termina la ejecucion del proceso hijo inmediatamente utilizando la funcion _exit y pasando el codigo de salida correspondiente
		_exit(codigo_salida_hijo);
	}

	// Mensaje impreso por el proceso padre indicando que se encuentra en espera de la finalizacion del proceso hijo
	printf("Proceso padre: esperando a que finalice el proceso hijo (PID %d)...\n", (int)pid_hijo);
	// Declara la variable para almacenar el estado de salida devuelto por el hijo al finalizar
	int estado_hijo = 0;
	// Bucle que suspende al proceso padre mediante waitpid hasta que el hijo termine, reintentando si es interrumpido por una senal
	while (waitpid(pid_hijo, &estado_hijo, 0) < 0) {
		// Si el error ocurrido es diferente a una interrupcion por senial , se considera un error real y aborta
		if (errno != EINTR) {
			perror("Error al esperar al proceso hijo");
			unlink(nombre_pipe);
			return 1;
		}
	}

	// El proceso padre procede a borrar definitivamente el archivo FIFO del sistema de archivos al concluir toda la ejecucion
	if (unlink(nombre_pipe) != 0) {
		perror("Error al borrar el pipe nominal");
	}

	// Verifica si el proceso hijo termino de forma normal mediante macro WIFEXITED y si su codigo de estado de salida fue 0
	if (WIFEXITED(estado_hijo) && WEXITSTATUS(estado_hijo) == 0) {
		printf("Exito: el proceso ISE finalizo correctamente.\n");
		return 0;
	}
	// Imprime un mensaje de error si el proceso hijo no concluyo de forma exitosa
	fprintf(stderr, "Error: el proceso hijo no finalizo correctamente.\n");
	return 1;
}
